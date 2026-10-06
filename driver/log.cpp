// Driver log implementation (see log.h).
#include "log.h"
#include <stdarg.h>

#define S2C_LOG_SIZE    65536           // characters kept (oldest lines are dropped)
#define S2C_PARAMS_KEY  L"Show2Cam\\Parameters"

static KSPIN_LOCK g_logLock;
static char       g_log[S2C_LOG_SIZE];
static ULONG      g_logLen;
static BOOLEAN    g_logDirty;
static KEVENT     g_fileLock;          // serialises the file writes (a KEVENT keeps us at PASSIVE_LEVEL)

static volatile LONG g_generation;     // changes with every line (S2cLogCopy)

// The log file: Parameters\LogFile (NT path, written by the installer), else the default below.
#ifndef RTL_QUERY_REGISTRY_TYPECHECK
#define RTL_QUERY_REGISTRY_TYPECHECK       0x00000100
#define RTL_QUERY_REGISTRY_TYPECHECK_SHIFT 24
#endif
#define S2C_LOG_FILE_DEFAULT L"\\??\\C:\\ProgramData\\Show2Cam\\logs\\driver.log"

static void WriteLogFile(const char* text, ULONG len)
{
    WCHAR pathBuf[260];
    UNICODE_STRING path = { 0, sizeof(pathBuf) - sizeof(WCHAR), pathBuf };
    RTL_QUERY_REGISTRY_TABLE q[2];
    RtlZeroMemory(q, sizeof(q));
    q[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_REQUIRED | RTL_QUERY_REGISTRY_TYPECHECK;
    q[0].DefaultType = REG_SZ << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT;
    q[0].Name = (PWSTR)L"LogFile";
    q[0].EntryContext = &path;
    if (!NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_SERVICES, S2C_PARAMS_KEY, q, nullptr, nullptr)) || path.Length == 0)
    {
        RtlInitUnicodeString(&path, S2C_LOG_FILE_DEFAULT);
    }

    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &path, OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE, nullptr, nullptr);
    IO_STATUS_BLOCK io;
    HANDLE file;
    // The whole ring is rewritten each time: the file always holds the current log (the folder is
    // created by the installer; until it exists, or early in boot, the write simply fails).
    if (NT_SUCCESS(ZwCreateFile(&file, GENERIC_WRITE | SYNCHRONIZE, &oa, &io, nullptr, FILE_ATTRIBUTE_NORMAL,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, FILE_OVERWRITE_IF,
                                FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE, nullptr, 0)))
    {
        ZwWriteFile(file, nullptr, nullptr, nullptr, &io, (PVOID)text, len, nullptr, nullptr);
        ZwClose(file);
    }
}


void S2cLogInit()
{
    KeInitializeSpinLock(&g_logLock);
    KeInitializeEvent(&g_fileLock, SynchronizationEvent, TRUE);
    g_logLen = 0;
    g_log[0] = 0;
}

void S2cLog(_In_z_ _Printf_format_string_ const char* Format, ...)
{
    char line[320];

    // Local wall-clock time stamp.
    LARGE_INTEGER sys, local;
    KeQuerySystemTimePrecise(&sys);
    ExSystemTimeToLocalTime(&sys, &local);
    TIME_FIELDS tf;
    RtlTimeToTimeFields(&local, &tf);

    int n = _snprintf(line, sizeof(line), "%04d-%02d-%02d %02d:%02d:%02d.%03d  ",
                      tf.Year, tf.Month, tf.Day, tf.Hour, tf.Minute, tf.Second, tf.Milliseconds);
    if (n < 0) n = 0;

    va_list args;
    va_start(args, Format);
    int m = _vsnprintf(line + n, sizeof(line) - n - 2, Format, args);
    va_end(args);
    if (m < 0) m = (int)(sizeof(line) - n - 3);     // truncated
    ULONG len = (ULONG)(n + m);
    line[len] = 0;

    DbgPrintEx(DPFLTR_IHVVIDEO_ID, DPFLTR_ERROR_LEVEL, "Show2Cam: %s\n", line + n);

    line[len++] = '\n';
    line[len] = 0;

    KIRQL irql;
    KeAcquireSpinLock(&g_logLock, &irql);
    if (g_logLen + len >= S2C_LOG_SIZE)
    {
        // Drop the oldest half, cut at a line boundary.
        ULONG cut = g_logLen / 2;
        while (cut < g_logLen && g_log[cut] != '\n') cut++;
        if (cut < g_logLen) cut++;
        RtlMoveMemory(g_log, g_log + cut, g_logLen - cut);
        g_logLen -= cut;
    }
    RtlCopyMemory(g_log + g_logLen, line, len);
    g_logLen += len;
    g_log[g_logLen] = 0;
    g_logDirty = TRUE;
    InterlockedIncrement(&g_generation);
    KeReleaseSpinLock(&g_logLock, irql);
}

