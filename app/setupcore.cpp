// Shared install/uninstall logic (see setupcore.h).
#include "setupcore.h"
#include "applog.h"
#include "devctl.h"
#include <devpropdef.h>
#include "lang.h"
#include "../driver/version.h"     // S2C_AUTHOR (the INF provider)
#include <setupapi.h>
#include <newdev.h>
#include <wincrypt.h>
#include <cfgmgr32.h>
#include <winioctl.h>
#include <ks.h>
#include <stdio.h>
#include <wchar.h>
#include "../driver/s2cproto.h"

static const wchar_t kHardwareId[] = L"ROOT\\Show2Cam";

// Shows / hides camera `cam` (S2C_PROPERTY_SHOW) through camera 1's filter: a hidden camera's own interfaces are off.
static bool ShowCamera(int cam, bool show)
{
    static const GUID kCategoryVideo = { 0x6994ad05, 0x93ef, 0x11d0, { 0xa3, 0xcc, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 } };
    static const GUID kPropSet = { 0x6f1c2a9e, 0x3b57, 0x4e0c, { 0x9d, 0x1a, 0x5c, 0x2e, 0x7b, 0x3f, 0x8a, 0x41 } };
    HDEVINFO set = SetupDiGetClassDevsW(&kCategoryVideo, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return false;
    bool ok = false;
    SP_DEVICE_INTERFACE_DATA di = { sizeof(di) };
    for (DWORD i = 0; !ok && SetupDiEnumDeviceInterfaces(set, nullptr, &kCategoryVideo, i, &di); i++)
    {
        BYTE buf[2048];
        auto* d = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf;
        d->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA info = { sizeof(info) };
        if (!SetupDiGetDeviceInterfaceDetailW(set, &di, d, sizeof(buf), nullptr, &info)) continue;
        wchar_t service[64] = L"";
        if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_SERVICE, nullptr, (BYTE*)service, sizeof(service) - 2, nullptr) ||
            _wcsicmp(service, L"Show2Cam") != 0)
            continue;
        const wchar_t* ref = wcsrchr(d->DevicePath, L'\\');
        if (!ref || _wcsicmp(ref + 1, L"camera1") != 0) continue;
        HANDLE h = CreateFileW(d->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        KSPROPERTY p = {};
        p.Set = kPropSet;
        p.Id = S2C_PROPERTY_SHOW;
        p.Flags = KSPROPERTY_TYPE_SET;
        S2C_SHOW s = { (ULONG)cam, show ? 1u : 0u };
        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD got = 0;
        BOOL done = ov.hEvent && DeviceIoControl(h, IOCTL_KS_PROPERTY, &p, sizeof(p), &s, sizeof(s), &got, &ov);
        if (!done && ov.hEvent && GetLastError() == ERROR_IO_PENDING) done = GetOverlappedResult(h, &ov, &got, TRUE);
        if (ov.hEvent) CloseHandle(ov.hEvent);
        CloseHandle(h);
        ok = done != FALSE;
    }
    SetupDiDestroyDeviceInfoList(set);
    return ok;
}

static void Log(SetupLog log, void* ctx, const wchar_t* fmt, ...)
{
    if (!log) return;
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf(buf, 512, fmt, args);
    va_end(args);
    buf[511] = 0;
    log(ctx, buf);
}

// Explanations for SetupAPI / trust errors that FormatMessage has no text for.
static const wchar_t* KnownError(DWORD err)
{
    switch (err)
    {
    case 0xE000022F: return TR(L"в пакете драйвера нет подписанного каталога (.cat)");
    case 0xE0000247: return TR(L"Windows не смогла добавить пакет в хранилище драйверов");
    case 0xE0000203: return TR(L"не найден подходящий драйвер в INF");
    case 0x800B0100: return TR(L"у файла нет цифровой подписи");
    case 0x800B0109: return TR(L"сертификат подписи не доверенный (корневой сертификат не установлен)");
    case 0x800B0110: return TR(L"сертификат нельзя использовать для этой цели");
    case 0x80096010: return TR(L"подпись повреждена или файл изменён после подписи");
    case 0x800B0101: return TR(L"срок действия сертификата истёк или ещё не начался (проверьте часы)");
    default: return nullptr;
    }
}

static void LogError(SetupLog log, void* ctx, const wchar_t* what, DWORD err)
{
    const wchar_t* known = KnownError(err);
    if (known)
    {
        Log(log, ctx, TR(L"Ошибка: %ls (0x%08lX): %ls. Подробности: C:\\Windows\\INF\\setupapi.dev.log"),
            what, (unsigned long)err, known);
        return;
    }
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, (LPWSTR)&msg, 0, nullptr);
    if (msg)
    {
        for (wchar_t* p = msg; *p; p++)
            if (*p == L'\r' || *p == L'\n') *p = L' ';
    }
    Log(log, ctx, TR(L"Ошибка: %ls (0x%08lX) %ls"), what, (unsigned long)err, msg ? msg : L"");
    if (msg) LocalFree(msg);
}

// ---------------------------------------------------------------------------
// Certificate

