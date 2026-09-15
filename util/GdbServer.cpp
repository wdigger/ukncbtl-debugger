// util/GdbServer.cpp

#include "stdafx.h"
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_t;
#define INVALID_SOCKET_VALUE INVALID_SOCKET
#define CloseSocket closesocket
#else
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
typedef int socket_t;
#define INVALID_SOCKET_VALUE (-1)
#define CloseSocket close
#endif

#include "Emulator.h"
#include "emubase/Emubase.h"
#include "GdbServer.h"

//////////////////////////////////////////////////////////////////////

namespace {

// The subset of the remote protocol implemented here, and why each of
// these is in it:
//
//   ?              what stopped the program -- gdb asks first thing
//   g, G           all registers at once
//   p, P           one register; gdb prefers these where a stub has them
//   m, M           memory
//   c, s           continue, single step
//   Z0, z0         breakpoints, so that gdb does not have to write BPT
//                  into memory itself (which it would otherwise do, and
//                  which would be wrong for anything in ROM)
//   D, k           detach, kill
//   H, qC, qfThreadInfo, qsThreadInfo
//                  threads.  There is one processor and no threads, but
//                  gdb asks, and an answer it understands is shorter
//                  than the fallbacks it tries when it gets none.
//   qSupported, qAttached
//                  the opening negotiation
//
// Everything else gets an empty reply, which is the protocol's way of
// saying "not implemented" and is always a valid answer.

socket_t g_socket = INVALID_SOCKET_VALUE;

// Which processor this session debugs, fixed when the server starts:
// the two have separate address spaces and separate breakpoint lists,
// and gdb has one of each.
bool g_okDebugCpu = true;

CProcessor* Proc()
{
    return g_okDebugCpu ? g_pBoard->GetCPU() : g_pBoard->GetPPU();
}

//////////////////////////////////////////////////////////////////////
// Hex, which every value in the protocol is written in

const char* const HEX_DIGITS = "0123456789abcdef";

int HexValue(char ch)
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

void AppendByte(std::string& out, uint8_t value)
{
    out.push_back(HEX_DIGITS[(value >> 4) & 0xf]);
    out.push_back(HEX_DIGITS[value & 0xf]);
}

// A register, in the layout gdb expects for this target: two bytes,
// least significant first.
void AppendWord(std::string& out, uint16_t value)
{
    AppendByte(out, (uint8_t)(value & 0xff));
    AppendByte(out, (uint8_t)(value >> 8));
}

// The four characters are two bytes, least significant byte first --
// the same layout AppendWord writes.
bool ParseWordFromHex(const std::string& text, size_t pos, uint16_t* value)
{
    if (pos + 4 > text.size())
        return false;

    int digits[4];
    for (int i = 0; i < 4; i++)
    {
        digits[i] = HexValue(text[pos + i]);
        if (digits[i] < 0)
            return false;
    }

    uint8_t low = (uint8_t)((digits[0] << 4) | digits[1]);
    uint8_t high = (uint8_t)((digits[2] << 4) | digits[3]);
    *value = (uint16_t)(low | (high << 8));
    return true;
}

// Step over `ch` if that is what comes next.
bool Expect(const std::string& text, size_t* pos, char ch)
{
    if (*pos >= text.size() || text[*pos] != ch)
        return false;
    (*pos)++;
    return true;
}

// Read a hexadecimal number up to `stop` or the end of the string.
bool ParseNumber(const std::string& text, size_t* pos, uint32_t* value)
{
    size_t start = *pos;
    uint32_t result = 0;
    while (*pos < text.size())
    {
        int digit = HexValue(text[*pos]);
        if (digit < 0)
            break;
        result = (result << 4) | (uint32_t)digit;
        (*pos)++;
    }
    if (*pos == start)
        return false;
    *value = result;
    return true;
}

//////////////////////////////////////////////////////////////////////
// The stream

bool SendAll(const char* data, size_t length)
{
    while (length > 0)
    {
        int sent = (int)send(g_socket, data, (int)length, 0);
        if (sent <= 0)
            return false;
        data += sent;
        length -= (size_t)sent;
    }
    return true;
}

// One byte, or -1 if the connection is gone. With `okBlock` false,
// returns -2 when there is nothing waiting.
int RecvByte(bool okBlock)
{
    if (!okBlock)
    {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(g_socket, &fds);
        struct timeval tv = { 0, 0 };
        int ready = select((int)g_socket + 1, &fds, nullptr, nullptr, &tv);
        if (ready <= 0)
            return -2;
    }

    unsigned char ch;
    int got = (int)recv(g_socket, (char*)&ch, 1, 0);
    if (got <= 0)
        return -1;
    return ch;
}

bool SendPacket(const std::string& data)
{
    std::string packet;
    packet.reserve(data.size() + 4);
    packet.push_back('$');
    packet.append(data);
    packet.push_back('#');

    uint8_t checksum = 0;
    for (char ch : data)
        checksum = (uint8_t)(checksum + (uint8_t)ch);
    AppendByte(packet, checksum);

    return SendAll(packet.data(), packet.size());
}

// Read one packet. Returns 1 with the payload in `packet`, 0 if the peer
// sent an interrupt (a bare 0x03, which is how gdb passes on a Ctrl-C),
// or -1 if the connection is gone.
//
// Acknowledgements from the peer are skipped rather than waited for:
// this is a reliable stream, a retransmission would mean the connection
// had already failed, and the protocol allows a stub to work this way.
int ReadPacket(std::string* packet)
{
    for (;;)
    {
        int ch = RecvByte(true);
        if (ch < 0)
            return -1;
        if (ch == 0x03)
            return 0;
        if (ch != '$')
            continue;  // An ack, or noise before the packet starts

        packet->clear();
        uint8_t checksum = 0;
        for (;;)
        {
            ch = RecvByte(true);
            if (ch < 0)
                return -1;
            if (ch == '#')
                break;
            packet->push_back((char)ch);
            checksum = (uint8_t)(checksum + (uint8_t)ch);
        }

        int high = RecvByte(true);
        int low = RecvByte(true);
        if (high < 0 || low < 0)
            return -1;
        int wanted = (HexValue((char)high) << 4) | HexValue((char)low);

        if (wanted != checksum)
        {
            if (!SendAll("-", 1))
                return -1;
            continue;
        }
        if (!SendAll("+", 1))
            return -1;
        return 1;
    }
}

//////////////////////////////////////////////////////////////////////
// The machine

// Memory is read through the non-intrusive view -- the same one the
// disassembler and the memory dump use -- so that gdb showing a window
// of memory cannot itself change a device register by reading it.
uint8_t ReadMemoryByte(uint16_t address)
{
    CProcessor* pProc = Proc();
    const CMemoryController* pMemCtl = pProc->GetMemoryControllerConst();
    int addrtype;
    uint16_t word = pMemCtl->GetWordView((uint16_t)(address & ~1),
                                         pProc->IsHaltMode(), false, &addrtype);
    return (address & 1) ? (uint8_t)(word >> 8) : (uint8_t)(word & 0xff);
}

void WriteMemoryByte(uint16_t address, uint8_t value)
{
    CProcessor* pProc = Proc();
    pProc->GetMemoryController()->SetByte(address, pProc->IsHaltMode(), value);
}

void ReadAllRegisters(std::string& out)
{
    CProcessor* pProc = Proc();
    for (int i = 0; i < 8; i++)
        AppendWord(out, pProc->GetReg(i));
    AppendWord(out, pProc->GetPSW());
}

void WriteRegister(int regnum, uint16_t value)
{
    CProcessor* pProc = Proc();
    if (regnum >= 0 && regnum < 8)
        pProc->SetReg(regnum, value);
    else if (regnum == 8)
        pProc->SetPSW(value);
}

bool AddBreakpoint(uint16_t address)
{
    return g_okDebugCpu ? Emulator_AddCPUBreakpoint(address)
                        : Emulator_AddPPUBreakpoint(address);
}

bool RemoveBreakpoint(uint16_t address)
{
    return g_okDebugCpu ? Emulator_RemoveCPUBreakpoint(address)
                        : Emulator_RemovePPUBreakpoint(address);
}

// Run until a breakpoint is hit or gdb interrupts. Returns the signal to
// report: 5 for a trap (which is what a breakpoint is), 2 for an
// interrupt.
int RunUntilStop()
{
    // Resuming from an address that has a breakpoint on it would trip
    // that breakpoint again before anything had run, and gdb would
    // report a stop where it had just been told to go. Step off it
    // first, which is what every stub has to do here.
    uint16_t pc = Proc()->GetReg(7);
    if (Emulator_IsBreakpoint(g_okDebugCpu, pc))
        g_pBoard->DebugTicks();

    Emulator_Start();
    int signal = 5;

    while (g_okEmulatorRunning)
    {
        if (!Emulator_SystemFrame())
            break;  // Breakpoint

        int ch = RecvByte(false);
        if (ch == 0x03)
        {
            signal = 2;
            break;
        }
        if (ch == -1)
        {
            signal = 2;
            break;  // The connection dropped; stop rather than run on
        }
    }

    Emulator_Stop();
    Emulator_OnUpdate();
    return signal;
}

//////////////////////////////////////////////////////////////////////

// Answer one packet. Returns false when the session is over.
bool HandlePacket(const std::string& packet)
{
    if (packet.empty())
        return SendPacket("");

    std::string reply;

    switch (packet[0])
    {
    case '?':
        // Whatever put us here, gdb wants a stop reason; the machine is
        // sitting at a console prompt, which is a trap as far as it is
        // concerned.
        return SendPacket("S05");

    case 'g':
        ReadAllRegisters(reply);
        return SendPacket(reply);

    case 'G':
        {
            for (int i = 0; i < 9; i++)
            {
                uint16_t value;
                if (!ParseWordFromHex(packet, 1 + (size_t)i * 4, &value))
                    return SendPacket("E01");
                WriteRegister(i, value);
            }
            return SendPacket("OK");
        }

    case 'p':
        {
            size_t pos = 1;
            uint32_t regnum;
            if (!ParseNumber(packet, &pos, &regnum) || regnum > 8)
                return SendPacket("E01");
            CProcessor* pProc = Proc();
            AppendWord(reply, regnum == 8 ? pProc->GetPSW()
                                          : pProc->GetReg((int)regnum));
            return SendPacket(reply);
        }

    case 'P':
        {
            size_t pos = 1;
            uint32_t regnum;
            if (!ParseNumber(packet, &pos, &regnum) || regnum > 8
                || pos >= packet.size() || packet[pos] != '=')
                return SendPacket("E01");
            uint16_t value;
            if (!ParseWordFromHex(packet, pos + 1, &value))
                return SendPacket("E01");
            WriteRegister((int)regnum, value);
            return SendPacket("OK");
        }

    case 'm':
        {
            size_t pos = 1;
            uint32_t address, length;
            if (!ParseNumber(packet, &pos, &address)
                || !Expect(packet, &pos, ',')
                || !ParseNumber(packet, &pos, &length))
                return SendPacket("E01");
            for (uint32_t i = 0; i < length; i++)
                AppendByte(reply, ReadMemoryByte((uint16_t)(address + i)));
            return SendPacket(reply);
        }

    case 'M':
        {
            size_t pos = 1;
            uint32_t address, length;
            if (!ParseNumber(packet, &pos, &address)
                || !Expect(packet, &pos, ',')
                || !ParseNumber(packet, &pos, &length)
                || !Expect(packet, &pos, ':'))
                return SendPacket("E01");
            for (uint32_t i = 0; i < length; i++)
            {
                if (pos + 2 > packet.size())
                    return SendPacket("E01");
                int high = HexValue(packet[pos]);
                int low = HexValue(packet[pos + 1]);
                if (high < 0 || low < 0)
                    return SendPacket("E01");
                WriteMemoryByte((uint16_t)(address + i),
                                (uint8_t)((high << 4) | low));
                pos += 2;
            }
            return SendPacket("OK");
        }

    case 'c':
        {
            // An address after the "c" means "resume there instead".
            if (packet.size() > 1)
            {
                size_t pos = 1;
                uint32_t address;
                if (ParseNumber(packet, &pos, &address))
                    Proc()->SetReg(7, (uint16_t)address);
            }
            int signal = RunUntilStop();
            char buffer[8];
            snprintf(buffer, sizeof (buffer), "S%02x", signal);
            return SendPacket(buffer);
        }

    case 's':
        {
            if (packet.size() > 1)
            {
                size_t pos = 1;
                uint32_t address;
                if (ParseNumber(packet, &pos, &address))
                    Proc()->SetReg(7, (uint16_t)address);
            }
            g_pBoard->DebugTicks();
            Emulator_OnUpdate();
            return SendPacket("S05");
        }

    case 'Z':
    case 'z':
        {
            // Z0/z0 is a software breakpoint. The others -- hardware
            // breakpoints and watchpoints -- are left unanswered, which
            // tells gdb it has to manage them itself.
            if (packet.size() < 2 || packet[1] != '0')
                return SendPacket("");
            size_t pos = 2;
            uint32_t address;
            if (!Expect(packet, &pos, ',')
                || !ParseNumber(packet, &pos, &address))
                return SendPacket("E01");
            bool okDone = (packet[0] == 'Z')
                ? AddBreakpoint((uint16_t)address)
                : RemoveBreakpoint((uint16_t)address);
            return SendPacket(okDone ? "OK" : "E01");
        }

    case 'H':
        // "Use thread N for the following operations." There is one.
        return SendPacket("OK");

    case 'D':
        SendPacket("OK");
        return false;

    case 'k':
        return false;

    case 'q':
        if (packet.compare(0, 10, "qSupported") == 0)
            return SendPacket("PacketSize=1000");
        if (packet == "qAttached")
            return SendPacket("1");   // Already running; do not kill on detach
        if (packet == "qC")
            return SendPacket("QC1");
        if (packet == "qfThreadInfo")
            return SendPacket("m1");
        if (packet == "qsThreadInfo")
            return SendPacket("l");
        return SendPacket("");

    default:
        return SendPacket("");
    }
}

socket_t Listen(int port)
{
    socket_t listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener == INVALID_SOCKET_VALUE)
        return INVALID_SOCKET_VALUE;

    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof (one));

    struct sockaddr_in address;
    memset(&address, 0, sizeof (address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons((uint16_t)port);

    if (bind(listener, (struct sockaddr*)&address, sizeof (address)) != 0
        || listen(listener, 1) != 0)
    {
        CloseSocket(listener);
        return INVALID_SOCKET_VALUE;
    }
    return listener;
}

}  // namespace

