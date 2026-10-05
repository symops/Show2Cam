// s2cinstall - console installer and test tool for the Show2Cam driver (administrator rights).
//
//   s2cinstall install                 trust the test certificates, create ROOT\Show2Cam, install Show2Cam.inf
//   s2cinstall remove                  remove the device and the driver packages
//   s2cinstall status                  device state, cameras (name, size, streaming, frames), driver log
//   s2cinstall cameras N               number of cameras (1..10) + device restart
//   s2cinstall size N WIDTH HEIGHT [FPS]   resolution of camera N + device restart
//   s2cinstall color N RRGGBB          show a solid colour on camera N (tests the picture path)
//   s2cinstall bmp N FILE.bmp          show a 24/32-bit BMP on camera N
//   s2cinstall pattern N               back to the test pattern
//   s2cinstall testsign on|off         Windows test signing mode (after a restart)
// Output in English; log in %ProgramData%\Show2Cam\logs\install.log.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <setupapi.h>
#include <newdev.h>
#include <cfgmgr32.h>
#include <wincrypt.h>
#include <shlobj.h>
#include <ks.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <io.h>
#include <fcntl.h>
#include "../driver/version.h"

static const wchar_t kHardwareId[] = L"ROOT\\Show2Cam";
static const wchar_t kParams[] = L"SYSTEM\\CurrentControlSet\\Services\\Show2Cam\\Parameters";
static const GUID kCategoryVideo = { 0x6994ad05, 0x93ef, 0x11d0, { 0xa3, 0xcc, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 } };
static const GUID kPropSetShow2Cam = { 0x6f1c2a9e, 0x3b57, 0x4e0c, { 0x9d, 0x1a, 0x5c, 0x2e, 0x7b, 0x3f, 0x8a, 0x41 } };
enum { S2C_PROPERTY_FRAME = 0, S2C_PROPERTY_STATUS = 1 };
struct S2C_FRAME_HEADER { ULONG Magic, Width, Height, Flags; };
struct S2C_STATUS
{
    ULONG Index, Width, Height, Fps, Streaming, SourceWidth, SourceHeight, Reserved;
    ULONGLONG FramesDelivered, FramesDropped, PicturesReceived;
};

static FILE* g_log;

static void Out(const wchar_t* fmt, ...)
{
    wchar_t buf[1024];
    va_list a;
    va_start(a, fmt);
    _vsnwprintf(buf, 1024, fmt, a);
    va_end(a);
    buf[1023] = 0;
    wprintf(L"%ls\n", buf);
    if (g_log)
    {
        SYSTEMTIME t;
        GetLocalTime(&t);
        fwprintf(g_log, L"%04u-%02u-%02u %02u:%02u:%02u.%03u  %ls\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                 t.wSecond, t.wMilliseconds, buf);
        fflush(g_log);
    }
}

static void OpenLog()
{
    wchar_t dir[MAX_PATH];
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, dir))) return;
    wcscat(dir, L"\\Show2Cam");
    CreateDirectoryW(dir, nullptr);
    wcscat(dir, L"\\logs");
    CreateDirectoryW(dir, nullptr);
    wcscat(dir, L"\\install.log");
    g_log = _wfopen(dir, L"a, ccs=UTF-8");
}

static void Fail(const wchar_t* what, DWORD err)
{
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err,
                   0, (LPWSTR)&msg, 0, nullptr);
    if (msg) for (wchar_t* p = msg; *p; p++) if (*p == L'\r' || *p == L'\n') *p = L' ';
    Out(L"error: %ls (0x%08lX) %ls", what, (unsigned long)err, msg ? msg : L"");
    if (msg) LocalFree(msg);
}

static void Sibling(const wchar_t* name, wchar_t* path)
{
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* s = wcsrchr(path, L'\\');
    wcscpy(s ? s + 1 : path, name);
}

// ---------------------------------------------------------------------------
// Device

static bool HasHardwareId(HDEVINFO set, SP_DEVINFO_DATA* info)
{
    wchar_t ids[2048] = {};
    if (!SetupDiGetDeviceRegistryPropertyW(set, info, SPDRP_HARDWAREID, nullptr, (BYTE*)ids, sizeof(ids) - 4, nullptr)) return false;
    for (const wchar_t* p = ids; *p; p += wcslen(p) + 1)
        if (_wcsicmp(p, kHardwareId) == 0) return true;
    return false;
}

