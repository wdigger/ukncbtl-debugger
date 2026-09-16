// Screen.h -- the machine's screen, in a window
//
// The picture the machine is drawing, shown as it draws it.  SDL3 does
// the showing, and Screen.cpp is the only file that knows that; a build
// without SDL3 keeps every function here, doing nothing.
//
// Doing nothing is also the normal case at run time.  The emulator is
// usually driven by uknc-run or by an editor, where a window would be
// in the way, so there is none unless --screen asks for one, and every
// call below returns immediately when there is not.
//
// No thread of its own, and none wanted.  The machine only advances in
// the gdb server's own frame loop, so that is where the picture is
// drawn -- one place, one thread, nothing to lock.  While gdb holds the
// machine stopped nothing advances and the picture stands still, which
// is what a stopped machine looks like.
//
// Which leaves the waiting, and there is a lot of it: for gdb to
// connect, and then for whatever it has to say next.  A window whose
// events nobody answers is a window the desktop greys out and declares
// hung -- so every wait in the server is a wait in steps of a
// twentieth of a second with Screen_Idle() in between, and that is what
// keeps the window a window rather than a picture of one.

#pragma once

// Opens the window.  Returns false, having said why, if SDL cannot --
// which is not fatal to anything: the machine runs on unwatched.
bool Screen_Open();

// Draws the machine's screen, shows it, and lets the window manager
// have its say.  Called once per emulated frame.
void Screen_Frame();

// Waits out the rest of the machine's own 1/50 second, so that what is
// being watched goes at the speed it would go at.  Only for a program
// actually running under gdb: bringing the operating system up is a
// thousand frames of scrolling nobody needs in real time.
void Screen_Pace();

// The same without the drawing: for a wait.  The picture
// is already there and the machine is not moving; this only keeps the
// window from going grey while gdb thinks.
void Screen_Idle();

bool Screen_IsOpen();

// Whether the window has been closed from the desktop -- the red button,
// Cmd-Q, the window manager.  That is a way of saying "enough", so it
// ends the run: whoever is at the machine has no other way to stop it,
// and a machine still running behind a window that is gone would be a
// strange thing to leave behind.  gdb, if it is connected, sees the
// connection end, which it knows what to do with.
bool Screen_QuitRequested();

void Screen_Close();
