// Driver log: a ring of text lines kept in memory, mirrored to the kernel debugger (DebugView) and
// saved to HKLM\SYSTEM\CurrentControlSet\Services\Show2Cam\Parameters\DriverLog (REG_SZ) so the
// user-mode tools can show it, and to the file Parameters\LogFile (default
// C:\ProgramData\Show2Cam\logs\driver.log). S2cLog may be called at IRQL <= DISPATCH_LEVEL; S2cLogFlush only at
// PASSIVE_LEVEL (it is a no-op otherwise, the lines are written by the next flush).
#pragma once

#include "common.h"

void S2cLogInit();
void S2cLog(_In_z_ _Printf_format_string_ const char* Format, ...);
void S2cLogFlush();

// Stores a DWORD next to the log (e.g. StartStatus, CamerasCreated). PASSIVE_LEVEL only.
void S2cLogSetValue(_In_z_ PCWSTR Name, _In_ ULONG Value);