template <class F> static int ForEachDevice(F fn)
{
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    SP_DEVINFO_DATA info;
    info.cbSize = sizeof(info);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); i++)
        if (HasHardwareId(set, &info)) { n++; fn(set, &info); }
    SetupDiDestroyDeviceInfoList(set);
    return n;
}

static bool AddCertificate(const wchar_t* path, const wchar_t* storeName)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { Fail(path, GetLastError()); return false; }
    BYTE data[8192];
    DWORD len = 0;
    BOOL ok = ReadFile(f, data, sizeof(data), &len, nullptr);
    CloseHandle(f);
    if (!ok || !len) { Fail(path, GetLastError()); return false; }
    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, storeName);
    bool added = store && CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, data, len, CERT_STORE_ADD_REPLACE_EXISTING, nullptr);
    if (!added) Fail(storeName, GetLastError());
    if (store) CertCloseStore(store, 0);
    return added;
}

static int CmdInstall()
{
    wchar_t inf[MAX_PATH], root[MAX_PATH], pub[MAX_PATH];
    Sibling(L"Show2Cam.inf", inf);
    Sibling(L"Show2Cam.cer", root);
    Sibling(L"Show2Cam-Publisher.cer", pub);
    if (!AddCertificate(root, L"Root") || !AddCertificate(pub, L"TrustedPublisher")) return 1;
    Out(L"test certificates trusted");
    if (ForEachDevice([](HDEVINFO, SP_DEVINFO_DATA*) {}) == 0)
    {
        GUID cls;
        wchar_t className[64];
        if (!SetupDiGetINFClassW(inf, &cls, className, 64, nullptr)) { Fail(L"Show2Cam.inf", GetLastError()); return 1; }
        HDEVINFO set = SetupDiCreateDeviceInfoList(&cls, nullptr);
        SP_DEVINFO_DATA info;
        info.cbSize = sizeof(info);
        wchar_t hwid[64] = {};
        wcscpy(hwid, kHardwareId);
        bool ok = SetupDiCreateDeviceInfoW(set, className, &cls, nullptr, nullptr, DICD_GENERATE_ID, &info) &&
                  SetupDiSetDeviceRegistryPropertyW(set, &info, SPDRP_HARDWAREID, (BYTE*)hwid, (DWORD)((wcslen(hwid) + 2) * sizeof(wchar_t))) &&
                  SetupDiCallClassInstaller(DIF_REGISTERDEVICE, set, &info);
        DWORD err = GetLastError();
        SetupDiDestroyDeviceInfoList(set);
        if (!ok) { Fail(L"create device", err); return 1; }
        Out(L"device %ls created", kHardwareId);
    }
    Out(L"installing the driver (if Windows asks about the publisher: \"Install this driver software anyway\")...");
    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kHardwareId, inf, INSTALLFLAG_FORCE, &reboot))
    {
        Fail(L"install driver", GetLastError());
        return 1;
    }
    Out(reboot ? L"driver installed, Windows asks for a restart" : L"driver installed");
    return 0;
}

static int CmdRemove()
{
    int n = ForEachDevice([](HDEVINFO set, SP_DEVINFO_DATA* info) {
        if (!SetupDiCallClassInstaller(DIF_REMOVE, set, info)) Fail(L"remove device", GetLastError());
    });
    Out(L"devices removed: %d", n);
    wchar_t dir[MAX_PATH], pattern[MAX_PATH];
    GetWindowsDirectoryW(dir, MAX_PATH);
    _snwprintf(pattern, MAX_PATH, L"%ls\\INF\\oem*.inf", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            wchar_t path[MAX_PATH], service[64] = {};
            _snwprintf(path, MAX_PATH, L"%ls\\INF\\%ls", dir, fd.cFileName);
            GetPrivateProfileStringW(L"Show2Cam_Device.NT.Services", L"AddService", L"", service, 64, path);
            if (_wcsnicmp(service, L"Show2Cam", 8) != 0) continue;
            if (SetupUninstallOEMInfW(fd.cFileName, SUOI_FORCEDELETE, nullptr)) Out(L"driver package %ls removed", fd.cFileName);
            else Fail(fd.cFileName, GetLastError());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return 0;
}

static bool RestartDevice()
{
    bool ok = true;
    int n = ForEachDevice([&](HDEVINFO set, SP_DEVINFO_DATA* info) {
        static const DWORD states[2] = { DICS_DISABLE, DICS_ENABLE };
        for (DWORD state : states)
        {
            SP_PROPCHANGE_PARAMS p = {};
            p.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
            p.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
            p.StateChange = state;
            p.Scope = DICS_FLAG_GLOBAL;
            if (!SetupDiSetClassInstallParamsW(set, info, &p.ClassInstallHeader, sizeof(p)) ||
                !SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, info))
            {
                Fail(state == DICS_DISABLE ? L"disable device" : L"enable device", GetLastError());
                ok = false;
            }
        }
    });
    if (!n) Out(L"error: the Show2Cam device is not installed");
    else if (ok) Out(L"device restarted");
    return n && ok;
}

