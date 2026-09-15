// Emulator.cpp

#include "stdafx.h"
#include "ukncbtldebug.h"
#include "Emulator.h"
#include "emubase/Emubase.h"


//////////////////////////////////////////////////////////////////////


CMotherboard* g_pBoard = nullptr;

static bool g_okEmulatorInitialized = false;
bool g_okEmulatorRunning = false;

int m_wEmulatorCPUBpsCount = 0;
int m_wEmulatorPPUBpsCount = 0;
uint16_t m_EmulatorCPUBps[MAX_BREAKPOINTCOUNT + 1];
uint16_t m_EmulatorPPUBps[MAX_BREAKPOINTCOUNT + 1];
uint16_t m_wEmulatorTempCPUBreakpoint = 0177777;
uint16_t m_wEmulatorTempPPUBreakpoint = 0177777;


static int m_nTickCount = 0;


static const int KEYEVENT_QUEUE_SIZE = 32;
static uint16_t m_EmulatorKeyQueue[KEYEVENT_QUEUE_SIZE];
static int m_EmulatorKeyQueueTop = 0;
static int m_EmulatorKeyQueueBottom = 0;
static int m_EmulatorKeyQueueCount = 0;

// The queue is drained once a frame; nothing outside this file touches it.
static void Emulator_ProcessKeyEvent();
static uint16_t Emulator_GetKeyEventFromQueue();



//////////////////////////////////////////////////////////////////////


bool Emulator_Init(const std::wstring& romFile)
{
    ASSERT(g_pBoard == nullptr);

    CProcessor::Init();

    m_wEmulatorCPUBpsCount = m_wEmulatorPPUBpsCount = 0;
    for (int i = 0; i <= MAX_BREAKPOINTCOUNT; i++)
    {
        m_EmulatorCPUBps[i] = 0177777;
        m_EmulatorPPUBps[i] = 0177777;
        //uint16_t address = Settings_GetDebugBreakpoint(i, true);
        //m_EmulatorCPUBps[i] = address;
        //if (address != 0177777) m_wEmulatorCPUBpsCount = i + 1;
        //address = Settings_GetDebugBreakpoint(i, false);
        //m_EmulatorPPUBps[i] = address;
        //if (address != 0177777) m_wEmulatorPPUBpsCount = i + 1;
    }

    g_pBoard = new CMotherboard();

    uint8_t buffer[32768];

    // Load ROM file
    memset(buffer, 0, 32768);
    std::string romPath = romFile.empty()
        ? std::string("uknc_rom.bin")
        : WStringToNarrowString(romFile);
    FILE* fpRomFile = ::fopen(romPath.c_str(), "rb");
    if (fpRomFile == nullptr)
    {
        std::wcout << L"Could not open ROM file " << romPath.c_str() << std::endl;
        return false;
    }
    size_t bytesToRead = 32256;
    size_t dwBytesRead = ::fread(buffer, 1, bytesToRead, fpRomFile);
    if (dwBytesRead != bytesToRead)
    {
        ::fclose(fpRomFile);
        std::wcout << L"Could not read ROM file " << romPath.c_str() << std::endl;
        return false;
    }
    ::fclose(fpRomFile);

    g_pBoard->LoadROM(buffer);

    //g_pBoard->SetNetStation((uint16_t)Settings_GetNetStation());

    g_pBoard->SetSoundGenCallback(nullptr);

    g_pBoard->Reset();

    g_okEmulatorInitialized = true;
    return true;
}

void Emulator_Done()
{
    ASSERT(g_pBoard != nullptr);

    //// Save breakpoints
    //for (int i = 0; i < MAX_BREAKPOINTCOUNT; i++)
    //    Settings_SetDebugBreakpoint(i, true, i < m_wEmulatorCPUBpsCount ? m_EmulatorCPUBps[i] : 0177777);
    //for (int i = 0; i < MAX_BREAKPOINTCOUNT; i++)
    //    Settings_SetDebugBreakpoint(i, false, i < m_wEmulatorPPUBpsCount ? m_EmulatorPPUBps[i] : 0177777);

    CProcessor::Done();

    g_pBoard->SetSoundGenCallback(nullptr);

    delete g_pBoard;
    g_pBoard = nullptr;

    g_okEmulatorInitialized = false;
}