static bool AddCertificate(const wchar_t* path, const wchar_t* storeName, SetupLog log, void* ctx)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        LogError(log, ctx, TR(L"не найден сертификат"), GetLastError());
        return false;
    }
    BYTE data[8192];
    DWORD len = 0;
    BOOL read = ReadFile(f, data, sizeof(data), &len, nullptr);
    CloseHandle(f);
    if (!read || len == 0)
    {
        LogError(log, ctx, TR(L"не удалось прочитать сертификат"), GetLastError());
        return false;
    }

    HCERTSTORE store = CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0, CERT_SYSTEM_STORE_LOCAL_MACHINE, storeName);
    bool ok = store &&
              CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, data, len, CERT_STORE_ADD_REPLACE_EXISTING, nullptr);
    if (!ok) LogError(log, ctx, storeName, GetLastError());
    if (store) CertCloseStore(store, 0);
    return ok;
}

bool SetupTrustCertificate(const wchar_t* rootCerPath, const wchar_t* publisherCerPath, SetupLog log, void* ctx)
{
    bool ok = AddCertificate(rootCerPath, L"Root", log, ctx) &&
              AddCertificate(publisherCerPath, L"TrustedPublisher", log, ctx);
    if (ok) Log(log, ctx, TR(L"Тестовые сертификаты Show2Cam добавлены в доверенные (корневой ЦС и издатель)."));
    return ok;
}

// ---------------------------------------------------------------------------
// Devices

static bool HasHardwareId(HDEVINFO set, SP_DEVINFO_DATA* info)
{
    wchar_t ids[2048] = {};
    if (!SetupDiGetDeviceRegistryPropertyW(set, info, SPDRP_HARDWAREID, nullptr, (BYTE*)ids, sizeof(ids) - 4, nullptr))
    {
        return false;
    }
    for (const wchar_t* p = ids; *p; p += wcslen(p) + 1)
    {
        if (_wcsicmp(p, kHardwareId) == 0) return true;
    }
    return false;
}

// Calls fn for every device (present or not) with our hardware ID; returns how many matched.
template <class F> static int ForEachDevice(F fn)
{
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
    if (set == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    SP_DEVINFO_DATA info;
    info.cbSize = sizeof(info);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); i++)
    {
        if (HasHardwareId(set, &info))
        {
            n++;
            fn(set, &info);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return n;
}

int SetupDeviceCount()
{
    return ForEachDevice([](HDEVINFO, SP_DEVINFO_DATA*) {});
}

bool SetupGetDeviceState(ULONG* status, ULONG* problem, wchar_t* instanceId, size_t idLen)
{
    // The device that tells most: a working one before one with a problem, before a camera switched off, before a
    // leftover registration.
    int best = 99;
    *status = 0;
    *problem = 0;
    if (idLen) instanceId[0] = 0;
    ForEachDevice([&](HDEVINFO set, SP_DEVINFO_DATA* info) {
        ULONG st = 0, pr = 0;
        if (CM_Get_DevNode_Status(&st, &pr, info->DevInst, 0) != CR_SUCCESS)
        {
            st = 0;
            pr = CM_PROB_PHANTOM;           // registered but not present
        }
        int rank = pr == CM_PROB_PHANTOM ? 3 : pr == CM_PROB_DISABLED ? 2 : (pr || !(st & DN_STARTED)) ? 1 : 0;
        if (rank >= best) return;
        best = rank;
        *status = st;
        *problem = pr;
        SetupDiGetDeviceInstanceIdW(set, info, instanceId, (DWORD)idLen, nullptr);
    });
    return best < 99;
}

// Tells the driver where to write its log file (Parameters\LogFile, an NT path): driver.log next to ours.
static void SetDriverLogFile()
{
    wchar_t path[MAX_PATH + 16];
    _snwprintf(path, MAX_PATH + 16, L"\\??\\%ls\\driver.log", AppLogDir());
    path[MAX_PATH + 15] = 0;
    RegSetKeyValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\Show2Cam\\Parameters", L"LogFile", REG_SZ, path,
                    (DWORD)((wcslen(path) + 1) * sizeof(wchar_t)));
}

// ---------------------------------------------------------------------------
// One device per camera (ROOT\Show2Cam, class Image): the camera is the device's Device Parameters\CameraIndex.

static const GUID kClassImage = { 0x6bdd1fc6, 0x810f, 0x11d0, { 0xbe, 0xc7, 0x08, 0x00, 0x2b, 0xe2, 0x09, 0x2f } };