static bool SetParam(const wchar_t* name, DWORD value)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kParams, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
    {
        Fail(L"registry", GetLastError());
        return false;
    }
    LSTATUS rs = RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value));
    RegCloseKey(key);
    return rs == ERROR_SUCCESS;
}

// ---------------------------------------------------------------------------
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

// Cameras (device interfaces "...#Camera<N>" of KSCATEGORY_VIDEO)

static bool CameraPath(int n, wchar_t* path, size_t len, wchar_t* name, size_t nameLen)
{
    HDEVINFO set = SetupDiGetClassDevsW(&kCategoryVideo, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return false;
    wchar_t want[24];
    _snwprintf(want, 24, L"\\camera%d", n);
    bool found = false;
    SP_DEVICE_INTERFACE_DATA di = { sizeof(di) };
    for (DWORD i = 0; !found && SetupDiEnumDeviceInterfaces(set, nullptr, &kCategoryVideo, i, &di); i++)
    {
        wchar_t found_path[1024];
        if (!IsShow2CamInterface(set, &di, found_path, 1024)) continue;
        wchar_t lower[1024];
        wcscpy(lower, found_path);
        _wcslwr(lower);
        size_t L = wcslen(lower), W = wcslen(want);
        if (L >= W && wcscmp(lower + L - W, want) == 0)
        {
            wcsncpy(path, found_path, len - 1);
            path[len - 1] = 0;
            if (name)
            {
                name[0] = 0;
                HKEY k = SetupDiOpenDeviceInterfaceRegKey(set, &di, 0, KEY_READ);
                if (k != INVALID_HANDLE_VALUE)
                {
                    DWORD sz = (DWORD)(nameLen * sizeof(wchar_t));
                    RegGetValueW(k, nullptr, L"FriendlyName", RRF_RT_REG_SZ, nullptr, name, &sz);
                    RegCloseKey(k);
                }
            }
            found = true;
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

static HANDLE OpenCamera(int n)
{
    wchar_t path[1024];
    if (!CameraPath(n, path, 1024, nullptr, 0))
    {
        Out(L"error: camera %d not found (is the driver installed and CameraCount >= %d?)", n, n);
        return INVALID_HANDLE_VALUE;
    }
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) Fail(L"open camera", GetLastError());
    return h;
}

static bool Property(HANDLE h, ULONG id, ULONG flags, void* data, DWORD size, DWORD* returned)
{
    KSPROPERTY p = {};
    p.Set = kPropSetShow2Cam;
    p.Id = id;
    p.Flags = flags;
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD got = 0;
    BOOL ok = DeviceIoControl(h, IOCTL_KS_PROPERTY, &p, sizeof(p), data, size, &got, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(h, &ov, &got, TRUE);
    DWORD err = GetLastError();
    CloseHandle(ov.hEvent);
    if (returned) *returned = got;
    SetLastError(err);
    return ok != FALSE;
}

static int SendPicture(int n, const ULONG* bgra, ULONG w, ULONG h)
{
    HANDLE cam = OpenCamera(n);
    if (cam == INVALID_HANDLE_VALUE) return 1;
    SIZE_T bytes = sizeof(S2C_FRAME_HEADER) + (SIZE_T)w * h * 4;
    BYTE* buf = (BYTE*)HeapAlloc(GetProcessHeap(), 0, bytes);
    S2C_FRAME_HEADER* hdr = (S2C_FRAME_HEADER*)buf;
    hdr->Magic = 0x4D415246;
    hdr->Width = w;
    hdr->Height = h;
    hdr->Flags = 0;
    if (w && h) memcpy(hdr + 1, bgra, (SIZE_T)w * h * 4);
    bool ok = Property(cam, S2C_PROPERTY_FRAME, KSPROPERTY_TYPE_SET, buf, (DWORD)bytes, nullptr);
    if (!ok) Fail(L"send picture", GetLastError());
    else Out(w ? L"camera %d: picture %lux%lu sent" : L"camera %d: back to the test pattern", n, w, h);
    HeapFree(GetProcessHeap(), 0, buf);
    CloseHandle(cam);
    return ok ? 0 : 1;
}

static int CmdColor(int n, const wchar_t* hex)
{
    ULONG rgb = wcstoul(hex, nullptr, 16) & 0xFFFFFF;
    const ULONG w = 640, h = 360;
    ULONG* px = (ULONG*)HeapAlloc(GetProcessHeap(), 0, w * h * 4);
    for (ULONG i = 0; i < w * h; i++) px[i] = 0xFF000000 | rgb;
    int rc = SendPicture(n, px, w, h);
    HeapFree(GetProcessHeap(), 0, px);
    return rc;
}

static int CmdBmp(int n, const wchar_t* file)
{
    HANDLE f = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) { Fail(file, GetLastError()); return 1; }
    DWORD size = GetFileSize(f, nullptr), got = 0;
    BYTE* data = (BYTE*)HeapAlloc(GetProcessHeap(), 0, size);
    ReadFile(f, data, size, &got, nullptr);
    CloseHandle(f);
    auto* fh = (BITMAPFILEHEADER*)data;
    auto* ih = (BITMAPINFOHEADER*)(data + sizeof(BITMAPFILEHEADER));
    if (got < sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER) || fh->bfType != 0x4D42 || ih->biCompression != BI_RGB ||
        (ih->biBitCount != 24 && ih->biBitCount != 32))
    {
        Out(L"error: only uncompressed 24/32-bit BMP files");
        return 1;
    }
    ULONG w = (ULONG)ih->biWidth, h = (ULONG)(ih->biHeight < 0 ? -ih->biHeight : ih->biHeight);
    bool bottomUp = ih->biHeight > 0;
    ULONG stride = ((w * ih->biBitCount / 8) + 3) & ~3u;
    ULONG* px = (ULONG*)HeapAlloc(GetProcessHeap(), 0, (SIZE_T)w * h * 4);
    for (ULONG y = 0; y < h; y++)
    {
        const BYTE* row = data + fh->bfOffBits + (SIZE_T)(bottomUp ? h - 1 - y : y) * stride;
        for (ULONG x = 0; x < w; x++)
        {
            const BYTE* p = row + x * (ih->biBitCount / 8);
            px[y * w + x] = 0xFF000000 | (p[2] << 16) | (p[1] << 8) | p[0];
        }
    }
    int rc = SendPicture(n, px, w, h);
    HeapFree(GetProcessHeap(), 0, px);
    HeapFree(GetProcessHeap(), 0, data);
    return rc;
}

static void PrintDriverLog()
{
    DWORD size = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kParams, L"DriverLog", RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS)
    {
        Out(L"driver log: empty (the driver never started)");
        return;
    }
    wchar_t* text = (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 2);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, kParams, L"DriverLog", RRF_RT_REG_SZ, nullptr, text, &size) == ERROR_SUCCESS)
    {
        Out(L"---- driver log ----");
        wchar_t* ctx = nullptr;
        for (wchar_t* line = wcstok(text, L"\r\n", &ctx); line; line = wcstok(nullptr, L"\r\n", &ctx)) Out(L"  %ls", line);
    }
    HeapFree(GetProcessHeap(), 0, text);
}