void Emulator_Start()
{
    g_okEmulatorRunning = true;

    m_nTickCount = 0;

    // For proper breakpoint processing
    if (m_wEmulatorCPUBpsCount != 0 || m_wEmulatorPPUBpsCount)
    {
        g_pBoard->GetCPU()->ClearInternalTick();
        g_pBoard->GetPPU()->ClearInternalTick();
    }
}
void Emulator_Stop()
{
    g_okEmulatorRunning = false;

    Emulator_SetTempCPUBreakpoint(0177777);
    Emulator_SetTempPPUBreakpoint(0177777);
}

void Emulator_Reset()
{
    ASSERT(g_pBoard != nullptr);

    g_pBoard->Reset();
}

bool Emulator_AddCPUBreakpoint(uint16_t address)
{
    if (m_wEmulatorCPUBpsCount == MAX_BREAKPOINTCOUNT - 1 || address == 0177777)
        return false;
    for (int i = 0; i < m_wEmulatorCPUBpsCount; i++)  // Check if the BP exists
    {
        if (m_EmulatorCPUBps[i] == address)
            return false;  // Already in the list
    }
    for (int i = 0; i < MAX_BREAKPOINTCOUNT; i++)  // Put in the first empty cell
    {
        if (m_EmulatorCPUBps[i] > address)  // found the place
        {
            memcpy(m_EmulatorCPUBps + i + 1, m_EmulatorCPUBps + i, sizeof(uint16_t) * (m_wEmulatorCPUBpsCount - i));
            m_EmulatorCPUBps[i] = address;
            break;
        }
        if (m_EmulatorCPUBps[i] == 0177777)
        {
            m_EmulatorCPUBps[i] = address;
            break;
        }
    }
    m_wEmulatorCPUBpsCount++;
    return true;
}
bool Emulator_AddPPUBreakpoint(uint16_t address)
{
    if (m_wEmulatorPPUBpsCount == MAX_BREAKPOINTCOUNT - 1 || address == 0177777)
        return false;
    for (int i = 0; i < m_wEmulatorPPUBpsCount; i++)  // Check if the BP exists
    {
        if (m_EmulatorPPUBps[i] == address)
            return false;  // Already in the list
    }
    for (int i = 0; i < MAX_BREAKPOINTCOUNT; i++)  // Put in the first empty cell
    {
        if (m_EmulatorPPUBps[i] > address)  // found the place
        {
            memcpy(m_EmulatorPPUBps + i + 1, m_EmulatorPPUBps + i, sizeof(uint16_t) * (m_wEmulatorPPUBpsCount - i));
            m_EmulatorPPUBps[i] = address;
            break;
        }
        if (m_EmulatorPPUBps[i] == 0177777)
        {
            m_EmulatorPPUBps[i] = address;
            break;
        }
    }
    m_wEmulatorPPUBpsCount++;
    return true;
}
bool Emulator_RemoveCPUBreakpoint(uint16_t address)
{
    if (m_wEmulatorCPUBpsCount == 0 || address == 0177777)
        return false;
    for (int i = 0; i < MAX_BREAKPOINTCOUNT; i++)
    {
        if (m_EmulatorCPUBps[i] == address)
        {
            m_EmulatorCPUBps[i] = 0177777;
            m_wEmulatorCPUBpsCount--;
            if (m_wEmulatorCPUBpsCount > i)  // fill the hole
            {
                memcpy(m_EmulatorCPUBps + i, m_EmulatorCPUBps + i + 1, sizeof(uint16_t) * (m_wEmulatorCPUBpsCount - i));
                m_EmulatorCPUBps[m_wEmulatorCPUBpsCount] = 0177777;
            }
            return true;
        }
    }
    return false;
}
bool Emulator_RemovePPUBreakpoint(uint16_t address)
{
    if (m_wEmulatorPPUBpsCount == 0 || address == 0177777)
        return false;
    for (int i = 0; i < MAX_BREAKPOINTCOUNT; i++)
    {
        if (m_EmulatorPPUBps[i] == address)
        {
            m_EmulatorPPUBps[i] = 0177777;
            m_wEmulatorPPUBpsCount--;
            if (m_wEmulatorPPUBpsCount > i)  // fill the hole
            {
                memcpy(m_EmulatorPPUBps + i, m_EmulatorPPUBps + i + 1, sizeof(uint16_t) * (m_wEmulatorPPUBpsCount - i));
                m_EmulatorPPUBps[m_wEmulatorPPUBpsCount] = 0177777;
            }
            return true;
        }
    }
    return false;
}
void Emulator_SetTempCPUBreakpoint(uint16_t address)
{
    if (m_wEmulatorTempCPUBreakpoint != 0177777)
        Emulator_RemoveCPUBreakpoint(m_wEmulatorTempCPUBreakpoint);
    if (address == 0177777)
    {
        m_wEmulatorTempCPUBreakpoint = 0177777;
        return;
    }
    for (int i = 0; i < MAX_BREAKPOINTCOUNT; i++)
    {
        if (m_EmulatorCPUBps[i] == address)
            return;  // We have regular breakpoint with the same address
    }
    m_wEmulatorTempCPUBreakpoint = address;
    m_EmulatorCPUBps[m_wEmulatorCPUBpsCount] = address;
    m_wEmulatorCPUBpsCount++;
}
void Emulator_SetTempPPUBreakpoint(uint16_t address)
{
    if (m_wEmulatorTempPPUBreakpoint != 0177777)
        Emulator_RemovePPUBreakpoint(m_wEmulatorTempPPUBreakpoint);
    if (address == 0177777)
    {
        m_wEmulatorTempPPUBreakpoint = 0177777;
        return;
    }
    for (int i = 0; i < MAX_BREAKPOINTCOUNT; i++)
    {
        if (m_EmulatorPPUBps[i] == address)
            return;  // We have regular breakpoint with the same address
    }
    m_wEmulatorTempPPUBreakpoint = address;
    m_EmulatorPPUBps[m_wEmulatorPPUBpsCount] = address;
    m_wEmulatorPPUBpsCount++;
}
bool Emulator_IsBreakpoint(bool okCpuPpu, uint16_t address)
{
    int bpsCount = okCpuPpu ? m_wEmulatorCPUBpsCount : m_wEmulatorPPUBpsCount;
    uint16_t* pbps = okCpuPpu ? m_EmulatorCPUBps : m_EmulatorPPUBps;
    if (bpsCount == 0)
        return false;
    for (int i = 0; i < bpsCount; i++)
    {
        if (address == pbps[i])
            return true;
    }
    return false;
}
bool Emulator_SystemFrame()
{
    Emulator_ProcessKeyEvent();

    g_pBoard->SetCPUBreakpoints(m_wEmulatorCPUBpsCount > 0 ? m_EmulatorCPUBps : nullptr);
    g_pBoard->SetPPUBreakpoints(m_wEmulatorPPUBpsCount > 0 ? m_EmulatorPPUBps : nullptr);

    if (!g_pBoard->SystemFrame())  // Breakpoint hit
    {
        Emulator_SetTempCPUBreakpoint(0177777);
        Emulator_SetTempPPUBreakpoint(0177777);
        return false;
    }

    return true;
}