static int DeviceCameraIndex(HDEVINFO set, SP_DEVINFO_DATA* info)
{
    HKEY k = SetupDiOpenDevRegKey(set, info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
    if (k == INVALID_HANDLE_VALUE) return -1;
    DWORD v = 0, size = sizeof(v);
    int index = RegGetValueW(k, nullptr, L"CameraIndex", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS && v < 10 ? (int)v : -1;
    RegCloseKey(k);
    return index;
}

static bool SetDeviceCameraIndex(HDEVINFO set, SP_DEVINFO_DATA* info, int index)
{
    HKEY k = SetupDiCreateDevRegKeyW(set, info, DICS_FLAG_GLOBAL, 0, DIREG_DEV, nullptr, nullptr);
    if (k == INVALID_HANDLE_VALUE) return false;
    DWORD v = (DWORD)index;
    bool ok = RegSetValueExW(k, L"CameraIndex", 0, REG_DWORD, (const BYTE*)&v, sizeof(v)) == ERROR_SUCCESS;
    RegCloseKey(k);
    return ok;
}

static void ApplyCameraStates(int count);

bool SetupSetCameraCount(int count, bool installDriver, bool* rebootNeeded, SetupLog log, void* ctx, int* hidden)
{
    *rebootNeeded = false;
    if (hidden) *hidden = 0;
    if (count < 0 || count > 10) return false;
    // First: a device that starts below (created, or restarted) tells from it whether its camera is shown.
    S2cSetParam(L"CameraCount", (DWORD)count);
    // Present devices only: leftovers of earlier installations (not present) do not hold a camera number.
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return false;
    // The existing devices and their cameras; a device without one (installed before one device per camera, or a
    // duplicate) gets the lowest free number.
    bool used[10] = {};
    int devIndex[64], devCount = 0, unassigned[64], unCount = 0;
    SP_DEVINFO_DATA info = { sizeof(info) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info) && devCount < 64; i++)
    {
        if (!HasHardwareId(set, &info)) continue;
        int cam = DeviceCameraIndex(set, &info);
        if (cam >= 0 && !used[cam]) used[cam] = true;
        else unassigned[unCount++] = (int)i;
        devIndex[devCount++] = (int)i;
    }
    for (int u = 0; u < unCount; u++)
    {
        SP_DEVINFO_DATA d = { sizeof(d) };
        if (!SetupDiEnumDeviceInfo(set, (DWORD)unassigned[u], &d)) continue;
        int cam = 0;
        while (cam < 10 && used[cam]) cam++;
        if (cam >= 10) cam = 9;
        used[cam] = true;
        SetDeviceCameraIndex(set, &d, cam);
        AppLog(L"camera devices: existing device -> camera %d", cam + 1);
    }
    SetupDiDestroyDeviceInfoList(set);
    bool ok = true;
    // Too many: the devices of the cameras above the count are removed, one at a time on a fresh list (removing a
    // device may change the positions in a device list).
    wchar_t done[16][200];          // devices already removed (one Windows keeps until a restart stays in the list)
    int doneCount = 0;
    for (int guard = 0; guard < 16; guard++)
    {
        HDEVINFO rs = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (rs == INVALID_HANDLE_VALUE) break;
        bool removed = false;
        SP_DEVINFO_DATA d = { sizeof(d) };
        for (DWORD i = 0; !removed && SetupDiEnumDeviceInfo(rs, i, &d); i++)
        {
            if (!HasHardwareId(rs, &d)) continue;
            int cam = DeviceCameraIndex(rs, &d);
            if (cam < count) continue;
            wchar_t id[200] = L"";
            SetupDiGetDeviceInstanceIdW(rs, &d, id, 200, nullptr);
            bool already = false;
            for (int k = 0; k < doneCount && !already; k++) already = !_wcsicmp(done[k], id);
            if (already) continue;
            if (doneCount < 16) wcscpy(done[doneCount++], id);
            removed = true;
            // A program has it open (Windows refuses): removing it anyway left a half-removed device until a restart,
            // and the program (a DLP agent) kept reading it as "camera N" while a new device was that camera. It stays
            // as it is, hidden from the list of cameras; it goes with a later change of the number once closed.
            PNP_VETO_TYPE veto = PNP_VetoTypeUnknown;
            wchar_t vetoName[MAX_PATH] = L"";
            CONFIGRET cr = CM_Query_And_Remove_SubTreeW(d.DevInst, &veto, vetoName, MAX_PATH, CM_REMOVE_NO_RESTART | CM_REMOVE_UI_NOT_OK);
            if (cr != CR_SUCCESS)
            {
                bool off = ShowCamera(cam, false);
                AppLog(L"camera devices: camera %d in use (0x%lX, veto %d %ls): %ls", cam + 1, (unsigned long)cr, (int)veto, vetoName,
                       off ? L"hidden until a program closes it" : L"left as it is (could not be hidden)");
                if (hidden) (*hidden)++;
                continue;
            }
            if (!SetupDiCallClassInstaller(DIF_REMOVE, rs, &d))
            {
                LogError(log, ctx, TR(L"не удалось удалить устройство"), GetLastError());
                ok = false;
            }
            SP_DEVINSTALL_PARAMS_W dip = { sizeof(dip) };
            if (SetupDiGetDeviceInstallParamsW(rs, &d, &dip) && (dip.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART))) *rebootNeeded = true;
            AppLog(L"camera devices: camera %d removed", cam + 1);
            if (cam < 10) used[cam] = false;            // cam >= count >= 0 here
        }
        SetupDiDestroyDeviceInfoList(rs);
        if (!removed || !ok) break;
    }
    // Missing: a new device per camera (with the driver from the driver store when asked).
    for (int cam = 0; cam < count; cam++)
    {
        if (used[cam])
        {
            // shown again if it was hidden (removed while in use, the program may still have it: the same device)
            // (not one switched off: it stays hidden)
            if (cam > 0 && SetupCameraEnabled(cam) && ShowCamera(cam, true)) AppLog(L"camera devices: camera %d shown", cam + 1);
            continue;
        }
        HDEVINFO nset = SetupDiCreateDeviceInfoList(&kClassImage, nullptr);
        if (nset == INVALID_HANDLE_VALUE)
        {
            ok = false;
            continue;
        }
        SP_DEVINFO_DATA d = { sizeof(d) };
        wchar_t hwid[64] = {};
        wcscpy(hwid, kHardwareId);      // REG_MULTI_SZ: string + extra terminator (array is zeroed)
        DWORD hwidBytes = (DWORD)((wcslen(hwid) + 2) * sizeof(wchar_t));
        bool created = SetupDiCreateDeviceInfoW(nset, L"Image", &kClassImage, nullptr, nullptr, DICD_GENERATE_ID, &d) &&
                       SetupDiSetDeviceRegistryPropertyW(nset, &d, SPDRP_HARDWAREID, (BYTE*)hwid, hwidBytes) &&
                       SetupDiCallClassInstaller(DIF_REGISTERDEVICE, nset, &d) && SetDeviceCameraIndex(nset, &d, cam);
        DWORD err = GetLastError();
        if (!created)
        {
            LogError(log, ctx, TR(L"не удалось создать устройство"), err);
            ok = false;
        }
        else
        {
            AppLog(L"camera devices: device for camera %d created", cam + 1);
            if (installDriver)
            {
                BOOL reboot = FALSE;
                // DiInstallDevice (newdev.dll, Windows 7+; not in the MinGW import library): the best driver of
                // the driver store on this one device, the other cameras are not touched.
                typedef BOOL(WINAPI * DiInstallDeviceFn)(HWND, HDEVINFO, PSP_DEVINFO_DATA, PSP_DRVINFO_DATA_W, DWORD, PBOOL);
                DiInstallDeviceFn diInstallDevice =
                    (DiInstallDeviceFn)(void*)GetProcAddress(LoadLibraryW(L"newdev.dll"), "DiInstallDevice");
                if (!diInstallDevice || !diInstallDevice(nullptr, nset, &d, nullptr, 0, &reboot))
                {
                    LogError(log, ctx, TR(L"не удалось установить драйвер"), GetLastError());
                    ok = false;
                }
                if (reboot) *rebootNeeded = true;
            }
        }
        SetupDiDestroyDeviceInfoList(nset);
    }
    ApplyCameraStates(count);
    return ok;
}

