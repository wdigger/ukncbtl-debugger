// ukncbtldebug.cpp -- a UKNC for gdb to debug.
//
// The emulator core, and a server for gdb's remote protocol in front of
// it.  There is no debugger of its own here: gdb is the debugger, and
// everything this used to offer at a prompt of its own -- registers,
// memory, breakpoints, stepping, disassembly, symbols, source lines --
// gdb does better, over the wire, against the program's own source.
//
// So it takes its settings on the command line, starts the machine, and
// waits:
//
//   ukncbtldebug --disk1 rt11.dsk --port 2345
//   (gdb) target extended-remote :2345
//
// See README.md for the rest, and util/GdbServer.cpp for which of the
// protocol it answers.

#include "stdafx.h"
#include <clocale>

#include "ukncbtldebug.h"
#include "Emulator.h"
#include "util/Screen.h"
#include "emubase/Emubase.h"
#include "util/GdbServer.h"


//////////////////////////////////////////////////////////////////////

struct Options
{
    std::wstring disks[4];
    std::wstring rom;
    int  port = 2345;
    int  frames = 0;
    bool okDebugCpu = true;
    bool okBoot = false;
    bool okScreen = false;
    bool okHelp = false;
};

void PrintUsage()
{
    std::wcout
        << L"ukncbtldebug -- a UKNC for gdb to debug" << std::endl
        << std::endl
        << L"  --disk1 FILE ... --disk4 FILE  attach a floppy image to a drive" << std::endl
        << L"  --rom FILE       the machine's firmware (default: uknc_rom.bin here)" << std::endl
        << L"  --port N         serve gdb on localhost:N (default: 2345)" << std::endl
        << L"  --ppu            start with the peripheral processor selected; both are" << std::endl
        << L"                   served either way, as gdb's processes 1 (CPU) and 2 (PPU)" << std::endl
        << L"  --boot           bring the operating system up before listening, so that" << std::endl
        << L"                   gdb's \"load\" has something to run under" << std::endl
        << L"  --screen         show the machine's screen in a window, and run it at the" << std::endl
        << L"                   speed that implies (needs SDL3; without it, no window)" << std::endl
        << L"  --frames N       give up on a program that has run this long," << std::endl
        << L"                   in frames of 1/50 second (default: no limit)" << std::endl
        << L"  --help           this" << std::endl
        << std::endl
        << L"In gdb:  target extended-remote :PORT" << std::endl;
}

// True if `arg` is the option `name`, which takes the value in `value`.
bool TakesValue(const std::wstring& arg, const wchar_t* name,
                std::vector<std::wstring>& args, size_t* index,
                std::wstring* value)
{
    if (arg != name)
        return false;
    if (*index + 1 >= args.size())
    {
        std::wcout << L"Missing value for " << name << std::endl;
        return false;
    }
    (*index)++;
    *value = args[*index];
    return true;
}

bool ParseCommandLine(std::vector<std::wstring>& args, Options* options)
{
    for (size_t i = 0; i < args.size(); i++)
    {
        const std::wstring& arg = args[i];
        std::wstring value;

        if (arg == L"--help" || arg == L"-h")
        {
            options->okHelp = true;
            return true;
        }
        if (arg == L"--ppu")
        {
            options->okDebugCpu = false;
            continue;
        }
        if (arg == L"--boot")
        {
            options->okBoot = true;
            continue;
        }
        if (arg == L"--screen")
        {
            options->okScreen = true;
            continue;
        }

        bool okDisk = false;
        for (int slot = 0; slot < 4; slot++)
        {
            wchar_t name[8];
            swprintf(name, 8, L"--disk%d", slot + 1);
            if (TakesValue(arg, name, args, &i, &value))
            {
                options->disks[slot] = value;
                okDisk = true;
                break;
            }
            if (arg == name)
                return false;  // Named but without a value
        }
        if (okDisk)
            continue;

        if (TakesValue(arg, L"--rom", args, &i, &value))
        {
            options->rom = value;
            continue;
        }
        if (TakesValue(arg, L"--port", args, &i, &value))
        {
            options->port = (int)wcstol(value.c_str(), nullptr, 10);
            if (options->port <= 0 || options->port > 65535)
            {
                std::wcout << L"Not a port number: " << value << std::endl;
                return false;
            }
            continue;
        }
        if (TakesValue(arg, L"--frames", args, &i, &value))
        {
            options->frames = (int)wcstol(value.c_str(), nullptr, 10);
            if (options->frames < 0)
            {
                std::wcout << L"Not a frame count: " << value << std::endl;
                return false;
            }
            continue;
        }

        if (arg == L"--rom" || arg == L"--port" || arg == L"--frames")
            return false;  // Named but without a value

        std::wcout << L"Unknown option: " << arg << std::endl;
        return false;
    }

    return true;
}