//////////////////////////////////////////////////////////////////////

void GdbServer_Run(int port, bool okDebugCpu)
{
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
    {
        std::wcout << L" Cannot start Winsock." << std::endl;
        return;
    }
#endif

    socket_t listener = Listen(port);
    if (listener == INVALID_SOCKET_VALUE)
    {
        std::wcout << L" Cannot listen on port " << port << L"." << std::endl;
        return;
    }

    g_okDebugCpu = okDebugCpu;

    std::wcout << L"Listening on localhost:" << port << L" for "
               << (g_okDebugCpu ? L"CPU" : L"PPU") << L"; in gdb say" << std::endl;
    std::wcout << L"  target remote :" << port << std::endl;

    g_socket = accept(listener, nullptr, nullptr);
    CloseSocket(listener);
    if (g_socket == INVALID_SOCKET_VALUE)
    {
        std::wcout << L" No connection." << std::endl;
        return;
    }

    int one = 1;
    setsockopt(g_socket, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof (one));
    std::wcout << L"gdb connected." << std::endl;

    for (;;)
    {
        std::string packet;
        int result = ReadPacket(&packet);
        if (result < 0)
            break;
        if (result == 0)
        {
            // An interrupt outside a run: nothing was going on, so just
            // say where we are.
            SendPacket("S02");
            continue;
        }
        if (!HandlePacket(packet))
            break;
    }

    CloseSocket(g_socket);
    g_socket = INVALID_SOCKET_VALUE;
    std::wcout << L"gdb disconnected." << std::endl;
}
