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
#include "Screen.h"

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
//   H, qC, qfThreadInfo, qsThreadInfo, qThreadExtraInfo, T
//                  which of the two processors a packet is about, what
//                  to call them, and whether they are still there
//   qSupported, qAttached
//                  the opening negotiation, including whether the two
//                  are told apart by process id -- see below
//   !, vRun, R     extended mode: starting and restarting a program.
//                  RT-11 loads programs, not gdb, so "start" here means
//                  what a person would do -- reset, let the system come
//                  up, type "R NAME" -- stopping at the entry point.
//   vCont, vCont?  resume, with range stepping (see RangeStep below)
//   X              binary memory write, which is what "load" uses
//
// and one packet sent the other way, unasked: O, which carries whatever
// the program writes to the console -- taken off the channel to the
// peripheral processor -- so that it appears in gdb's terminal as well
// as on the machine's screen.
//
// Everything else gets an empty reply, which is the protocol's way of
// saying "not implemented" and is always a valid answer.

socket_t g_socket = INVALID_SOCKET_VALUE;

// The machine's two processors are two processes to gdb: the central
// one is process 1, the peripheral one process 2, each with a single
// thread of its own -- "p1.1" and "p2.1" as the protocol writes them.
//
// Processes rather than threads, which is what these were at first,
// because threads share one address space and one set of symbols.  Two
// processors that share neither do not fit that: gdb would label an
// address in one with whatever name the other had near it, and code
// loaded into the peripheral processor could have no symbols of its own
// at all, since the one executable gdb reads is the central one's.
// Processes get a symbol table each, which is what these two want.
//
// gdb has to ask for that, though -- the multiprocess extension, agreed
// in qSupported -- and a client that does not ask gets the older
// answer: the same two, numbered the same, as threads of one process.
// Only how an id is written differs; everything else about the session
// is the same either way.
//
// Stepping runs the board, so both advance together whichever one is
// selected; what the selection decides is whose registers and whose
// memory a packet is about.
const int PROC_CPU = 1;
const int PROC_PPU = 2;

// Set by the H packet, and by --ppu before gdb has said anything.
int g_selected = PROC_CPU;

// Whether gdb asked for the multiprocess extension. It decides how an
// id is written and read, and nothing else.
bool g_okMultiprocess = false;

bool IsCpu(int id)
{
    return id != PROC_PPU;
}

// Set by the "!" packet. gdb sends it when connected with "target
// extended-remote", and will not offer "run" without it.
bool g_okExtended = false;

// The RT-11 name of the program to start, remembered from the last vRun
// so that "R" -- which carries no name -- can restart the same one.
std::string g_programName;

// See GdbServer.h: how long one continue may run, in frames.
int g_maxFrames = 0;

// .EXIT, the operating-system call an RT-11 program ends with. There is
// no other sign that it has: the monitor simply takes over.
const uint8_t EMT_EXIT = 0350;

// Filled in by the two observers below, which run deep inside
// instruction execution and must not do anything slow there; the frame
// loop sends this on and clears it.
std::string g_consoleOutput;
bool g_okProgramExited = false;

// Console output, taken where the central processor hands a byte to the
// peripheral one -- channel 0 is the terminal.
//
// Not where the program asks for it: .TTYOUT reports failure when the
// console is busy and newlib retries the same character until it is
// taken, so watching the calls counts one character hundreds of times.
// This is the byte itself, once.
void CALLBACK TerminalObserver(uint8_t byte)
{
    g_consoleOutput.push_back((char)byte);
}

void EmtObserver(CProcessor* pProc, uint8_t code)
{
    // The central processor only: RT-11 runs there, and so does every
    // program that has a console to write to or an exit to make.
    if (pProc != g_pBoard->GetCPU())
        return;

    if (code == EMT_EXIT)
        g_okProgramExited = true;
}

// Zero means "whichever gdb has selected": it is what an unqualified
// vCont action, and every packet that names neither of the two, is
// about.
int SelectedOr(int id)
{
    return id == 0 ? g_selected : id;
}