// ---------------------------------------------------------------------------
// A camera switched off / on: Camera<N>Disabled in the driver settings (the panel reads it without administrator
// rights) and its device disabled / enabled as in Device Manager (kept across a restart; programs no longer see it).

static void CameraParamName(int cam, const wchar_t* what, wchar_t* out)
{
    _snwprintf(out, 32, L"Camera%d%ls", cam + 1, what);
    out[31] = 0;
}

bool SetupCameraEnabled(int cam)
{
    wchar_t name[32];
    CameraParamName(cam, L"Disabled", name);
    return S2cGetParam(name, 0) == 0;
}

static void SetCameraDisabledParam(int cam, bool disabled)
{
    wchar_t name[32];
    CameraParamName(cam, L"Disabled", name);
    if (disabled) S2cSetParam(name, 1);
    else RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, S2C_PARAMS_KEY, name);
}

// The interface of camera `cam` (reference string "camera<N>") in a device interface class, opened for its properties;
// INVALID_HANDLE_VALUE if there is none.
static HANDLE OpenCameraInterface(const GUID& category, int cam)
{
    HDEVINFO set = SetupDiGetClassDevsW(&category, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
    wchar_t want[16];
    _snwprintf(want, 16, L"camera%d", cam + 1);
    HANDLE h = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA di = { sizeof(di) };
    for (DWORD i = 0; h == INVALID_HANDLE_VALUE && SetupDiEnumDeviceInterfaces(set, nullptr, &category, i, &di); i++)
    {
        BYTE buf[2048];
        auto* d = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf;
        d->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA info = { sizeof(info) };
        if (!SetupDiGetDeviceInterfaceDetailW(set, &di, d, sizeof(buf), nullptr, &info)) continue;
        wchar_t service[64] = L"";
        if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_SERVICE, nullptr, (BYTE*)service, sizeof(service) - 2, nullptr) ||
            _wcsicmp(service, L"Show2Cam") != 0)
            continue;
        const wchar_t* ref = wcsrchr(d->DevicePath, L'\\');
        if (!ref || _wcsicmp(ref + 1, want) != 0) continue;
        h = CreateFileW(d->DevicePath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OVERLAPPED, nullptr);
    }
    SetupDiDestroyDeviceInfoList(set);
    return h;
}

static bool CameraProperty(HANDLE h, ULONG id, ULONG flags, void* data, DWORD size)
{
    static const GUID kPropSet = { 0x6f1c2a9e, 0x3b57, 0x4e0c, { 0x9d, 0x1a, 0x5c, 0x2e, 0x7b, 0x3f, 0x8a, 0x41 } };
    KSPROPERTY p = {};
    p.Set = kPropSet;
    p.Id = id;
    p.Flags = flags;
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    DWORD got = 0;
    BOOL done = ov.hEvent && DeviceIoControl(h, IOCTL_KS_PROPERTY, &p, sizeof(p), data, size, &got, &ov);
    if (!done && ov.hEvent && GetLastError() == ERROR_IO_PENDING) done = GetOverlappedResult(h, &ov, &got, TRUE);
    DWORD err = done ? 0 : GetLastError();
    if (ov.hEvent) CloseHandle(ov.hEvent);
    SetLastError(err);
    return done != FALSE;
}

