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

// Serve one gdb connection on `port`.
//
// Both processors are served: two processes to gdb, the central one
// process 1 and the peripheral one process 2, each with its own
// registers, its own address space and -- which is the point of
// processes rather than threads -- its own symbols. A gdb that does not
// ask for the multiprocess extension gets them as two threads instead,
// numbered the same. `okDebugCpu` only says which of them is selected
// before gdb has said anything.
//
// `maxFrames` bounds a single continue: a program that never stops would
// otherwise run for ever, which is right at a keyboard and wrong in a
// test harness. Zero means no bound. Reaching it is reported to gdb as
// SIGALRM, so a log says what happened.
//
// Returns when gdb disconnects, or immediately if the port cannot be
// listened on.
void GdbServer_Run(int port, bool okDebugCpu, int maxFrames);