CProcessor* ProcFor(int id)
{
    return IsCpu(SelectedOr(id))
        ? g_pBoard->GetCPU() : g_pBoard->GetPPU();
}

CProcessor* Proc()
{
    return ProcFor(0);
}

//////////////////////////////////////////////////////////////////////
// Typing, for starting a program the way a person would

// The machine's keyboard sends hardware matrix scan-codes, not
// characters, so the few characters an RT-11 command line is made of
// have to be looked up. Letters are the Latin glyph on the key. From
// UKNCBTL's own emulator/KeyboardView.cpp.
struct KeyForChar
{
    char ch;
    uint8_t scancode;
};

const KeyForChar KEYS_FOR_CHARS[] =
{
    { 'A', 0072 }, { 'B', 0076 }, { 'C', 0050 }, { 'D', 0057 },
    { 'E', 0033 }, { 'F', 0047 }, { 'G', 0055 }, { 'H', 0156 },
    { 'I', 0073 }, { 'J', 0027 }, { 'K', 0052 }, { 'L', 0056 },
    { 'M', 0112 }, { 'N', 0054 }, { 'O', 0075 }, { 'P', 0053 },
    { 'Q', 0067 }, { 'R', 0074 }, { 'S', 0111 }, { 'T', 0114 },
    { 'U', 0051 }, { 'V', 0137 }, { 'W', 0071 }, { 'X', 0115 },
    { 'Y', 0070 }, { 'Z', 0157 },
    { '0', 0176 }, { '1', 0030 }, { '2', 0031 }, { '3', 0032 },
    { '4', 0013 }, { '5', 0034 }, { '6', 0035 }, { '7', 0016 },
    { '8', 0017 }, { '9', 0177 },
    { '.', 0135 }, { ',', 0117 }, { ';', 0007 }, { ':', 0174 },
    { '-', 0175 }, { '/', 0173 },
    { ' ', 0113 },  // SPACE
    { '\r', 0153 },  // ENTER
};

// Enough frames around each press and release for the machine to see it.
const int KEY_HOLD_FRAMES = 6;