static const GUID kControlCategory = S2C_CONTROL_CATEGORY_INIT;
static const GUID kVideoCategory = { 0x6994ad05, 0x93ef, 0x11d0, { 0xa3, 0xcc, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 } };

int SetupCameraSwitchedOff(int cam)
{
    HANDLE h = OpenCameraInterface(kControlCategory, cam);
    if (h == INVALID_HANDLE_VALUE) return -1;
    S2C_SHOW s = {};
    bool ok = CameraProperty(h, S2C_PROPERTY_ENABLED, KSPROPERTY_TYPE_GET, &s, sizeof(s));
    CloseHandle(h);
    return ok ? (s.Show ? 0 : 1) : -1;
}

int SetupSetCameraEnabled(int cam, bool enable)
{
    if (cam < 0 || cam >= 10) return 3;
    HANDLE h = OpenCameraInterface(kControlCategory, cam);
    if (h == INVALID_HANDLE_VALUE)
    {
        // no device for it (yet): the setting only (it starts so); administrator rights
        SetCameraDisabledParam(cam, !enable);
        AppLog(L"camera devices: camera %d %ls: no device, setting only", cam + 1, enable ? L"on" : L"off");
        return SetupCameraEnabled(cam) == enable ? 1 : 3;
    }
    S2C_SHOW s = { (ULONG)cam, enable ? 1u : 0u };
    bool ok = CameraProperty(h, S2C_PROPERTY_ENABLED, KSPROPERTY_TYPE_SET, &s, sizeof(s));
    DWORD err = ok ? 0 : GetLastError();
    CloseHandle(h);
    // the driver changes its interfaces a moment later (a work item): programs see it / no longer see it
    bool seen = !enable;
    for (int t = 0; ok && t < 50; t++)
    {
        HANDLE v = OpenCameraInterface(kVideoCategory, cam);
        seen = v != INVALID_HANDLE_VALUE;
        if (seen) CloseHandle(v);
        if (seen == enable) break;
        Sleep(100);
    }
    int rc = !ok ? 3 : (seen == enable ? 0 : 3);
    AppLog(L"camera devices: camera %d %ls -> %d (%lu)", cam + 1, enable ? L"on" : L"off", rc, err);
    return rc;
}

// Cameras above the count lose the setting (a camera added later starts on); the others read theirs when they start.
static void ApplyCameraStates(int count)
{
    for (int cam = count; cam < 10; cam++) SetCameraDisabledParam(cam, false);
}

// New camera i (0..n-1) takes the driver settings and the on / off state of old camera src[i]; the settings of cameras
// n.. are deleted (up to `count`). 0 done, 5 restart needed, 7 a camera could not take its new on / off state.
static int PermuteCameras(const int* src, int n, int count)
{
    static const wchar_t* kWhat[] = { L"Name", L"Width", L"Height", L"Fps", L"Disabled" };
    const int kinds = (int)(sizeof(kWhat) / sizeof(kWhat[0]));
    struct Value { bool has; DWORD type, size; BYTE data[160]; };
    static Value v[10][5];
    bool was[10];
    for (int cam = 0; cam < 10; cam++)
    {
        was[cam] = SetupCameraEnabled(cam);
        for (int k = 0; k < kinds; k++)
        {
            wchar_t name[32];
            CameraParamName(cam, kWhat[k], name);
            v[cam][k].size = sizeof(v[cam][k].data);
            v[cam][k].has = RegGetValueW(HKEY_LOCAL_MACHINE, S2C_PARAMS_KEY, name, RRF_RT_REG_DWORD | RRF_RT_REG_SZ, &v[cam][k].type,
                                         v[cam][k].data, &v[cam][k].size) == ERROR_SUCCESS;
        }
    }
    for (int cam = 0; cam < count && cam < 10; cam++)
        for (int k = 0; k < kinds; k++)
        {
            wchar_t name[32];
            CameraParamName(cam, kWhat[k], name);
            const Value* from = cam < n ? &v[src[cam]][k] : nullptr;
            if (from && from->has) RegSetKeyValueW(HKEY_LOCAL_MACHINE, S2C_PARAMS_KEY, name, from->type, from->data, from->size);
            else RegDeleteKeyValueW(HKEY_LOCAL_MACHINE, S2C_PARAMS_KEY, name);
        }
    int rc = 0;
    for (int cam = 0; cam < n; cam++)
    {
        bool want = was[src[cam]];
        if (want == was[cam]) continue;     // the device already is in that state
        int r = SetupSetCameraEnabled(cam, want);
        if (r == 5 && rc == 0) rc = 5;
        else if (r == 2 || r == 3) rc = 7;
    }
    return rc;
}

int SetupMoveCamera(int from, int to)
{
    int count = (int)S2cGetParam(L"CameraCount", 1);
    if (count < 1 || count > 10 || from < 0 || from >= count || to < 0 || to >= count) return 4;
    if (from == to) return 0;
    int src[10], n = 0;
    for (int cam = 0; cam < count; cam++)
        if (cam != from) src[n++] = cam;
    for (int k = n; k > to; k--) src[k] = src[k - 1];
    src[to] = from;
    int rc = PermuteCameras(src, count, count);
    AppLog(L"camera devices: camera %d moved to %d -> %d", from + 1, to + 1, rc);
    return rc;
}

