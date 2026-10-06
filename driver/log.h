// Driver log: a ring of text lines kept in memory, mirrored to the kernel debugger (DebugView) and
// saved to HKLM\SYSTEM\CurrentControlSet\Services\Show2Cam\Parameters\DriverLog (REG_SZ) so the
// user-mode tools can show it, and to the file Parameters\LogFile (default
// C:\ProgramData\Show2Cam\logs\driver.log). S2cLog and S2cLogFlush may be called at IRQL <= DISPATCH_LEVEL.
#pragma once

#include "common.h"

void S2cLogInit();
void S2cLog(_In_z_ _Printf_format_string_ const char* Format, ...);
// Nothing while running (the panel fetches the log, S2cLogCopy / S2C_PROPERTY_LOG, and writes driver.log): a file or
// registry write from the driver could be held by a DLP file filter while its agent opens a camera.
void S2cLogFlush();
// Writes it to the registry and the file now (PASSIVE_LEVEL): DriverEntry and device start / removal only.
void S2cLogFlushNow();
// The newest bytes of the log (up to Max) and its generation (changes with every line).
ULONG S2cLogCopy(_Out_writes_bytes_(Max) char* Out, _In_ ULONG Max, _Out_ ULONG* Generation);

// Stores a DWORD next to the log (e.g. StartStatus, CamerasCreated). PASSIVE_LEVEL only.
void S2cLogSetValue(_In_z_ PCWSTR Name, _In_ ULONG Value);