static int CmdStatus()
{
    Out(L"Show2Cam %ls", L"" S2C_VER_STR);
    int devices = 0;
    ForEachDevice([&](HDEVINFO set, SP_DEVINFO_DATA* info) {
        wchar_t id[256] = {};
        SetupDiGetDeviceInstanceIdW(set, info, id, 256, nullptr);
        ULONG status = 0, problem = 0;
        if (CM_Get_DevNode_Status(&status, &problem, info->DevInst, 0) != CR_SUCCESS) { status = 0; problem = CM_PROB_PHANTOM; }
        Out(L"device %ls: %ls (problem code %lu)", id, problem ? L"NOT working" : (status & DN_STARTED) ? L"started" : L"not started", problem);
        devices++;
    });
    if (!devices) Out(L"device: not installed");
    for (int n = 1; n <= 10; n++)
    {
        wchar_t path[1024], name[256];
        if (!CameraPath(n, path, 1024, name, 256)) continue;
        HANDLE cam = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        S2C_STATUS s = {};
        DWORD got = 0;
        if (cam != INVALID_HANDLE_VALUE && Property(cam, S2C_PROPERTY_STATUS, KSPROPERTY_TYPE_GET, &s, sizeof(s), &got))
            Out(L"camera %d \"%ls\": %lux%lu %lu fps, %ls, picture %ls, frames %llu (dropped %llu)", n, name, s.Width, s.Height,
                s.Fps, s.Streaming ? L"STREAMING" : L"idle", s.SourceWidth ? L"from the program" : L"test pattern",
                s.FramesDelivered, s.FramesDropped);
        else
            Out(L"camera %d \"%ls\": status not available (0x%08lX)", n, name, (unsigned long)GetLastError());
        if (cam != INVALID_HANDLE_VALUE) CloseHandle(cam);
    }
    PrintDriverLog();
    return devices ? 0 : 1;
}