void Emulator_KeyEvent(uint8_t keyscan, bool pressed)
{
    if (m_EmulatorKeyQueueCount == KEYEVENT_QUEUE_SIZE) return;  // Full queue

    uint16_t keyevent = MAKEWORD(keyscan, pressed ? 128 : 0);

    m_EmulatorKeyQueue[m_EmulatorKeyQueueTop] = keyevent;
    m_EmulatorKeyQueueTop++;
    if (m_EmulatorKeyQueueTop >= KEYEVENT_QUEUE_SIZE)
        m_EmulatorKeyQueueTop = 0;
    m_EmulatorKeyQueueCount++;
}

static uint16_t Emulator_GetKeyEventFromQueue()
{
    if (m_EmulatorKeyQueueCount == 0) return 0;  // Empty queue

    uint16_t keyevent = m_EmulatorKeyQueue[m_EmulatorKeyQueueBottom];
    m_EmulatorKeyQueueBottom++;
    if (m_EmulatorKeyQueueBottom >= KEYEVENT_QUEUE_SIZE)
        m_EmulatorKeyQueueBottom = 0;
    m_EmulatorKeyQueueCount--;

    return keyevent;
}

static void Emulator_ProcessKeyEvent()
{
    // Process next event in the keyboard queue
    uint16_t keyevent = Emulator_GetKeyEventFromQueue();
    if (keyevent != 0)
    {
        bool pressed = ((keyevent & 0x8000) != 0);
        uint8_t ukncscan = (uint8_t)(keyevent & 0xff);
        g_pBoard->KeyboardEvent(ukncscan, pressed);
    }
}

bool Emulator_AttachFloppyImage(int slot, LPCTSTR sFilePath)
{
    return g_pBoard->AttachFloppyImage(slot, sFilePath);
}
