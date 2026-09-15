// util/GdbServer.h
//
// A gdb remote-protocol server, so that pdp11-uknc-rt11-gdb can drive the
// emulated machine: "target remote :2345" from gdb's side, "gdbserver"
// from this one.
//
// The protocol is the one documented in the gdb manual under "Remote
// Serial Protocol" -- packets of the form $data#xx over a stream, with
// the peer acknowledging each one. Only the subset a stub has to answer
// is implemented, which is enough for registers, memory, breakpoints,
// stepping and running; see GdbServer.cpp for exactly which packets and
// why each is there.
//
// The server owns the machine while it is attached: it accepts one
// connection, serves it until gdb detaches or the connection drops, and
// only then returns to the console prompt. Two debuggers stepping the
// same processor at once would answer each other's questions wrongly, so
// there is no attempt to do both.

#pragma once

//////////////////////////////////////////////////////////////////////

// Serve one gdb connection on `port`. `okDebugCpu` picks the processor:
// the two have separate address spaces and separate breakpoint lists,
// and gdb has one of each. Returns when gdb disconnects, or immediately
// if the port cannot be listened on.
void GdbServer_Run(int port, bool okDebugCpu);

// Type an ASCII line on the machine's keyboard, pumping frames so the
// machine consumes it -- "R T\r" and the like. Implemented by the console
// (commands.cpp), which owns the key tables; declared here so that the
// protocol code does not have to reach into them.
void GdbServer_TypeLine(const std::string& text);
