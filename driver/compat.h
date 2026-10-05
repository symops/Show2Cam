// Declarations missing from the MinGW DDK headers. Used only for the syntax check on Linux
// (see check_syntax.sh); a real build with the Windows Driver Kit never takes this path.
#pragma once

#if defined(__MINGW32__)

#ifndef POOL_FLAG_NON_PAGED
typedef ULONG64 POOL_FLAGS;
#define POOL_FLAG_NON_PAGED 0x0000000000000040ULL
extern "C" NTKERNELAPI PVOID NTAPI ExAllocatePool2(POOL_FLAGS Flags, SIZE_T NumberOfBytes, ULONG Tag);
#endif

extern "C" NTKERNELAPI VOID NTAPI KeQuerySystemTimePrecise(PLARGE_INTEGER CurrentTime);

#ifndef DEVPROPKEY_DEFINED
#define DEVPROPKEY_DEFINED
typedef GUID DEVPROPGUID;
typedef ULONG DEVPROPID;
typedef struct _DEVPROPKEY { DEVPROPGUID fmtid; DEVPROPID pid; } DEVPROPKEY;
#endif
typedef ULONG DEVPROPTYPE;
#ifndef DEVPROP_TYPE_STRING
#define DEVPROP_TYPE_STRING 0x00000012
#endif
extern "C" NTKERNELAPI NTSTATUS NTAPI IoSetDeviceInterfacePropertyData(PUNICODE_STRING SymbolicLinkName, const DEVPROPKEY* PropertyKey,
                                                                      LCID Lcid, ULONG Flags, DEVPROPTYPE Type, ULONG Size, PVOID Data);

#endif
