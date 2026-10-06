// Driver log: a ring of text lines kept in memory, mirrored to the kernel debugger (DebugView) and
// saved to HKLM\SYSTEM\CurrentControlSet\Services\Show2Cam\Parameters\DriverLog (REG_SZ) so the
// user-mode tools can show it, and to the file Parameters\LogFile (default
// C:\ProgramData\Show2Cam\logs\driver.log). S2cLog and S2cLogFlush may be called at IRQL <= DISPATCH_LEVEL.
#pragma once

#include "common.h"

void S2cLogInit();
void S2cLog(_In_z_ _Printf_format_string_ const char* Format, ...);
// Writes the log out on a system worker thread (any IRQL <= DISPATCH_LEVEL; never blocks the caller).
void S2cLogFlush();
// Writes it out now, in this thread (PASSIVE_LEVEL): DriverEntry and PnP only, never in a program's camera calls.
void S2cLogFlushNow();
// The device objects the worker items are queued on (added at start, removed at removal).
void S2cLogSetDevice(_In_ PDEVICE_OBJECT Device, _In_ BOOLEAN Present);

// Stores a DWORD next to the log (e.g. StartStatus, CamerasCreated). PASSIVE_LEVEL only.
void S2cLogSetValue(_In_z_ PCWSTR Name, _In_ ULONG Value);