int Run(std::vector<std::wstring>& args)
{
    Options options;
    if (!ParseCommandLine(args, &options))
    {
        PrintUsage();
        return 255;
    }
    if (options.okHelp)
    {
        PrintUsage();
        return 0;
    }

    if (!Emulator_Init(options.rom))
    {
        std::wcout << L"Failed to initialize emulator." << std::endl;
        return 1;
    }

    for (int slot = 0; slot < 4; slot++)
    {
        if (options.disks[slot].empty())
            continue;
        std::string path = WStringToNarrowString(options.disks[slot]);
        if (!Emulator_AttachFloppyImage(slot, path.c_str()))
        {
            std::wcout << L"Failed to attach " << options.disks[slot] << std::endl;
            Emulator_Done();
            return 1;
        }
    }

    // Before the boot, so that it is something to watch.  A window that
    // will not open is not fatal: Screen_Open() says why and the
    // machine runs on unwatched.
    if (options.okScreen)
        Screen_Open();

    if (options.okBoot)
    {
        // Long enough for this project's own firmware and disk, which
        // reach the RT-11 prompt in about 700 frames.  Nothing types
        // anything, so firmware that stops at a boot menu will still be
        // sitting at it afterwards.
        const int kBootFrames = 1200;

        std::wcout << L"Booting..." << std::endl;
        Emulator_Reset();
        for (int i = 0; i < kBootFrames && !Screen_QuitRequested(); i++)
        {
            Emulator_SystemFrame();
            Screen_Frame();
        }
    }

    // Closed during the boot: nothing to serve, then.
    if (Screen_QuitRequested())
    {
        Screen_Close();
        Emulator_Done();
        return 0;
    }

    GdbServer_Run(options.port, options.okDebugCpu, options.frames);

    Screen_Close();
    Emulator_Done();
    return 0;
}


//////////////////////////////////////////////////////////////////////

#ifdef _MSC_VER
int wmain(int argc, wchar_t* argv[])
{
    _setmode(_fileno(stdout), _O_U16TEXT);

    std::vector<std::wstring> args;
    for (int argn = 1; argn < argc; argn++)
        args.push_back(std::wstring(argv[argn]));

    return Run(args);
}
#else

// Minimal UTF-8 -> wstring decoder for command-line arguments, used instead
// of the now-deprecated <codecvt>/std::wstring_convert (which is also
// missing std::codecvt_utf8_utf16 entirely on libc++/macOS). wchar_t is
// 32-bit on Linux/macOS, so each decoded code point maps to a single
// wchar_t -- no surrogate pairs to worry about here (that's only a
// concern for the 16-bit wchar_t used on the _MSC_VER branch above, which
// doesn't go through this function at all).
std::wstring Utf8ToWString(const char* s)
{
    std::wstring result;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
    while (*p != 0)
    {
        unsigned char c = *p;
        uint32_t codepoint = 0;
        int extraBytes = 0;
        if ((c & 0x80) == 0x00)      { codepoint = c; extraBytes = 0; }
        else if ((c & 0xE0) == 0xC0) { codepoint = c & 0x1F; extraBytes = 1; }
        else if ((c & 0xF0) == 0xE0) { codepoint = c & 0x0F; extraBytes = 2; }
        else if ((c & 0xF8) == 0xF0) { codepoint = c & 0x07; extraBytes = 3; }
        else { p++; continue; }  // Invalid lead byte, skip it

        p++;
        bool okValid = true;
        for (int i = 0; i < extraBytes; i++)
        {
            if ((*p & 0xC0) != 0x80) { okValid = false; break; }  // Truncated/invalid sequence
            codepoint = (codepoint << 6) | (*p & 0x3F);
            p++;
        }
        if (okValid)
            result.push_back((wchar_t)codepoint);
    }
    return result;
}

int main(int argc, char* argv[])
{
    // Prefer the environment's locale (for correct wide/UTF-8 console
    // I/O), but some platforms advertise a locale via LANG/LC_ALL that
    // their C++ runtime can't actually construct (seen on macOS with a
    // Homebrew-built libstdc++: std::locale("") throws even though
    // std::setlocale(LC_ALL, "") -- a plain libc call -- succeeds for the
    // same name) -- fall back to the classic "C" locale rather than crash
    // the whole program over console cosmetics.
    std::setlocale(LC_ALL, "");
    try
    {
        std::wcout.imbue(std::locale(""));
    }
    catch (const std::runtime_error&)
    {
        std::wcout.imbue(std::locale::classic());
    }

    std::vector<std::wstring> args;
    for (int argn = 1; argn < argc; argn++)
        args.push_back(Utf8ToWString(argv[argn]));

    return Run(args);
}
#endif