ULONG S2cLogCopy(_Out_writes_bytes_(Max) char* Out, _In_ ULONG Max, _Out_ ULONG* Generation)
{
    KIRQL irql;
    KeAcquireSpinLock(&g_logLock, &irql);
    ULONG from = g_logLen > Max ? g_logLen - Max : 0;
    ULONG n = g_logLen - from;
    RtlCopyMemory(Out, g_log + from, n);
    *Generation = (ULONG)g_generation;
    KeReleaseSpinLock(&g_logLock, irql);
    return n;
}

static void FlushNow(BOOLEAN registry);

// While running nothing is written: the panel fetches the log (S2C_PROPERTY_LOG) and writes driver.log itself.
void S2cLogFlush()
{
}

void S2cLogFlushNow()
{
    FlushNow(TRUE);
}

// registry: also the DriverLog value (128 KB; only at start / removal: writing it often loads registry filters such as
// DLP agents', which watch the very programs that open cameras).
static void FlushNow(BOOLEAN registry)
{
    if (KeGetCurrentIrql() != PASSIVE_LEVEL || !g_logDirty)
    {
        return;
    }

    // Snapshot the log as UTF-16 (the text is ASCII) with CRLF line ends.
    SIZE_T cap = (SIZE_T)(S2C_LOG_SIZE * 2 + 2) * sizeof(WCHAR);
    WCHAR* w = (WCHAR*)ExAllocatePool2(POOL_FLAG_NON_PAGED, cap, S2C_POOLTAG);
    char* a = (char*)ExAllocatePool2(POOL_FLAG_NON_PAGED, S2C_LOG_SIZE * 2 + 2, S2C_POOLTAG);
    if (!w || !a)
    {
        if (w) ExFreePoolWithTag(w, S2C_POOLTAG);
        if (a) ExFreePoolWithTag(a, S2C_POOLTAG);
        return;
    }
    ULONG wl = 0;
    KIRQL irql;
    KeAcquireSpinLock(&g_logLock, &irql);
    for (ULONG i = 0; i < g_logLen; i++)
    {
        if (g_log[i] == '\n') { a[wl] = '\r'; w[wl++] = L'\r'; }
        a[wl] = g_log[i];
        w[wl++] = (WCHAR)(unsigned char)g_log[i];
    }
    w[wl] = 0;
    g_logDirty = FALSE;
    KeReleaseSpinLock(&g_logLock, irql);

    if (registry) RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, S2C_PARAMS_KEY, L"DriverLog", REG_SZ, w, (wl + 1) * sizeof(WCHAR));
    ExFreePoolWithTag(w, S2C_POOLTAG);

    KeWaitForSingleObject(&g_fileLock, Executive, KernelMode, FALSE, nullptr);
    WriteLogFile(a, wl);
    KeSetEvent(&g_fileLock, IO_NO_INCREMENT, FALSE);
    ExFreePoolWithTag(a, S2C_POOLTAG);
}

void S2cLogSetValue(_In_z_ PCWSTR Name, _In_ ULONG Value)
{
    if (KeGetCurrentIrql() == PASSIVE_LEVEL)
    {
        RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, S2C_PARAMS_KEY, Name, REG_DWORD, &Value, sizeof(Value));
    }
}
