// Diagnostics implementation (see diag.h).
#include "diag.h"
#include "lang.h"
#include "applog.h"
#include <cfgmgr32.h>
#include <setupapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <wctype.h>

#define PARAMS_KEY L"SYSTEM\\CurrentControlSet\\Services\\Show2Cam\\Parameters"


static void Say(SetupLog log, void* ctx, const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf(buf, 512, fmt, args);
    va_end(args);
    buf[511] = 0;
    if (log) log(ctx, buf);
    else AppLog(L"%ls", buf);
}

static const wchar_t* ProblemText(ULONG p)
{
    switch (p)
    {
    case 1:  return TR(L"устройство не настроено");
    case 10: return TR(L"устройство не удалось запустить (драйвер вернул ошибку при старте)");
    case 18: return TR(L"драйвер нужно переустановить");
    case 22: return TR(L"устройство отключено");
    case 24: return TR(L"устройство отсутствует");
    case 28: return TR(L"драйвер не установлен");
    case 31: return TR(L"Windows не смогла загрузить драйвер для устройства");
    case 37: return TR(L"ошибка в DriverEntry драйвера");
    case 39: return TR(L"драйвер не загрузился (повреждён или несовместим)");
    case 43: return TR(L"драйвер сообщил об ошибке устройства");
    case 45: return TR(L"устройство сейчас не подключено");
    case 48: return TR(L"драйвер заблокирован политикой");
    case 52: return TR(L"Windows не смогла проверить подпись драйвера (включите тестовый режим)");
    default: return TR(L"см. код в Диспетчере устройств");
    }
}


// True when the interface belongs to a device driven by the Show2Cam service. The device instance is
// named after the class by Windows (ROOT\CAMERA\0000), so the interface path can't be matched by name.
static bool IsShow2CamInterface(HDEVINFO set, SP_DEVICE_INTERFACE_DATA* di, wchar_t* path, size_t len)
{
    BYTE buf[2048];
    auto* d = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf;
    d->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    SP_DEVINFO_DATA info = { sizeof(info) };
    if (!SetupDiGetDeviceInterfaceDetailW(set, di, d, sizeof(buf), nullptr, &info)) return false;
    wchar_t service[64] = L"";
    if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_SERVICE, nullptr, (BYTE*)service, sizeof(service) - 2, nullptr)
        || _wcsicmp(service, L"Show2Cam") != 0)
        return false;
    wcsncpy(path, d->DevicePath, len - 1);
    path[len - 1] = 0;
    return true;
}

// The Show2Cam cameras Windows offers to programs (video interfaces of ROOT\Show2Cam): how many, and (report)
// their names.
static int ScanCameras(SetupLog log, void* ctx, bool report)
{
    static const GUID kCategoryVideo = { 0x6994ad05, 0x93ef, 0x11d0, { 0xa3, 0xcc, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 } };
    HDEVINFO set = SetupDiGetClassDevsW(&kCategoryVideo, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    SP_DEVICE_INTERFACE_DATA di = { sizeof(di) };
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &kCategoryVideo, i, &di); i++)
    {
        wchar_t path[1024];
        if (!IsShow2CamInterface(set, &di, path, 1024)) continue;
        n++;
        if (!report) continue;
        wchar_t name[256] = L"?";
        HKEY k = SetupDiOpenDeviceInterfaceRegKey(set, &di, 0, KEY_READ);
        if (k != INVALID_HANDLE_VALUE)
        {
            DWORD sz = sizeof(name);
            RegGetValueW(k, nullptr, L"FriendlyName", RRF_RT_REG_SZ, nullptr, name, &sz);
            RegCloseKey(k);
        }
        Say(log, ctx, TR(L"Камера: «%ls»."), name);
        AppLog(L"camera interface %ls", path);
    }
    SetupDiDestroyDeviceInfoList(set);
    return n;
}

static DWORD ReadDword(const wchar_t* name, DWORD def)
{
    DWORD v = def, size = sizeof(v);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, PARAMS_KEY, name, RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return def;
    return v;
}

