// Camera settings in the registry and the settings file (see camcfg.h).
#include "camcfg.h"
#include "../driver/version.h"
#include <wchar.h>

static void ConfigKey(int index, wchar_t* key)
{
    _snwprintf(key, 64, L"%ls\\Camera%d", S2C_USER_KEY, index + 1);
    key[63] = 0;
}

void CamConfigDefault(int index, CamConfig* c)
{
    *c = CamConfig();
    _snwprintf(c->text, 256, L"Camera %d", index + 1);
}

void CamConfigLoad(int index, CamConfig* c)
{
    CamConfigDefault(index, c);
    wchar_t key[64];
    ConfigKey(index, key);
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    auto num = [&](const wchar_t* name, DWORD def) {
        DWORD v = def, size = sizeof(v);
        RegGetValueW(k, nullptr, name, RRF_RT_REG_DWORD, nullptr, &v, &size);
        return v;
    };
    auto str = [&](const wchar_t* name, wchar_t* out, DWORD chars) {
        DWORD size = MAX_PATH * 2 * sizeof(wchar_t);
        wchar_t tmp[MAX_PATH * 2];
        if (RegGetValueW(k, nullptr, name, RRF_RT_REG_SZ, nullptr, tmp, &size) == ERROR_SUCCESS)
        {
            wcsncpy(out, tmp, chars - 1);
            out[chars - 1] = 0;
        }
    };
    c->kind = (int)num(L"Source", SourceText);
    if (c->kind < 0 || c->kind >= SourceKindCount) c->kind = SourceText;
    str(L"Text", c->text, 256);
    str(L"ImageFolder", c->imageFolder, MAX_PATH);
    str(L"VideoFolder", c->videoFolder, MAX_PATH);
    str(L"Url", c->url, 512);
    c->audioMode = (int)num(L"AudioMode", AudioSpeak2Mic);
    if (c->audioMode < AudioSpeak2Mic || c->audioMode > AudioDevice) c->audioMode = AudioSpeak2Mic;
    str(L"AudioDevice", c->audioDevice, 256);
    c->width = num(L"Width", 0);
    c->height = num(L"Height", 0);
    c->fps = num(L"Fps", 0);
    c->paused = num(L"Paused", 0) != 0;
    RegCloseKey(k);
}

void CamConfigSave(int index, const CamConfig& c)
{
    wchar_t key[64];
    ConfigKey(index, key);
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr) != ERROR_SUCCESS) return;
    auto num = [&](const wchar_t* name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v)); };
    auto str = [&](const wchar_t* name, const wchar_t* v) {
        RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v, (DWORD)((wcslen(v) + 1) * sizeof(wchar_t)));
    };
    num(L"Source", (DWORD)c.kind);
    str(L"Text", c.text);
    str(L"ImageFolder", c.imageFolder);
    str(L"VideoFolder", c.videoFolder);
    str(L"Url", c.url);
    num(L"AudioMode", (DWORD)c.audioMode);
    str(L"AudioDevice", c.audioDevice);
    num(L"Width", c.width);
    num(L"Height", c.height);
    num(L"Fps", c.fps);
    num(L"Paused", c.paused ? 1 : 0);
    RegCloseKey(k);
}

bool CamPanelRunning()
{
    return FindWindowW(S2C_PANEL_CLASS, nullptr) != nullptr;
}

void CamConfigNotifyPanel(int index)
{
    UINT msg = RegisterWindowMessageW(S2C_SETTINGS_CHANGED_MSG);
    for (HWND w = FindWindowExW(nullptr, nullptr, S2C_PANEL_CLASS, nullptr); w; w = FindWindowExW(nullptr, w, S2C_PANEL_CLASS, nullptr))
        PostMessageW(w, msg, (WPARAM)(INT_PTR)index, 0);
}

// ---------------------------------------------------------------------------
// Settings file

bool CamIniCreate(const wchar_t* path)
{
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    const WORD bom = 0xFEFF;
    DWORD written = 0;
    bool ok = WriteFile(f, &bom, sizeof(bom), &written, nullptr) && written == sizeof(bom);
    CloseHandle(f);
    return ok;
}

static void IniNum(const wchar_t* path, const wchar_t* sec, const wchar_t* key, unsigned long value)
{
    wchar_t v[32];
    _snwprintf(v, 32, L"%lu", value);
    WritePrivateProfileStringW(sec, key, v, path);
}

void CamIniWriteHeader(const wchar_t* path, int cameraCount)
{
    WritePrivateProfileStringW(L"Show2Cam", L"Version", L"" S2C_VER_STR, path);
    IniNum(path, L"Show2Cam", L"CameraCount", (unsigned long)cameraCount);
}