int SetupDeleteCamera(int cam, bool* rebootNeeded, int* hidden)
{
    *rebootNeeded = false;
    if (hidden) *hidden = 0;
    int count = (int)S2cGetParam(L"CameraCount", 1);
    if (count < 1 || count > 10 || cam < 0 || cam >= count) return 4;
    // The cameras after it take its place: their settings move down one number; the last device goes.
    int src[10], n = 0;
    for (int i = 0; i < count; i++)
        if (i != cam) src[n++] = i;
    int rc = PermuteCameras(src, n, count);
    if (rc == 5) *rebootNeeded = true;
    AppLog(L"camera devices: camera %d deleted (of %d)", cam + 1, count);
    bool reboot = false;
    int hid = 0;
    bool ok = SetupSetCameraCount(count - 1, true, &reboot, nullptr, nullptr, &hid);
    if (reboot) *rebootNeeded = true;
    if (hidden) *hidden = hid;
    return !ok ? 4 : (*rebootNeeded ? 5 : (rc == 7 ? 7 : (hid ? 6 : 0)));
}

static wchar_t g_stagedInf[64];      // the package put into the store without a device (no cameras)

bool SetupInstallDriver(const wchar_t* infArg, bool* rebootNeeded, SetupLog log, void* ctx)
{
    *rebootNeeded = false;
    wchar_t inf[MAX_PATH];
    if (!GetFullPathNameW(infArg, MAX_PATH, inf, nullptr) || GetFileAttributesW(inf) == INVALID_FILE_ATTRIBUTES)
    {
        Log(log, ctx, TR(L"Ошибка: не найден файл %ls"), infArg);
        return false;
    }

    // One root-enumerated device per camera (the number of cameras in the driver settings, 1 by default).
    int count = (int)S2cGetParam(L"CameraCount", 1);
    if (count < 0 || count > 10) count = 1;
    bool unused = false;
    if (!SetupSetCameraCount(count, false, &unused, log, ctx)) return false;

    SetDriverLogFile();
    if (SetupDeviceCount() == 0)
    {
        // No cameras (the number of cameras is 0): the driver package goes into the driver store only; a camera added
        // later gets its driver from there.
        wchar_t staged[MAX_PATH] = L"";
        if (!SetupCopyOEMInfW(inf, nullptr, SPOST_PATH, 0, staged, MAX_PATH, nullptr, nullptr))
        {
            LogError(log, ctx, TR(L"не удалось установить драйвер"), GetLastError());
            return false;
        }
        const wchar_t* name = wcsrchr(staged, L'\\');
        wcsncpy(g_stagedInf, name ? name + 1 : staged, 63);
        AppLog(L"no cameras: driver package %ls staged", g_stagedInf);
        Log(log, ctx, TR(L"Драйвер установлен (камер: 0)."));
        return true;
    }
    Log(log, ctx, TR(L"Установка драйвера… Если Windows спросит про издателя, выберите «Все равно установить»."));
    BOOL reboot = FALSE;
    if (!UpdateDriverForPlugAndPlayDevicesW(nullptr, kHardwareId, inf, INSTALLFLAG_FORCE, &reboot))
    {
        DWORD err = GetLastError();
        LogError(log, ctx, TR(L"не удалось установить драйвер"), err);
        if (err == ERROR_CANCELLED)
            Log(log, ctx, TR(L"Установка отменена в окне предупреждения о драйвере."));
        return false;
    }
    *rebootNeeded = reboot != FALSE;
    Log(log, ctx, reboot ? TR(L"Драйвер установлен. Требуется перезагрузка.") : TR(L"Драйвер установлен."));
    return true;
}

