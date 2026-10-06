// The Show2Cam cameras (see camdev.h).
#include "camdev.h"
#include <winioctl.h>
#include <setupapi.h>
#include <ks.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

static const GUID kCategoryVideo = { 0x6994ad05, 0x93ef, 0x11d0, { 0xa3, 0xcc, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 } };
static const GUID kPropSetShow2Cam = { 0x6f1c2a9e, 0x3b57, 0x4e0c, { 0x9d, 0x1a, 0x5c, 0x2e, 0x7b, 0x3f, 0x8a, 0x41 } };   // PROPSETID_Show2Cam

#ifdef S2C_UI_TEST
// Layout / source checks under Wine (no driver): three pretend cameras that take everything. Never in a release build.
static S2C_STATUS g_fake[3];
static wchar_t g_fakeName[3][S2C_NAME_CHARS];
static int FakeIndex(HANDLE h) { ULONG_PTR v = (ULONG_PTR)h; return v >= 0x7F000000 && v < 0x7F000003 ? (int)(v - 0x7F000000) : -1; }
#endif

int CamList(CamInfo* out, int max)
{
#ifdef S2C_UI_TEST
    for (int i = 0; i < 3 && i < max; i++)
    {
        out[i].index = i;
        _snwprintf(out[i].path, 512, L"test:%d", i);
        if (!g_fakeName[i][0]) _snwprintf(g_fakeName[i], S2C_NAME_CHARS, L"Show2Cam Camera %d", i + 1);
        wcscpy(out[i].name, g_fakeName[i]);
        if (!g_fake[i].Width) { g_fake[i].Index = i; g_fake[i].Width = 1280; g_fake[i].Height = 720; g_fake[i].Fps = 30; }
    }
    g_fake[1].Streaming = 1;
    DWORD explorer = 0;
    GetWindowThreadProcessId(FindWindowW(L"Shell_TrayWnd", nullptr), &explorer);
    g_fake[1].UserPids[0] = explorer;
    return max < 3 ? max : 3;
#endif
    HDEVINFO set = SetupDiGetClassDevsW(&kCategoryVideo, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    SP_DEVICE_INTERFACE_DATA di = { sizeof(di) };
    for (DWORD i = 0; n < max && SetupDiEnumDeviceInterfaces(set, nullptr, &kCategoryVideo, i, &di); i++)
    {
        BYTE buf[2048];
        auto* d = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf;
        d->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA info = { sizeof(info) };
        if (!SetupDiGetDeviceInterfaceDetailW(set, &di, d, sizeof(buf), nullptr, &info)) continue;
        // Ours: the device is driven by the Show2Cam service (Windows names the device instance after its class,
        // ROOT\CAMERA\NNNN, so the path tells nothing).
        wchar_t service[64] = L"";
        if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_SERVICE, nullptr, (BYTE*)service, sizeof(service) - 2, nullptr) ||
            _wcsicmp(service, L"Show2Cam") != 0)
            continue;
        const wchar_t* ref = wcsrchr(d->DevicePath, L'\\');
        if (!ref || _wcsnicmp(ref + 1, L"camera", 6) != 0) continue;
        int number = _wtoi(ref + 7);
        if (number < 1 || number > 99) continue;
        CamInfo& c = out[n];
        c.index = number - 1;
        wcsncpy(c.path, d->DevicePath, 511);
        c.path[511] = 0;
        _snwprintf(c.name, S2C_NAME_CHARS, L"Show2Cam Camera %d", number);
        c.name[S2C_NAME_CHARS - 1] = 0;
        HKEY k = SetupDiOpenDeviceInterfaceRegKey(set, &di, 0, KEY_READ);
        if (k != INVALID_HANDLE_VALUE)
        {
            wchar_t name[S2C_NAME_CHARS];
            DWORD sz = sizeof(name);
            if (RegGetValueW(k, nullptr, L"FriendlyName", RRF_RT_REG_SZ, nullptr, name, &sz) == ERROR_SUCCESS && name[0])
                wcscpy(c.name, name);
            RegCloseKey(k);
        }
        n++;
    }
    SetupDiDestroyDeviceInfoList(set);
    qsort(out, n, sizeof(CamInfo), [](const void* a, const void* b) {
        return ((const CamInfo*)a)->index - ((const CamInfo*)b)->index;
    });
    return n;
}

HANDLE CamOpen(const wchar_t* path)
{
#ifdef S2C_UI_TEST
    if (!wcsncmp(path, L"test:", 5)) return (HANDLE)(ULONG_PTR)(0x7F000000 + _wtoi(path + 5));
#endif
    return CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                       FILE_FLAG_OVERLAPPED, nullptr);
}