void CamIniWriteCamera(const wchar_t* path, int index, const wchar_t* name, const CamConfig& c)
{
    wchar_t sec[16];
    _snwprintf(sec, 16, L"Camera%d", index + 1);
    WritePrivateProfileStringW(sec, L"Name", name, path);
    IniNum(path, sec, L"Source", (unsigned long)c.kind);
    WritePrivateProfileStringW(sec, L"Text", c.text, path);
    WritePrivateProfileStringW(sec, L"ImageFolder", c.imageFolder, path);
    WritePrivateProfileStringW(sec, L"VideoFolder", c.videoFolder, path);
    WritePrivateProfileStringW(sec, L"Url", c.url, path);
    IniNum(path, sec, L"AudioMode", (unsigned long)c.audioMode);
    WritePrivateProfileStringW(sec, L"AudioDevice", c.audioDevice, path);
    IniNum(path, sec, L"Width", c.width);
    IniNum(path, sec, L"Height", c.height);
    IniNum(path, sec, L"Fps", c.fps);
    IniNum(path, sec, L"Paused", c.paused ? 1 : 0);
}

bool CamIniFlush(const wchar_t* path)
{
    return WritePrivateProfileStringW(nullptr, nullptr, nullptr, path) != FALSE ||
           GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES;
}

static const UINT kNone = 0xFFFFFFFF;

static UINT IniGetNum(const wchar_t* path, const wchar_t* sec, const wchar_t* key)
{
    return (UINT)GetPrivateProfileIntW(sec, key, (INT)kNone, path);
}

static bool IniGetStr(const wchar_t* path, const wchar_t* sec, const wchar_t* key, wchar_t* out, DWORD len)
{
    wchar_t buf[MAX_PATH * 2] = L"\x01";
    GetPrivateProfileStringW(sec, key, L"\x01", buf, MAX_PATH * 2, path);
    if (buf[0] == 1) return false;           // not in the file
    wcsncpy(out, buf, len - 1);
    out[len - 1] = 0;
    return true;
}

bool CamIniCheck(const wchar_t* path, int* count)
{
    *count = 0;
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) return false;
    UINT n = IniGetNum(path, L"Show2Cam", L"CameraCount");
    bool any = n != kNone;
    if (n != kNone && n >= 1 && n <= S2C_MAX_CAMERAS_UI) *count = (int)n;
    for (int i = 1; i <= S2C_MAX_CAMERAS_UI && !any; i++)
    {
        wchar_t sec[16];
        _snwprintf(sec, 16, L"Camera%d", i);
        any = IniGetNum(path, sec, L"Source") != kNone;
    }
    return any;
}

bool CamIniReadCamera(const wchar_t* path, int index, CamConfig* c, wchar_t* name, bool* hasName)
{
    wchar_t sec[16];
    _snwprintf(sec, 16, L"Camera%d", index + 1);
    UINT k = IniGetNum(path, sec, L"Source");
    *hasName = false;
    if (k == kNone) return false;
    if (k < SourceKindCount) c->kind = (int)k;
    IniGetStr(path, sec, L"Text", c->text, 256);
    IniGetStr(path, sec, L"ImageFolder", c->imageFolder, MAX_PATH);
    IniGetStr(path, sec, L"VideoFolder", c->videoFolder, MAX_PATH);
    IniGetStr(path, sec, L"Url", c->url, 512);
    k = IniGetNum(path, sec, L"AudioMode");
    if (k <= AudioDevice) c->audioMode = (int)k;
    IniGetStr(path, sec, L"AudioDevice", c->audioDevice, 256);
    UINT w = IniGetNum(path, sec, L"Width"), h = IniGetNum(path, sec, L"Height"), fps = IniGetNum(path, sec, L"Fps");
    if (w == 0 || h == 0) c->width = c->height = 0;
    else if (w != kNone && h != kNone && w >= S2C_MIN_WIDTH && w <= S2C_MAX_WIDTH && h >= S2C_MIN_HEIGHT && h <= S2C_MAX_HEIGHT)
    {
        c->width = w & ~1u;
        c->height = h & ~1u;
    }
    if (fps != kNone && fps <= S2C_MAX_FPS) c->fps = fps;
    k = IniGetNum(path, sec, L"Paused");
    if (k <= 1) c->paused = k == 1;
    name[0] = 0;
    *hasName = IniGetStr(path, sec, L"Name", name, S2C_NAME_CHARS);
    return true;
}