bool DiagRunningDriverVersion(wchar_t* running, size_t len)
{
    running[0] = 0;
    bool found = false;
    DWORD size = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, PARAMS_KEY, L"DriverLog", RRF_RT_REG_SZ, nullptr, nullptr, &size) == ERROR_SUCCESS && size)
    {
        wchar_t* text = (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 2);
        if (text && RegGetValueW(HKEY_LOCAL_MACHINE, PARAMS_KEY, L"DriverLog", RRF_RT_REG_SZ, nullptr, text, &size) == ERROR_SUCCESS)
        {
            // Last "DriverEntry: Show2Cam <version>," line.
            const wchar_t* key = L"DriverEntry: Show2Cam ";
            const wchar_t* last = nullptr;
            for (const wchar_t* p = wcsstr(text, key); p; p = wcsstr(p + 1, key)) last = p;
            if (last)
            {
                last += wcslen(key);
                size_t k = 0;
                while (last[k] && last[k] != L',' && !iswspace(last[k]) && k + 1 < len) { running[k] = last[k]; k++; }
                running[k] = 0;
                found = k > 0;
            }
        }
        if (text) HeapFree(GetProcessHeap(), 0, text);
    }
    return found;
}

bool DiagPackageDriverVersion(wchar_t* packaged, size_t len)
{
    wchar_t inf[MAX_PATH], ver[128] = L"";
    GetModuleFileNameW(nullptr, inf, MAX_PATH);
    wchar_t* slash = wcsrchr(inf, L'\\');
    wcscpy(slash ? slash + 1 : inf, L"Show2Cam.inf");
    GetPrivateProfileStringW(L"Version", L"DriverVer", L"", ver, 128, inf);
    const wchar_t* v = wcschr(ver, L',');
    packaged[0] = 0;
    if (!v || !v[1]) return false;
    wcsncpy(packaged, v + 1, len - 1);
    packaged[len - 1] = 0;
    return true;
}

// Version of the running driver (from its log) vs. the package next to this program.
static void CheckDriverVersion(SetupLog log, void* ctx)
{
    wchar_t running[64] = L"?";
    DiagRunningDriverVersion(running, 64);
    if (!running[0]) wcscpy(running, L"?");

    wchar_t packaged[64];
    if (DiagPackageDriverVersion(packaged, 64))
    {
        Say(log, ctx, TR(L"Запущен драйвер версии %ls, в пакете — %ls."), running, packaged);
        if (_wcsicmp(running, packaged) != 0)
            Say(log, ctx, TR(L"ВНИМАНИЕ: работает не та версия драйвера, что в пакете. Нажмите «Переустановить» или перезагрузите компьютер."));
    }
    else
    {
        Say(log, ctx, TR(L"Запущен драйвер версии %ls."), running);
    }
}

static void LogDriverLog()
{
    DWORD size = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, PARAMS_KEY, L"DriverLog", RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size == 0)
    {
        AppLog(TR(L"[driver] журнал драйвера пуст: драйвер ни разу не запускался"));
        return;
    }
    wchar_t* text = (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 2);
    if (!text) return;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, PARAMS_KEY, L"DriverLog", RRF_RT_REG_SZ, nullptr, text, &size) == ERROR_SUCCESS)
    {
        AppLog(TR(L"[driver] ---- журнал драйвера (DriverLog) ----"));
        AppLogText(L"[driver] ", text);
    }
    HeapFree(GetProcessHeap(), 0, text);
}

// Copies the last setupapi.dev.log section that mentions Show2Cam into the program log.
static void LogSetupApiSection()
{
    wchar_t path[MAX_PATH];
    GetWindowsDirectoryW(path, MAX_PATH);
    wcscat(path, L"\\INF\\setupapi.dev.log");
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        AppLog(TR(L"[setupapi] не удалось открыть %ls (%lu)"), path, GetLastError());
        return;
    }
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    const LONGLONG tail = 4 * 1024 * 1024;
    LONGLONG start = size.QuadPart > tail ? size.QuadPart - tail : 0;
    DWORD len = (DWORD)(size.QuadPart - start), got = 0;
    char* buf = (char*)HeapAlloc(GetProcessHeap(), 0, len + 1);
    if (buf)
    {
        LARGE_INTEGER pos;
        pos.QuadPart = start;
        SetFilePointerEx(f, pos, nullptr, FILE_BEGIN);
        ReadFile(f, buf, len, &got, nullptr);
        buf[got] = 0;

        // Find the last "show2cam" (case-insensitive), then the section around it.
        char* hit = nullptr;
        for (char* p = buf; *p; p++)
            if (_strnicmp(p, "show2cam", 8) == 0) hit = p;
        if (!hit)
        {
            AppLog(TR(L"[setupapi] в setupapi.dev.log нет записей о Show2Cam"));
        }
        else
        {
            char* sec = hit;
            while (sec > buf && strncmp(sec, ">>>  [", 6) != 0) sec--;
            char* end = strstr(hit, "<<<  [Exit status");
            if (end)
            {
                char* nl = strchr(end, '\n');
                end = nl ? nl + 1 : end + strlen(end);
            }
            else
            {
                end = buf + got;
            }
            *end = 0;
            int wn = MultiByteToWideChar(CP_ACP, 0, sec, -1, nullptr, 0);
            wchar_t* w = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, wn * sizeof(wchar_t));
            if (w)
            {
                MultiByteToWideChar(CP_ACP, 0, sec, -1, w, wn);
                AppLog(TR(L"[setupapi] ---- последняя секция setupapi.dev.log о Show2Cam ----"));
                AppLogText(L"[setupapi] ", w);
                HeapFree(GetProcessHeap(), 0, w);
            }
        }
        HeapFree(GetProcessHeap(), 0, buf);
    }
    CloseHandle(f);
}