bool CamProperty(HANDLE cam, ULONG id, ULONG flags, void* data, DWORD size, DWORD* returned)
{
#ifdef S2C_UI_TEST
    int fi = FakeIndex(cam);
    if (fi >= 0)
    {
        if (id == S2C_PROPERTY_STATUS) { g_fake[fi].PicturesReceived++; memcpy(data, &g_fake[fi], sizeof(S2C_STATUS)); if (returned) *returned = sizeof(S2C_STATUS); }
        if (id == S2C_PROPERTY_FORMAT) { auto* f = (S2C_FORMAT*)data; g_fake[fi].Width = f->Width; g_fake[fi].Height = f->Height; g_fake[fi].Fps = f->Fps; }
        if (id == S2C_PROPERTY_LOG) { auto* l = (S2C_LOG*)data; l->Generation = 1; l->Length = (ULONG)sprintf(l->Text, "test driver log line\n"); if (returned) *returned = sizeof(S2C_LOG); }
        if (id == S2C_PROPERTY_NAME) { auto* n = (S2C_NAME*)data; if (n->Name[0]) wcscpy(g_fakeName[fi], n->Name); else _snwprintf(g_fakeName[fi], S2C_NAME_CHARS, L"Show2Cam Camera %d", fi + 1); }
        return true;
    }
#endif
    KSPROPERTY p = {};
    p.Set = kPropSetShow2Cam;
    p.Id = id;
    p.Flags = flags;
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;
    DWORD got = 0;
    BOOL ok = DeviceIoControl(cam, IOCTL_KS_PROPERTY, &p, sizeof(p), data, size, &got, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(cam, &ov, &got, TRUE);
    DWORD err = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(ov.hEvent);
    if (returned) *returned = got;
    SetLastError(err);
    return ok != FALSE;
}

bool CamGetLog(HANDLE cam, S2C_LOG* log)
{
    DWORD got = 0;
    log->Length = 0;
    return CamProperty(cam, S2C_PROPERTY_LOG, KSPROPERTY_TYPE_GET, log, sizeof(*log), &got) && got >= sizeof(*log) - S2C_LOG_MAX &&
           log->Length <= S2C_LOG_MAX;
}

bool CamGetStatus(HANDLE cam, S2C_STATUS* status)
{
    DWORD got = 0;
    ZeroMemory(status, sizeof(*status));
    return CamProperty(cam, S2C_PROPERTY_STATUS, KSPROPERTY_TYPE_GET, status, sizeof(*status), &got) && got >= sizeof(*status);
}

DWORD CamSetFormat(HANDLE cam, ULONG width, ULONG height, ULONG fps)
{
    S2C_FORMAT f = { width, height, fps, 0 };
    if (CamProperty(cam, S2C_PROPERTY_FORMAT, KSPROPERTY_TYPE_SET, &f, sizeof(f), nullptr)) return ERROR_SUCCESS;
    DWORD err = GetLastError();
    return err == ERROR_DEVICE_IN_USE || err == ERROR_BUSY ? ERROR_BUSY : err;     // STATUS_DEVICE_BUSY
}

bool CamSetName(HANDLE cam, const wchar_t* name)
{
    S2C_NAME n = {};
    wcsncpy(n.Name, name ? name : L"", S2C_NAME_CHARS - 1);
    return CamProperty(cam, S2C_PROPERTY_NAME, KSPROPERTY_TYPE_SET, &n, sizeof(n), nullptr);
}

CamFrame::~CamFrame()
{
    if (buffer) VirtualFree(buffer, 0, MEM_RELEASE);
}

bool CamFrame::Resize(ULONG w, ULONG h)
{
    SIZE_T need = sizeof(S2C_FRAME_HEADER) + (SIZE_T)w * h * 4;
    if (need > capacity)
    {
        if (buffer) VirtualFree(buffer, 0, MEM_RELEASE);
        buffer = (BYTE*)VirtualAlloc(nullptr, need, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        capacity = buffer ? need : 0;
        if (!buffer) return false;
    }
    width = w;
    height = h;
    return true;
}

bool CamSendFrame(HANDLE cam, CamFrame& frame)
{
    if (!frame.buffer) return false;
    auto* h = (S2C_FRAME_HEADER*)frame.buffer;
    h->Magic = S2C_FRAME_MAGIC;
    h->Width = frame.width;
    h->Height = frame.height;
    h->Flags = 0;
    return CamProperty(cam, S2C_PROPERTY_FRAME, KSPROPERTY_TYPE_SET, frame.buffer,
                       (DWORD)(sizeof(S2C_FRAME_HEADER) + (SIZE_T)frame.width * frame.height * 4), nullptr);
}

bool CamSendTestPattern(HANDLE cam)
{
    S2C_FRAME_HEADER h = { S2C_FRAME_MAGIC, 0, 0, 0 };
    return CamProperty(cam, S2C_PROPERTY_FRAME, KSPROPERTY_TYPE_SET, &h, sizeof(h), nullptr);
}