// Driver packages in the store are %windir%\INF\oemNN.inf copies of Show2Cam.inf.
static int RemoveDriverPackages(SetupLog log, void* ctx, const wchar_t* keepInf = nullptr)
{
    wchar_t dir[MAX_PATH], pattern[MAX_PATH];
    GetWindowsDirectoryW(dir, MAX_PATH);
    _snwprintf(pattern, MAX_PATH, L"%ls\\INF\\oem*.inf", dir);
    pattern[MAX_PATH - 1] = 0;

    int removed = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do
    {
        wchar_t path[MAX_PATH], provider[64] = {}, service[64] = {};
        _snwprintf(path, MAX_PATH, L"%ls\\INF\\%ls", dir, fd.cFileName);
        path[MAX_PATH - 1] = 0;
        GetPrivateProfileStringW(L"Strings", L"ProviderName", L"", provider, 64, path);
        GetPrivateProfileStringW(L"Show2Cam_Device.NT.Services", L"AddService", L"", service, 64, path);
        // the service name identifies the package (the provider is the author)
        if (_wcsicmp(provider, L"" S2C_AUTHOR) != 0 || _wcsnicmp(service, L"Show2Cam", 8) != 0)
        {
            continue;
        }
        if (keepInf && _wcsicmp(fd.cFileName, keepInf) == 0) continue;     // the package the device uses now
        if (SetupUninstallOEMInfW(fd.cFileName, SUOI_FORCEDELETE, nullptr))
        {
            Log(log, ctx, TR(L"Пакет драйвера %ls удалён из хранилища драйверов."), fd.cFileName);
            removed++;
        }
        else
        {
            LogError(log, ctx, fd.cFileName, GetLastError());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return removed;
}

bool SetupInstalledWithoutCameras()
{
    DWORD count = 1, size = sizeof(count);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, S2C_PARAMS_KEY, L"CameraCount", RRF_RT_REG_DWORD, nullptr, &count, &size) != ERROR_SUCCESS ||
        count != 0)
        return false;
    wchar_t dir[MAX_PATH], pattern[MAX_PATH];
    GetWindowsDirectoryW(dir, MAX_PATH);
    _snwprintf(pattern, MAX_PATH, L"%ls\\INF\\oem*.inf", dir);
    pattern[MAX_PATH - 1] = 0;
    bool found = false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    do
    {
        wchar_t path[MAX_PATH], provider[64] = {}, service[64] = {};
        _snwprintf(path, MAX_PATH, L"%ls\\INF\\%ls", dir, fd.cFileName);
        path[MAX_PATH - 1] = 0;
        GetPrivateProfileStringW(L"Strings", L"ProviderName", L"", provider, 64, path);
        GetPrivateProfileStringW(L"Show2Cam_Device.NT.Services", L"AddService", L"", service, 64, path);
        found = !_wcsicmp(provider, L"" S2C_AUTHOR) && !_wcsnicmp(service, L"Show2Cam", 8);
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

#define S2C_PARAMS_PATH L"SYSTEM\\CurrentControlSet\\Services\\Show2Cam\\Parameters"

// Driver status values (written by the driver; never carried over to a new version).
static bool IsStatusValue(const wchar_t* name)
{
    return !_wcsicmp(name, L"StartStatus") || !_wcsicmp(name, L"CamerasCreated") || !_wcsicmp(name, L"DriverLog");
}

// The user's driver settings (camera count, sizes, frame rates: every DWORD but the status values; the cameras'
// names Camera<N>Name), kept across an update / reinstall: removing the driver package may delete the service key
// with them.
struct DriverSettings
{
    int     count;
    wchar_t name[64][32];
    DWORD   type[64];
    DWORD   value[64];
    wchar_t text[64][64];
};

static bool IsCameraName(const wchar_t* name)
{
    size_t n = wcslen(name);
    return _wcsnicmp(name, L"Camera", 6) == 0 && n > 10 && _wcsicmp(name + n - 4, L"Name") == 0;
}

static void SaveDriverSettings(DriverSettings* s)
{
    s->count = 0;
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, S2C_PARAMS_PATH, 0, KEY_READ, &key) != ERROR_SUCCESS) return;
    for (DWORD i = 0; s->count < 64; i++)
    {
        wchar_t name[32], text[64] = L"";
        DWORD nameLen = 32, type = 0, size = sizeof(text);
        LSTATUS rs = RegEnumValueW(key, i, name, &nameLen, nullptr, &type, (BYTE*)text, &size);
        if (rs == ERROR_NO_MORE_ITEMS) break;
        if (rs != ERROR_SUCCESS || IsStatusValue(name)) continue;
        if (type == REG_DWORD && size == sizeof(DWORD))
            s->value[s->count] = *(DWORD*)text;
        else if (type == REG_SZ && IsCameraName(name))
        {
            text[63] = 0;
            wcscpy(s->text[s->count], text);
        }
        else
            continue;
        wcscpy(s->name[s->count], name);
        s->type[s->count++] = type;
    }
    RegCloseKey(key);
}

// Written back right after the old version is removed, before the new one is installed: the INF does not
// overwrite existing values (NOCLOBBER), so the new driver starts with them.
static void RestoreDriverSettings(const DriverSettings* s, SetupLog log, void* ctx)
{
    if (!s->count) return;
    HKEY key;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, S2C_PARAMS_PATH, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    for (int i = 0; i < s->count; i++)
    {
        if (s->type[i] == REG_SZ)
        {
            RegSetValueExW(key, s->name[i], 0, REG_SZ, (const BYTE*)s->text[i], (DWORD)((wcslen(s->text[i]) + 1) * sizeof(wchar_t)));
            AppLog(L"kept setting %ls = \"%ls\"", s->name[i], s->text[i]);
        }
        else
        {
            RegSetValueExW(key, s->name[i], 0, REG_DWORD, (const BYTE*)&s->value[i], sizeof(DWORD));
            AppLog(L"kept setting %ls = %lu", s->name[i], s->value[i]);
        }
    }
    RegCloseKey(key);
    Log(log, ctx, TR(L"Настройки камер сохранены для новой версии."));
}

// Removes all driver settings: the next installation starts from the INF defaults (one camera, 1280x720, 30 fps).
static void ResetDriverSettings(SetupLog log, void* ctx)
{
    if (RegDeleteTreeW(HKEY_LOCAL_MACHINE, S2C_PARAMS_PATH) == ERROR_SUCCESS)
        Log(log, ctx, TR(L"Настройки драйвера сброшены (после установки — одна камера, 1280×720, 30 кадров/с)."));
}