static void LogServiceState(SetupLog log, void* ctx)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    SC_HANDLE svc = scm ? OpenServiceW(scm, L"Show2Cam", SERVICE_QUERY_STATUS) : nullptr;
    if (!svc)
    {
        Say(log, ctx, TR(L"Служба драйвера Show2Cam не зарегистрирована."));
    }
    else
    {
        SERVICE_STATUS st = {};
        QueryServiceStatus(svc, &st);
        Say(log, ctx, TR(L"Служба драйвера: %ls."), st.dwCurrentState == SERVICE_RUNNING ? TR(L"запущена")
                                               : st.dwCurrentState == SERVICE_STOPPED ? TR(L"остановлена") : TR(L"запускается/останавливается"));
        CloseServiceHandle(svc);
    }
    if (scm) CloseServiceHandle(scm);
}

DiagResult RunDiagnostics(SetupLog log, void* ctx)
{
    AppLog(TR(L"---- диагностика ----"));
    Say(log, ctx, TR(L"Тестовый режим подписи: %ls; Secure Boot: %ls."),
        SetupTestSigningEnabled() ? TR(L"включён") : TR(L"выключен"), SetupSecureBootEnabled() ? TR(L"включён") : TR(L"выключен"));

    ULONG status = 0, problem = 0;
    wchar_t id[256];
    DiagResult result;
    if (!SetupGetDeviceState(&status, &problem, id, 256))
    {
        Say(log, ctx, TR(L"Устройство ROOT\\Show2Cam не найдено: драйвер не установлен."));
        result = DiagNotInstalled;
    }
    else
    {
        AppLog(L"device %ls: status=0x%08lX problem=%lu", id, status, problem);
        LogServiceState(log, ctx);
        CheckDriverVersion(log, ctx);
        DWORD start = ReadDword(L"StartStatus", 0xFFFFFFFF), cameras = ReadDword(L"CamerasCreated", 0xFFFFFFFF);
        if (start != 0xFFFFFFFF)
            Say(log, ctx, TR(L"Драйвер при запуске создал камер: %lu (код 0x%08lX)."), cameras, start);

        if (problem != 0 || !(status & DN_STARTED))
        {
            Say(log, ctx, TR(L"Устройство не запущено: код %lu — %ls."), problem, ProblemText(problem));
            result = DiagDeviceProblem;
        }
        else
        {
            Say(log, ctx, TR(L"Устройство Show2Cam запущено."));
            int cameras = ScanCameras(log, ctx, true);
            if (cameras > 0) result = DiagOk;
            else
            {
                Say(log, ctx, TR(L"Драйвер работает, но Windows не показывает камеры Show2Cam."));
                result = DiagNoCameras;
            }
        }
        LogDriverLog();
    }
    LogSetupApiSection();
    AppLog(TR(L"---- конец диагностики (результат %d) ----"), (int)result);
    return result;
}

DiagResult WaitForCameras(DWORD timeoutMs)
{
    DWORD t0 = GetTickCount();
    for (;;)
    {
        ULONG status = 0, problem = 0;
        wchar_t id[256];
        DiagResult r;
        if (!SetupGetDeviceState(&status, &problem, id, 256)) r = DiagNotInstalled;
        else if (problem != 0 || !(status & DN_STARTED)) r = DiagDeviceProblem;
        else r = ScanCameras(nullptr, nullptr, false) > 0 ? DiagOk : DiagNoCameras;
        if (r == DiagOk || GetTickCount() - t0 > timeoutMs) return r;
        Sleep(500);
    }
}
