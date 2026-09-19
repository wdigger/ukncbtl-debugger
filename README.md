# ukncbtl-debugger

A UKNC for gdb to debug: the emulation core of
[UKNCBTL](https://github.com/nzeemin/ukncbtl) — the Soviet UKNC (УКНЦ, Электроника МС 0511)
educational computer — with a server for gdb's remote serial protocol in front of it. It links
against UKNCBTL's `emubase` directly, with no GUI dependency, so it builds and runs anywhere a
C++17 compiler does — Linux, macOS, and Windows.

There is no debugger of its own here any more. There used to be: a console port of the WinAPI
debugger's command dispatcher, with its own registers, memory, breakpoints, stepping,
disassembly, symbol table and source-line display. gdb does all of that, over the wire, against
the program's own source and types — so the commands went and the server stayed.

The gdb to talk to it is `pdp11-uknc-rt11-gdb`, from
[the uknc toolchain](https://github.com/wdigger/uknc): gdb has no pdp11 target of its own, and
that port supplies one.

## Building

Requires a C++17 compiler (GCC and Clang on Linux/macOS; MSVC/Visual Studio 2022 project files
are also included for Windows).

```sh
make            # release build (default) -> build/release/ukncbtldebug
make debug      # debug build -> build/debug/ukncbtldebug
make clean      # remove build artifacts
```

On Windows, open `ukncbtldebug.sln` in Visual Studio 2022.

## Running

```
ukncbtldebug [--disk1 FILE ... --disk4 FILE] [--rom FILE] [--port N] [--ppu]
             [--boot] [--screen] [--frames N]
```

| Option | Meaning |
|---|---|
| `--diskN FILE` | Attach a floppy image to drive `N` (1–4) |
| `--rom FILE` | The machine's firmware; the default is `uknc_rom.bin` in the current directory |
| `--port N` | Serve gdb on `localhost:N` (default 2345) |
| `--ppu` | Start with the peripheral processor selected; both are served either way |
| `--boot` | Bring the operating system up before listening, so that gdb's `load` has something to run under |
| `--screen` | Show the machine's screen in a window, and run at the speed that implies |
| `--frames N` | Give up on a program that has run this long, in frames of 1/50 second |

The UKNC has two PDP-11-style processors, a central one (CPU) and a peripheral one (PPU), with
separate address spaces, separate breakpoints and nothing shared between them. They are served
as gdb's two processes -- `info inferiors` lists both, `inferior 2` switches to the peripheral
one -- so each has a symbol table of its own, which is what lets a program loaded into the PPU
be debugged with its own symbols. A gdb that does not ask for the multiprocess extension gets
the same two as threads of one process instead.

While the window has the keyboard, keys go to the machine: the same table
UKNCBTL's own front ends use, and the machine's own register decides which of
its two alphabets a key means. They take effect as it runs -- one stopped at a
breakpoint takes what it was given at the next `continue`.

`--screen` needs SDL3 (`brew install sdl3`, `apt install libsdl3-dev`); the Makefile finds it
with pkg-config, and a build without it is the same build minus that option. The window is
resizable, and closing it stops the machine: that is the only way to say "enough" to a machine
you are watching, and gdb, if it is connected, sees the connection end.

`--frames` is for a harness that must not wait on a program that never ends — a program that
runs past it is reported to gdb as terminated. At a keyboard there is Ctrl-C and no reason for
a bound, which is the default.

It serves one connection and exits when gdb disconnects.

## A session

```
$ ukncbtldebug --disk1 rt11.dsk --port 2345
Listening on localhost:2345 for CPU first; in gdb say
  target remote :2345
```

```
$ pdp11-uknc-rt11-gdb prog.elf
(gdb) target extended-remote :2345
(gdb) set remote exec-file T
(gdb) break main
(gdb) run
Breakpoint 1, main () at g2.c:12
(gdb) next
13	  int a = sum(10);
(gdb) print a
$1 = 55
(gdb) bt
#0  main () at g2.c:13
(gdb) continue
55 210
[Inferior 1 (Remote target) exited normally]
```

## What the server answers

The subset of the protocol a stub has to: registers (`g`, `G`, `p`, `P`), memory (`m`, `M`,
and `X` for binary writes), continue and step (`c`, `s`, `vCont` including range stepping),
software breakpoints (`Z0`, `z0`), the opening negotiation and extended mode (`qSupported`,
`qAttached`, `!`, `vRun`, `R`), and `qRcmd` for the things below. Anything else gets the empty
reply that means "not implemented", which is always a valid answer — so gdb manages hardware
breakpoints and watchpoints itself rather than being told a lie.

Three things about it are particular to this machine.

**Starting a program.** RT-11 loads programs, not gdb, so `run` means what a person means by
it: reset, wait for the system to come up, type `R NAME` at the monitor, stop at the entry
point — which for an absolute `.sav` image is always 001000. `set remote exec-file NAME` says
which program. This needs firmware that boots without asking, since nothing types answers to
the stock firmware's boot menu; the uknc toolchain has one in `rom/`.

`load` is the other way in, and does not need the program to be on the disk at all: gdb writes
it into memory and sets the entry point.

**Output and the end of a program.** What the program prints reaches gdb's terminal in `O`
packets, taken where the central processor hands a byte to the peripheral one — channel 0 is
the terminal. Not where the program asks for it: `.TTYOUT` reports failure when the console is
busy and newlib retries the same character until it is taken, so watching the calls counts one
character hundreds of times.

`.EXIT` is how an RT-11 program ends, and the server watches for it — that is what tells gdb
the program finished, rather than waiting for a stop that was never coming. It happens while
the last of what was written is still queued, so the run carries on until that trickle stops.

**Things done to a machine rather than to a program.** A machine is switched off and on, typed
at, looked at, and no debugger has a packet for any of that. `monitor` is gdb's way of passing
a line through to the other end, and this is what the other end does with it:

| Command | |
|---|---|
| `monitor reset` | switch the machine off and on |
| `monitor keys TEXT` | type that on its keyboard, and Enter |
| `monitor frames N` | let it run N frames of 1/50 s |
| `monitor disk N FILE` | put a floppy image in drive N (1–4) |
| `monitor screen on`, `off` | show its screen in a window, or stop |
| `monitor screenshot FILE` | write the screen to FILE, as a BMP |

`frames` is the odd one: between stops the machine is not running, so nothing typed at it has
happened yet, and this is how time passes when there is no program to continue. A whole session
without gdb starting anything looks like

```
(gdb) monitor keys R DEMO
(gdb) monitor frames 300
(gdb) monitor screenshot /tmp/demo.bmp
```

and the program's own output comes back in the terminal as it goes. The screenshot is drawn
from the machine's memory rather than from anything on screen, so it works with no window open
and in a build with no SDL3 — which is the point: a program that draws is otherwise hard to
check from a script.

Registers, memory, breakpoints, stepping, frames, arguments and locals all work; the last three
come from the call frame information the compiler emits, so they need `-g`. A backtrace ends at
`main` with "previous frame inner to this frame": the startup code below it has no call frame
information, so there is nothing to unwind through.

## Platform notes

- `TCHAR` is `char` on non-Windows builds; the Win32 `wmain`/wide-string code paths only compile
  under MSVC. Console output uses UTF-16 wide mode on Windows and a UTF-8 locale elsewhere.

## Status

This is a work in progress, developed interactively.