bool SetupRemoveDriver(SetupLog log, void* ctx, bool keepSettings)
{
    DriverSettings kept = {};
    if (keepSettings) SaveDriverSettings(&kept);
    else ResetDriverSettings(log, ctx);
    int failed = 0;
    int n = ForEachDevice([&](HDEVINFO set, SP_DEVINFO_DATA* info) {
        if (!SetupDiCallClassInstaller(DIF_REMOVE, set, info))
        {
            LogError(log, ctx, TR(L"не удалось удалить устройство"), GetLastError());
            failed++;
        }
    });
    Log(log, ctx, TR(L"Удалено устройств: %d."), n - failed);
    int packages = RemoveDriverPackages(log, ctx);
    if (n == 0 && packages == 0)
    {
        Log(log, ctx, TR(L"Show2Cam не был установлен."));
    }
    if (keepSettings) RestoreDriverSettings(&kept, log, ctx);
    return failed == 0;
}

// ---------------------------------------------------------------------------
// Boot configuration

bool SetupTestSigningEnabled()
{
    return S2cTestSigningEnabled();
}

bool SetupSecureBootEnabled()
{
    return S2cSecureBootEnabled();
}

// The setting in the boot configuration store (what the next start will use). The store is a registry hive:
// the boot manager's default entry ({9dea862c-...}, element 23000003) names the Windows loader entry, whose element
// 16000049 (BcdLibraryBoolean_AllowPrereleaseSignatures) is "testsigning"; no such element means off.
int SetupTestSigningConfigured()
{
    wchar_t entry[64] = {};
    DWORD size = sizeof(entry) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"BCD00000000\\Objects\\{9dea862c-5cdd-4e70-acc1-f32b344d4795}\\Elements\\23000003",
                     L"Element", RRF_RT_REG_SZ, nullptr, entry, &size) != ERROR_SUCCESS || entry[0] != L'{')
        return -1;
    wchar_t path[200];
    _snwprintf(path, 200, L"BCD00000000\\Objects\\%ls", entry);
    path[199] = 0;
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &key) != ERROR_SUCCESS) return -1;
    BYTE value[8] = {};
    size = sizeof(value);
    LSTATUS rs = RegGetValueW(key, L"Elements\\16000049", L"Element", RRF_RT_REG_BINARY, nullptr, value, &size);
    RegCloseKey(key);
    if (rs == ERROR_FILE_NOT_FOUND) return 0;
    if (rs != ERROR_SUCCESS || size < 1) return -1;
    return value[0] ? 1 : 0;
}

bool SetupSetTestSigning(bool on, SetupLog log, void* ctx)
{
    wchar_t sys[MAX_PATH], cmd[MAX_PATH + 64];
    GetSystemDirectoryW(sys, MAX_PATH);
    _snwprintf(cmd, MAX_PATH + 64, L"\"%ls\\bcdedit.exe\" /set {current} testsigning %ls", sys, on ? L"on" : L"off");
    cmd[MAX_PATH + 63] = 0;

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        LogError(log, ctx, TR(L"не удалось запустить bcdedit"), GetLastError());
        return false;
    }
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    AppLog(L"bcdedit testsigning %ls: exit code %lu", on ? L"on" : L"off", (unsigned long)code);
    if (code != 0)
    {
        Log(log, ctx, on ? TR(L"Ошибка: bcdedit завершился с кодом %lu. Если включён Secure Boot, сначала выключите его.")
                         : TR(L"Ошибка: bcdedit завершился с кодом %lu."),
            (unsigned long)code);
        return false;
    }
    Log(log, ctx, on ? TR(L"Тестовый режим подписи включён. Перезагрузите компьютер и снова запустите установку.")
                     : TR(L"Тестовый режим подписи будет выключен после перезагрузки компьютера."));
    return true;
}

bool SetupEnableTestSigning(SetupLog log, void* ctx)
{
    return SetupSetTestSigning(true, log, ctx);
}

// DEVPKEY_Device_DriverInfPath: the INF (oemNN.inf) the device uses.
static const DEVPROPKEY kDevDriverInfPath = { { 0xa8b865dd, 0x2e3d, 0x4094, { 0xad, 0x97, 0xe5, 0x93, 0xa7, 0x0c, 0x75, 0xd6 } }, 5 };

int SetupRemoveOldDriverPackages(SetupLog log, void* ctx)
{
    wchar_t current[64] = {};
    ForEachDevice([&](HDEVINFO set, SP_DEVINFO_DATA* info) {
        DEVPROPTYPE type = 0;
        wchar_t inf[64] = {};
        if (!current[0] && SetupDiGetDevicePropertyW(set, info, &kDevDriverInfPath, &type, (BYTE*)inf, sizeof(inf) - 2, nullptr, 0) &&
            type == DEVPROP_TYPE_STRING)
            wcscpy(current, inf);
    });
    if (!current[0]) wcscpy(current, g_stagedInf);  // no cameras: the package just put into the store
    if (!current[0]) return 0;      // unknown: keep everything
    AppLog(L"driver package in use: %ls", current);
    return RemoveDriverPackages(log, ctx, current);
}

void SetupResetDriverStatus()
{
    HKEY key;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, S2C_PARAMS_PATH, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) return;
    const wchar_t* names[] = { L"StartStatus", L"CamerasCreated" };
    for (const wchar_t* n : names) RegDeleteValueW(key, n);
    RegCloseKey(key);
}

bool SetupDevicePresent()
{
    ULONG status = 0, problem = 0;
    wchar_t id[200];
    return SetupGetDeviceState(&status, &problem, id, 200) && problem != CM_PROB_PHANTOM;
}