void TypeLine(const std::string& text)
{
    for (char ch : text)
    {
        char wanted = (ch == '\n') ? '\r' : (char)toupper((unsigned char)ch);

        for (const KeyForChar& key : KEYS_FOR_CHARS)
        {
            if (key.ch != wanted)
                continue;

            Emulator_KeyEvent(key.scancode, true);
            for (int i = 0; i < KEY_HOLD_FRAMES; i++)
                g_pBoard->SystemFrame();
            Emulator_KeyEvent(key.scancode, false);
            for (int i = 0; i < KEY_HOLD_FRAMES; i++)
                g_pBoard->SystemFrame();
            break;
        }
    }
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

// Step over `what` if that is what comes next.
bool ExpectText(const std::string& text, size_t* pos, const char* what)
{
    size_t length = strlen(what);
    if (*pos + length > text.size() || text.compare(*pos, length, what) != 0)
        return false;
    *pos += length;
    return true;
}

// One of the two, written the way the protocol asks for it: "p1.1" once
// gdb has asked for the multiprocess extension, plain "1" otherwise.
std::string FormatId(int id)
{
    char buffer[16];
    if (g_okMultiprocess)
        snprintf(buffer, sizeof (buffer), "p%x.1", id);
    else
        snprintf(buffer, sizeof (buffer), "%x", id);
    return buffer;
}

// And back: "p2.1", "p2.-1", "p-1.-1", "2", "-1", "0" -- everything gdb
// writes where an id goes.  The process is the part that matters, each
// of these two having exactly one thread, so the thread is parsed only
// to step over it.  "All" and "any" both answer zero, which every
// caller reads as "whichever is selected".
int ParseId(const std::string& text, size_t* pos)
{
    uint32_t value = 0;
    int id = 0;

    if (*pos < text.size() && text[*pos] == 'p')
    {
        (*pos)++;
        if (!ExpectText(text, pos, "-1") && ParseNumber(text, pos, &value))
            id = (int)value;
        if (ExpectText(text, pos, "."))
        {
            if (!ExpectText(text, pos, "-1"))
                ParseNumber(text, pos, &value);
        }
        return id;
    }

    if (ExpectText(text, pos, "-1"))
        return 0;
    if (!ParseNumber(text, pos, &value))
        return 0;
    return (int)value;
}

// The one a vCont action is for: the ":id" written after it.  Nothing
// there is "every thread", which answers zero like the rest.
int ParseActionId(const std::string& text, size_t* pos)
{
    if (*pos >= text.size() || text[*pos] != ':')
        return 0;
    (*pos)++;
    return ParseId(text, pos);
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
// A select() that came back because a signal arrived, rather than
// because anything happened -- which is not an error and not an answer,
// only a reason to ask again.
bool IsInterrupted()
{
#ifdef _WIN32
    return false;
#else
    return errno == EINTR;
#endif
}

// Waiting is done in steps of this, with a window open: it wants its
// events answered every so often or the desktop decides the program has
// hung -- and a wait here can be a long one, a breakpoint being somebody
// reading their screen.  So a blocking read is a wait with a timeout,
// and the timeout is where the window gets its turn.
const int kIdleMicroseconds = 20 * 1000;

int RecvByte(bool okBlock)
{
    for (;;)
    {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(g_socket, &fds);
        struct timeval tv = { 0, okBlock ? kIdleMicroseconds : 0 };
        int ready = select((int)g_socket + 1, &fds, nullptr, nullptr, &tv);
        if (ready > 0)
            break;
        if (ready < 0 && !IsInterrupted())
            return -1;
        if (!okBlock)
            return -2;
        if (Screen_QuitRequested())
            return -1;   // As if the connection had gone: the run is over
        Screen_Idle();
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
    return IsCpu(g_selected)
        ? Emulator_AddCPUBreakpoint(address)
        : Emulator_AddPPUBreakpoint(address);
}

bool RemoveBreakpoint(uint16_t address)
{
    return IsCpu(g_selected)
        ? Emulator_RemoveCPUBreakpoint(address)
        : Emulator_RemovePPUBreakpoint(address);
}

// Run until a breakpoint is hit or gdb interrupts. Returns the signal to
// report: 5 for a trap (which is what a breakpoint is), 2 for an
// interrupt.
// How a resume ended, in the values RunUntilStop returns: a signal the
// program stopped for, or one of these.
const int STOPPED_EXITED = -1;    // The program called .EXIT
const int STOPPED_TIMEOUT = -2;   // It ran past the frame bound

// Hand gdb whatever the program has printed since last time, as an "O"
// packet -- the one thing a stub may send while the program is running.
void FlushConsoleOutput()
{
    if (g_consoleOutput.empty())
        return;

    std::string hex;
    for (char ch : g_consoleOutput)
        AppendByte(hex, (uint8_t)ch);
    g_consoleOutput.clear();
    SendPacket("O" + hex);
}

int RunUntilStop()
{
    // Resuming from an address that has a breakpoint on it would trip
    // that breakpoint again before anything had run, and gdb would
    // report a stop where it had just been told to go. Step off it
    // first, which is what every stub has to do here.
    //
    // Either processor, not just the selected one: they run together,
    // so a breakpoint the other is sitting on would stop the resume
    // just as immediately. One step moves both (see DebugTicks).
    if (Emulator_IsBreakpoint(true, g_pBoard->GetCPU()->GetPC())
        || Emulator_IsBreakpoint(false, g_pBoard->GetPPU()->GetPC()))
        g_pBoard->DebugTicks();

    Emulator_Start();
    int signal = 5;

    g_okProgramExited = false;

    int frames = 0;
    while (g_okEmulatorRunning)
    {
        if (g_maxFrames > 0 && frames >= g_maxFrames)
        {
            signal = STOPPED_TIMEOUT;
            break;
        }
        frames++;

        bool okHitBreakpoint = !Emulator_SystemFrame();
        Screen_Frame();
        Screen_Pace();
        FlushConsoleOutput();
        if (okHitBreakpoint)
            break;
        if (g_okProgramExited)
        {
            // .EXIT happens while the last of what was written is
            // still in the operating system's console buffer, on its
            // way to the peripheral processor a byte at a time -- and
            // slowly, a program that has printed a page of text having
            // queued far more than it has delivered. Keep going until
            // the trickle stops, or the end of the output is lost.
            const int kDrainMaxFrames = 900;
            const int kDrainIdleFrames = 250;
            int idle = 0;
            for (int i = 0; i < kDrainMaxFrames && idle < kDrainIdleFrames; i++)
            {
                size_t before = g_consoleOutput.size();
                Emulator_SystemFrame();
                Screen_Frame();
                idle = (g_consoleOutput.size() == before) ? idle + 1 : 0;
                FlushConsoleOutput();
            }
            signal = STOPPED_EXITED;
            break;
        }

        if (Screen_QuitRequested())
        {
            signal = 2;   // Stopped, and the next read will end the session
            break;
        }

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
    return signal;
}

// Single-step while the program counter stays inside [low, high), which
// is what gdb's range stepping asks for: it saves a round trip per
// instruction when stepping over a line's worth of code.
//
// Stops early on a breakpoint, on gdb interrupting, and on a cap, since
// stepping does not advance the frame timer and code waiting on it would
// never leave the range.
int RangeStep(uint16_t low, uint16_t high, int id)
{
    const long kMaxInstructions = 2000000;
    CProcessor* pProc = ProcFor(id);
    bool okCpu = IsCpu(SelectedOr(id));

    for (long executed = 0; executed < kMaxInstructions; executed++)
    {
        g_pBoard->DebugTicks();

        uint16_t pc = pProc->GetPC();
        if (Emulator_IsBreakpoint(okCpu, pc))
            break;
        if (pc < low || pc >= high)
            break;

        int ch = RecvByte(false);
        if (ch == 0x03 || ch == -1)
        {
            return 2;
        }
    }

    return 5;
}

// Turn the file name gdb sent into something RT-11 will answer to: the
// last path component, without its extension, upper case, six characters
// at most. "build/hello.sav" becomes "HELLO".
std::string Rt11NameFromPath(const std::string& path)
{
    size_t slash = path.find_last_of("/\\");
    std::string name = (slash == std::string::npos) ? path : path.substr(slash + 1);

    size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0)
        name.erase(dot);

    if (name.size() > 6)
        name.erase(6);
    for (char& ch : name)
        ch = (char)toupper((unsigned char)ch);
    return name;
}

// Start a program the way a person would: reset, wait for RT-11, type
// "R NAME", and stop at the entry point. A .sav is an absolute image
// loaded at 001000, so that is where every program here begins.
//
// Returns false with a reason if it does not get there, which usually
// means the firmware stopped at its boot menu -- "run" needs the
// autoboot firmware, since nothing types the menu's answers.
bool StartProgram(const std::string& name, std::string* error)
{
    const int kBootFrames = 1200;      // ~700 is enough for the autoboot ROM
    const int kProgramFrames = 600;    // for RT-11 to find and load the file
    const uint16_t kEntryPoint = 01000;

    if (name.empty())
    {
        *error = "no program to run: say \"set remote exec-file NAME\" in gdb";
        return false;
    }

    Emulator_Reset();
    for (int i = 0; i < kBootFrames; i++)
        g_pBoard->SystemFrame();

    TypeLine("R " + name + "\r");

    g_okProgramExited = false;

    Emulator_SetTempCPUBreakpoint(kEntryPoint);
    Emulator_Start();
    bool okStarted = false;
    for (int i = 0; i < kProgramFrames && g_okEmulatorRunning; i++)
    {
        if (!Emulator_SystemFrame())
        {
            okStarted = true;
            break;
        }
        Screen_Frame();
    }
    Emulator_Stop();

    // Everything printed up to here is the operating system's -- the
    // echo of the "R NAME" just typed -- and not the program's.
    g_consoleOutput.clear();

    if (!okStarted)
    {
        *error = "the machine never reached the program: is this the autoboot "
                 "firmware, and is " + name + " on the disk?";
        return false;
    }
    return true;
}

// Which processor stopped: the one sitting on a breakpoint of its own.
// With nothing to go by -- a step, an interrupt -- the answer is
// whichever one is selected.
int StoppedProcessor()
{
    if (Emulator_IsBreakpoint(true, g_pBoard->GetCPU()->GetPC()))
        return PROC_CPU;
    if (Emulator_IsBreakpoint(false, g_pBoard->GetPPU()->GetPC()))
        return PROC_PPU;
    return g_selected;
}

// The stop reply for one of those.
//
// A timeout is reported as "terminated by SIGALRM" rather than "stopped
// with SIGALRM", because gdb passes that signal on and resumes -- which
// against a program that is not going to stop means resuming for ever.
// Terminated leaves nothing to resume, which is what giving up means.
// RT-11's .EXIT carries no status, so a program that ended reports zero.
//
// A plain stop names which of the two it is, so that gdb knows what it
// is looking at; and since the answer is a stop in one of them while
// the other is simply also there, the selection follows it. Zero means
// "work it out"; anything else is the one the resume was for -- see the
// vCont packet.
std::string StopReply(int signal, int id = 0)
{
    // The program that ends is the central processor's -- the
    // peripheral one is a process of its own and carries on -- so with
    // the multiprocess extension the end has to say whose it was.
    const std::string whose = g_okMultiprocess ? ";process:1" : "";

    if (signal == STOPPED_EXITED)
        return "W00" + whose;
    if (signal == STOPPED_TIMEOUT)
        return "X0e" + whose;

    // A resume that named one of them is answered for that one, whatever
    // the other happens to be sitting on: gdb asked this one to step,
    // and an answer about the other is not an answer it knows what to do
    // with.
    g_selected = (id == 0) ? StoppedProcessor() : id;

    char buffer[32];
    snprintf(buffer, sizeof (buffer), "T%02xthread:%s;",
             signal, FormatId(g_selected).c_str());
    return buffer;
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
    case '!':
        g_okExtended = true;
        return SendPacket("OK");

    case 'R':
        {
            // Restart. The argument is required by the protocol and
            // ignored by everyone; there is no reply either way.
            std::string error;
            if (!StartProgram(g_programName, &error))
                std::wcout << L" gdb: " << std::wstring(error.begin(), error.end())
                           << std::endl;
            g_selected = PROC_CPU;   // As for vRun above
            return true;
        }

    case '?':
        // Whatever put us here, gdb wants a stop reason; the machine is
        // sitting at a console prompt, which is a trap as far as it is
        // concerned.
        return SendPacket(StopReply(5));

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

    case 'X':
        {
            // Binary memory write, which is what "load" uses -- one byte
            // per byte rather than two hex digits. '}' escapes the next
            // character, which is the original XOR 0x20; that is how $,
            // #, } and * are kept out of the payload.
            size_t pos = 1;
            uint32_t address, length;
            if (!ParseNumber(packet, &pos, &address)
                || !Expect(packet, &pos, ',')
                || !ParseNumber(packet, &pos, &length)
                || !Expect(packet, &pos, ':'))
                return SendPacket("E01");

            uint32_t written = 0;
            while (pos < packet.size() && written < length)
            {
                uint8_t value = (uint8_t)packet[pos++];
                if (value == '}')
                {
                    if (pos >= packet.size())
                        return SendPacket("E01");
                    value = (uint8_t)(packet[pos++] ^ 0x20);
                }
                WriteMemoryByte((uint16_t)(address + written), value);
                written++;
            }
            if (written != length)
                return SendPacket("E01");
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
            return SendPacket(StopReply(RunUntilStop()));
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
            return SendPacket(StopReply(5));
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

    case 'v':
        {
            if (packet == "vCont?")
                return SendPacket("vCont;c;C;s;S;t;r");

            if (packet.compare(0, 6, "vCont;") == 0)
            {
                // The whole list is read, and which of the two each
                // action names with it. gdb steps one over a breakpoint
                // while the rest run -- "vCont;s:p1.1;c" -- and
                // answering that step for the other is not a mistake gdb
                // recovers from: it aborts with an internal error.
                //
                // Only one action can be carried out, since the board
                // runs as a whole: a step advances both processors (see
                // DebugTicks) and a resume runs both. So the first
                // action that is not a plain continue decides, and the
                // one it names is whose stop this is reported as.
                size_t pos = 6;
                char what = 0;
                int id = 0;
                uint32_t low = 0, high = 0;

                while (pos < packet.size())
                {
                    char action = packet[pos++];
                    uint32_t first = 0, second = 0;

                    if (action == 'r')
                    {
                        if (!ParseNumber(packet, &pos, &first)
                            || !Expect(packet, &pos, ',')
                            || !ParseNumber(packet, &pos, &second))
                            return SendPacket("E01");
                    }
                    else if (action == 'C' || action == 'S')
                    {
                        // The signal to resume with, which this machine
                        // has no way to deliver -- only the resume
                        // itself is carried out.
                        if (!ParseNumber(packet, &pos, &first))
                            return SendPacket("E01");
                    }
                    else if (action != 'c' && action != 's' && action != 't')
                        return SendPacket("E01");

                    int actionId = ParseActionId(packet, &pos);
                    if (what == 0 && action != 'c' && action != 'C')
                    {
                        what = action;
                        id = actionId;
                        low = first;
                        high = second;
                    }
                    if (pos < packet.size() && packet[pos] == ';')
                        pos++;
                }

                if (what == 'r')
                    return SendPacket(StopReply(RangeStep((uint16_t)low,
                                                          (uint16_t)high, id),
                                                id));
                if (what == 's' || what == 'S')
                {
                    g_pBoard->DebugTicks();
                    return SendPacket(StopReply(5, id));
                }
                if (what == 't')
                    return SendPacket(StopReply(2, id));
                return SendPacket(StopReply(RunUntilStop()));
            }

            if (packet.compare(0, 5, "vRun;") == 0)
            {
                // The file name is hex-encoded, and empty means "the one
                // from last time".
                std::string path;
                size_t pos = 5;
                while (pos + 1 < packet.size() && packet[pos] != ';')
                {
                    int high = HexValue(packet[pos]);
                    int low = HexValue(packet[pos + 1]);
                    if (high < 0 || low < 0)
                        return SendPacket("E01");
                    path.push_back((char)((high << 4) | low));
                    pos += 2;
                }
                if (!path.empty())
                    g_programName = Rt11NameFromPath(path);

                std::string error;
                if (!StartProgram(g_programName, &error))
                {
                    std::wcout << L" gdb: " << std::wstring(error.begin(), error.end())
                               << std::endl;
                    return SendPacket("E01");
                }
                // Whatever was selected before, what has just started is
                // the central processor's, and that is where the stop is.
                g_selected = PROC_CPU;
                return SendPacket(StopReply(5));
            }

            if (packet.compare(0, 6, "vKill;") == 0)
                return SendPacket("OK");

            return SendPacket("");
        }

    case 'T':
        {
            // "Is thread N still alive?"  Both always are: they are the
            // machine's two processors, and the machine is switched on.
            // Without this answer gdb decides they have terminated and
            // will not switch to either.
            size_t pos = 1;
            int id = ParseId(packet, &pos);
            return SendPacket(
                (id == PROC_CPU || id == PROC_PPU) ? "OK" : "E01");
        }

    case 'H':
        {
            // "Use thread N for what follows."  The letter after H says
            // what for -- g for reading and writing, c for running --
            // and only the first changes anything here, since running
            // runs the whole board.
            if (packet.size() < 2)
                return SendPacket("E01");
            size_t pos = 2;
            int id = ParseId(packet, &pos);
            if (packet[1] == 'g' && id != 0)   // Zero is "any", which leaves it alone
            {
                if (id != PROC_CPU && id != PROC_PPU)
                    return SendPacket("E01");
                g_selected = id;
            }
            return SendPacket("OK");
        }

    case 'D':
        // "D;pid" detaches one process: the other is still there and so
        // is the session. A bare "D" is the whole thing.
        SendPacket("OK");
        return packet.size() > 1 && packet[1] == ';';

    case 'k':
        return false;

    case 'q':
        if (packet.compare(0, 10, "qSupported") == 0)
        {
            // The two processors are told apart by process id only if
            // gdb asks for that here: a stub must not answer in a form
            // the other side did not agree to.
            g_okMultiprocess = packet.find("multiprocess+") != std::string::npos;
            return SendPacket(g_okMultiprocess
                ? "PacketSize=1000;vContSupported+;swbreak+;multiprocess+"
                : "PacketSize=1000;vContSupported+;swbreak+");
        }
        if (packet.compare(0, 10, "qAttached") == 0)
            return SendPacket("1");   // Already running; do not kill on detach
        if (packet == "qC")
            return SendPacket("QC" + FormatId(g_selected));
        if (packet == "qfThreadInfo")
            return SendPacket("m" + FormatId(PROC_CPU) + "," + FormatId(PROC_PPU));
        if (packet == "qsThreadInfo")
            return SendPacket("l");
        if (packet.compare(0, 17, "qThreadExtraInfo,") == 0)
        {
            // What to call them in a thread list.  Hex, like every
            // string the protocol carries.
            size_t pos = 17;
            const char* name = (ParseId(packet, &pos) == PROC_PPU)
                ? "PPU (peripheral processor)"
                : "CPU (central processor)";
            for (const char* p = name; *p != '\0'; p++)
                AppendByte(reply, (uint8_t)*p);
            return SendPacket(reply);
        }
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

void GdbServer_Run(int port, bool okDebugCpu, int maxFrames)
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

    g_selected = okDebugCpu ? PROC_CPU : PROC_PPU;
    g_maxFrames = maxFrames;

    std::wcout << L"Listening on localhost:" << port << L" for "
               << (IsCpu(g_selected) ? L"CPU" : L"PPU")
               << L" first; in gdb say" << std::endl;
    std::wcout << L"  target remote :" << port << std::endl;

    // Waiting for gdb to turn up, in steps rather than in one blocking
    // accept: with a window open, this is the longest the machine ever
    // sits still, and a window whose events nobody answers is a window
    // the desktop greys out and calls hung.
    for (;;)
    {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(listener, &fds);
        struct timeval tv = { 0, kIdleMicroseconds };
        int ready = select((int)listener + 1, &fds, nullptr, nullptr, &tv);
        if (ready > 0)
            break;
        // A signal cuts the wait short without meaning anything by it --
        // SDL takes SIGTERM as "quit", and answers it through the event
        // queue like any other way of closing the window, so the flag
        // below is what decides, not the interruption.
        if ((ready < 0 && !IsInterrupted()) || Screen_QuitRequested())
        {
            CloseSocket(listener);
            std::wcout << L" No connection." << std::endl;
            return;
        }
        Screen_Idle();
    }

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

    g_consoleOutput.clear();
    g_okProgramExited = false;
    CProcessor::SetEMTCallback(EmtObserver);
    g_pBoard->SetTerminalCallback(TerminalObserver);

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
            SendPacket(StopReply(2));
            continue;
        }
        if (!HandlePacket(packet))
            break;
    }

    CProcessor::SetEMTCallback(nullptr);
    g_pBoard->SetTerminalCallback(nullptr);
    CloseSocket(g_socket);
    g_socket = INVALID_SOCKET_VALUE;
    std::wcout << L"gdb disconnected." << std::endl;
}
