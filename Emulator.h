// Emulator.h

#pragma once

#include "ukncbtldebug.h"
#include "emubase/Board.h"

//////////////////////////////////////////////////////////////////////

const int MAX_BREAKPOINTCOUNT = 16;

extern CMotherboard* g_pBoard;

extern bool g_okEmulatorRunning;


//////////////////////////////////////////////////////////////////////

// Bring the machine up. `romFile` is the firmware image; empty means
// uknc_rom.bin in the current directory, which is where it has always
// been looked for.
bool Emulator_Init(const std::wstring& romFile);
void Emulator_Done();

bool Emulator_AddCPUBreakpoint(uint16_t address);
bool Emulator_AddPPUBreakpoint(uint16_t address);
bool Emulator_RemoveCPUBreakpoint(uint16_t address);
bool Emulator_RemovePPUBreakpoint(uint16_t address);
void Emulator_SetTempCPUBreakpoint(uint16_t address);
void Emulator_SetTempPPUBreakpoint(uint16_t address);
bool Emulator_IsBreakpoint(bool okCpuPpu, uint16_t address);

void Emulator_Start();
void Emulator_Stop();
void Emulator_Reset();
bool Emulator_SystemFrame();

void Emulator_KeyEvent(uint8_t keyPressed, bool pressed);

bool Emulator_AttachFloppyImage(int slot, LPCTSTR sFilePath);

//////////////////////////////////////////////////////////////////////