static int CmdTestSign(bool on)
{
    wchar_t sys[MAX_PATH], cmd[MAX_PATH + 64];
    GetSystemDirectoryW(sys, MAX_PATH);
    _snwprintf(cmd, MAX_PATH + 64, L"\"%ls\\bcdedit.exe\" /set {current} testsigning %ls", sys, on ? L"on" : L"off");
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { Fail(L"bcdedit", GetLastError()); return 1; }
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    Out(code == 0 ? L"test signing mode %ls after a restart" : L"error: bcdedit failed (Secure Boot on?)", on ? L"on" : L"off");
    return code == 0 ? 0 : 1;
}

static void Usage()
{
    Out(L"Show2Cam %ls - driver installer and test tool (run as administrator)", L"" S2C_VER_STR);
    Out(L"  s2cinstall install | remove | status");
    Out(L"  s2cinstall cameras N                      (1..10, restarts the device)");
    Out(L"  s2cinstall size N WIDTH HEIGHT [FPS]      (camera N, restarts the device)");
    Out(L"  s2cinstall color N RRGGBB | bmp N FILE.bmp | pattern N");
    Out(L"  s2cinstall testsign on|off");
}

int wmain(int argc, wchar_t** argv)
{
    _setmode(_fileno(stdout), _O_U16TEXT);
    OpenLog();
    if (g_log)
    {
        wchar_t line[512] = L"";
        for (int i = 1; i < argc; i++) { wcsncat(line, L" ", 511 - wcslen(line)); wcsncat(line, argv[i], 511 - wcslen(line)); }
        fwprintf(g_log, L"==== s2cinstall %ls:%ls ====\n", L"" S2C_VER_STR, line);
    }
    const wchar_t* cmd = argc > 1 ? argv[1] : L"";
    int rc = 2;
    if (!_wcsicmp(cmd, L"install")) rc = CmdInstall();
    else if (!_wcsicmp(cmd, L"remove")) rc = CmdRemove();
    else if (!_wcsicmp(cmd, L"status")) rc = CmdStatus();
    else if (!_wcsicmp(cmd, L"cameras") && argc == 3)
    {
        int n = _wtoi(argv[2]);
        if (n < 1 || n > 10) Out(L"error: 1..10");
        else rc = SetParam(L"CameraCount", (DWORD)n) && RestartDevice() ? 0 : 1;
    }
    else if (!_wcsicmp(cmd, L"size") && (argc == 5 || argc == 6))
    {
        int n = _wtoi(argv[2]);
        wchar_t name[32];
        _snwprintf(name, 32, L"Camera%dWidth", n);
        bool ok = SetParam(name, (DWORD)_wtoi(argv[3]));
        _snwprintf(name, 32, L"Camera%dHeight", n);
        ok = ok && SetParam(name, (DWORD)_wtoi(argv[4]));
        if (argc == 6) { _snwprintf(name, 32, L"Camera%dFps", n); ok = ok && SetParam(name, (DWORD)_wtoi(argv[5])); }
        rc = ok && RestartDevice() ? 0 : 1;
    }
    else if (!_wcsicmp(cmd, L"color") && argc == 4) rc = CmdColor(_wtoi(argv[2]), argv[3]);
    else if (!_wcsicmp(cmd, L"bmp") && argc == 4) rc = CmdBmp(_wtoi(argv[2]), argv[3]);
    else if (!_wcsicmp(cmd, L"pattern") && argc == 3) rc = SendPicture(_wtoi(argv[2]), nullptr, 0, 0);
    else if (!_wcsicmp(cmd, L"testsign") && argc == 3) rc = CmdTestSign(!_wcsicmp(argv[2], L"on"));
    else Usage();
    if (g_log) fclose(g_log);
    return rc;
}
