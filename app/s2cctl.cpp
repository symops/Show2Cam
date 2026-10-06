// s2cctl - Show2Cam settings from the command line (what the control panel sets).
//
//   s2cctl status                                   cameras, their sources and who uses them
//   s2cctl count N                                  number of cameras 1..10 (admin: devices added / removed)
//   s2cctl name CAM "NAME"                          the camera's name in Windows ("" or "default" = default name)
//   s2cctl source CAM text ["TEXT"]                 what the camera shows (CAM: 1..10 or "all")
//   s2cctl source CAM images|video [FOLDER]         ("default" = the folder next to the panel)
//   s2cctl source CAM stream URL                    MJPEG over HTTP/HTTPS
//   s2cctl source CAM generator
//   s2cctl audio CAM speak2mic|off|DEVICE           the videos' sound (DEVICE: part of a playback device's name, or its id)
//   s2cctl format CAM source|WIDTHxHEIGHT [FPS|source]
//   s2cctl pause CAM on|off
//   s2cctl reset CAM                                defaults (text "Camera N", as the source, default name)
//   s2cctl export FILE.ini   /   s2cctl import FILE.ini    (same file as the panel's; a camera count: admin)
//   s2cctl picture CAM FILE|pattern                 a still picture straight to the driver (without the panel)
//
// The sources run in the control panel: a running panel takes changed settings at once (it is told), otherwise
// they apply when it starts. Exit codes: 0 ok, 1 failed, 2 bad arguments, 3 administrator rights needed, 4 cannot
// work (Secure Boot on, test signing mode off or the driver not installed). Output and log in English
// (%ProgramData%\Show2Cam\logs\ctl.log).
#include "camcfg.h"
#include "camdev.h"
#include "applog.h"
#include "audioout.h"
#include "devctl.h"
#include "setupcore.h"
#include "../driver/version.h"
#include <objbase.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <wctype.h>
#include <io.h>
#include <fcntl.h>

enum { ExitOk = 0, ExitFailed = 1, ExitUsage = 2, ExitAdmin = 3, ExitNotReady = 4 };

static void Out(const wchar_t* fmt, ...)
{
    wchar_t buf[2048];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf(buf, 2048, fmt, args);
    va_end(args);
    buf[2047] = 0;
    wprintf(L"%ls\n", buf);
    AppLog(L"%ls", buf);
}

static bool IsElevated()
{
    HANDLE token = nullptr;
    TOKEN_ELEVATION el = {};
    DWORD size = 0;
    bool elevated = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        if (GetTokenInformation(token, TokenElevation, &el, sizeof(el), &size)) elevated = el.TokenIsElevated != 0;
        CloseHandle(token);
    }
    return elevated;
}

static bool ParseUlong(const wchar_t* s, ULONG* v)
{
    if (!s || !*s) return false;
    wchar_t* end = nullptr;
    unsigned long n = wcstoul(s, &end, 10);
    if (*end || n > 0xFFFFFFF) return false;
    *v = (ULONG)n;
    return true;
}

// The number of cameras: as set for the driver, or more if more are there.
static int CameraCount()
{
    int count = (int)S2cGetParam(L"CameraCount", 1);
    if (count < 1 || count > S2C_MAX_CAMERAS_UI) count = 1;
    CamInfo cams[S2C_MAX_CAMERAS_UI];
    int n = CamList(cams, S2C_MAX_CAMERAS_UI);
    for (int i = 0; i < n; i++)
        if (cams[i].index + 1 > count) count = cams[i].index + 1;
    return count;
}

// CAM: "1".."10" or "all". Fills the 0-based indexes; false: not a camera.
static bool ParseCameras(const wchar_t* s, int* list, int* n, bool allowAll)
{
    *n = 0;
    if (allowAll && !_wcsicmp(s, L"all"))
    {
        int count = CameraCount();
        for (int i = 0; i < count; i++) list[(*n)++] = i;
        return true;
    }
    ULONG v;
    if (!ParseUlong(s, &v) || v < 1 || v > S2C_MAX_CAMERAS_UI) return false;
    list[(*n)++] = (int)v - 1;
    return true;
}

// The camera with that number as Windows offers it now.
static bool FindCamera(int index, CamInfo* out)
{
    CamInfo cams[S2C_MAX_CAMERAS_UI];
    int n = CamList(cams, S2C_MAX_CAMERAS_UI);
    for (int i = 0; i < n; i++)
        if (cams[i].index == index)
        {
            *out = cams[i];
            return true;
        }
    return false;
}

static const wchar_t* KindName(int kind)
{
    switch (kind)
    {
    case SourceText: return L"text";
    case SourceImages: return L"images";
    case SourceVideo: return L"video";
    case SourceStream: return L"stream";
    case SourceGenerator: return L"generator";
    default: return L"?";
    }
}

static void ProcessName(DWORD pid, const wchar_t* driverName, wchar_t* out, size_t len)
{
    out[0] = 0;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p)
    {
        wchar_t path[MAX_PATH];
        DWORD size = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &size))
        {
            const wchar_t* s = wcsrchr(path, L'\\');
            wcsncpy(out, s ? s + 1 : path, len - 1);
            out[len - 1] = 0;
        }
        CloseHandle(p);
    }
    if (!out[0] && driverName[0])
    {
        wcsncpy(out, driverName, len - 1);
        out[len - 1] = 0;
    }
    if (!out[0]) wcsncpy(out, L"?", len - 1);
}

static void ConfigText(int index, const CamConfig& c, wchar_t* source, wchar_t* sound, wchar_t* format)
{
    wchar_t folder[MAX_PATH];
    switch (c.kind)
    {
    case SourceText: _snwprintf(source, 400, L"text \"%ls\"", c.text); break;
    case SourceImages:
    case SourceVideo:
        if ((c.kind == SourceImages ? c.imageFolder : c.videoFolder)[0]) wcscpy(folder, c.kind == SourceImages ? c.imageFolder : c.videoFolder);
        else wcscpy(folder, L"(default folder)");
        _snwprintf(source, 400, L"%ls %ls", KindName(c.kind), folder);
        break;
    case SourceStream: _snwprintf(source, 400, L"stream %ls", c.url); break;
    default: _snwprintf(source, 400, L"%ls", KindName(c.kind)); break;
    }
    if (c.paused) wcsncat(source, L" (paused)", 399 - wcslen(source));
    source[399] = 0;
    if (c.audioMode == AudioOff) wcscpy(sound, L"off");
    else if (c.audioMode == AudioDevice)
    {
        RenderDevice d;
        if (FindRenderDevice(c.audioDevice, &d)) _snwprintf(sound, 300, L"%ls", d.name);
        else _snwprintf(sound, 300, L"device %ls (not present)", c.audioDevice);
    }
    else wcscpy(sound, L"Speak2Mic Speaker");
    sound[299] = 0;
    wchar_t size[40], rate[40];
    if (c.width && c.height) _snwprintf(size, 40, L"%lux%lu", c.width, c.height);
    else wcscpy(size, L"as the source");
    if (c.fps) _snwprintf(rate, 40, L"%lu fps", c.fps);
    else wcscpy(rate, L"fps as the source");
    _snwprintf(format, 100, L"%ls, %ls", size, rate);
    (void)index;
}

// ---------------------------------------------------------------------------
// Commands

static int CmdStatus()
{
    int count = CameraCount();
    Out(L"Show2Cam %ls; driver devices: %d; cameras: %d; control panel: %ls", L"" S2C_VER_STR, SetupDeviceCount(), count,
        CamPanelRunning() ? L"running" : L"not running");
    CamInfo cams[S2C_MAX_CAMERAS_UI];
    int n = CamList(cams, S2C_MAX_CAMERAS_UI);
    if (!n) Out(L"no Show2Cam cameras");
    for (int i = 0; i < n; i++)
    {
        CamConfig c;
        CamConfigLoad(cams[i].index, &c);
        wchar_t source[400], sound[300], format[100];
        ConfigText(cams[i].index, c, source, sound, format);
        Out(L"camera %d: \"%ls\"", cams[i].index + 1, cams[i].name);
        Out(L"  source: %ls", source);
        if (c.kind == SourceVideo) Out(L"  sound:  %ls", sound);
        Out(L"  wanted: %ls", format);
        HANDLE h = CamOpen(cams[i].path);
        S2C_STATUS st = {};
        if (h == INVALID_HANDLE_VALUE || !CamGetStatus(h, &st))
        {
            Out(L"  driver: no status (error %lu)", GetLastError());
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
            continue;
        }
        CloseHandle(h);
        wchar_t users[400] = L"";
        for (int u = 0; u < S2C_MAX_USERS && st.PinsOpen; u++)
        {
            if (!st.UserPids[u]) continue;
            wchar_t name[64], one[96];
            WCHAR dn[33];
            wcsncpy(dn, st.UserNames[u], 32);
            dn[32] = 0;
            ProcessName(st.UserPids[u], dn, name, 64);
            _snwprintf(one, 96, L"%ls%ls (%lu)", users[0] ? L", " : L"", name, st.UserPids[u]);
            one[95] = 0;
            wcsncat(users, one, 399 - wcslen(users));
        }
        Out(L"  camera: %lux%lu %lu fps; %ls%ls%ls", st.Width, st.Height, st.Fps,
            st.PinsOpen ? L"in use" : L"not in use", users[0] ? L" by " : L"", users);
        Out(L"  frames: %llu delivered, %llu dropped; pictures from the panel: %llu%ls", st.FramesDelivered, st.FramesDropped,
            st.PicturesReceived, st.SourceWidth ? L"" : L" (showing the test pattern)");
    }
    return ExitOk;
}

static int CmdCount(int argc, wchar_t** argv)
{
    ULONG n;
    if (argc < 1 || !ParseUlong(argv[0], &n) || n < 1 || n > S2C_MAX_CAMERAS_UI)
    {
        Out(L"usage: s2cctl count N   (1..%d)", S2C_MAX_CAMERAS_UI);
        return ExitUsage;
    }
    if (!IsElevated())
    {
        Out(L"administrator rights needed (run from an elevated command prompt)");
        return ExitAdmin;
    }
    bool reboot = false;
    bool ok = SetupSetCameraCount((int)n, true, &reboot, [](void*, const wchar_t* line) { Out(L"  %ls", line); }, nullptr);
    if (!ok)
    {
        Out(L"could not set the number of cameras to %lu", n);
        return ExitFailed;
    }
    // the cameras' device interfaces appear a moment later
    for (int t = 0; t < 50; t++)
    {
        CamInfo cams[S2C_MAX_CAMERAS_UI];
        if (CamList(cams, S2C_MAX_CAMERAS_UI) == (int)n) break;
        Sleep(200);
    }
    CamConfigNotifyPanel(-1);
    Out(L"cameras: %lu%ls", n, reboot ? L" (Windows asks for a restart)" : L"");
    return ExitOk;
}

static int CmdName(int argc, wchar_t** argv)
{
    int list[S2C_MAX_CAMERAS_UI], n;
    if (argc < 2 || !ParseCameras(argv[0], list, &n, false))
    {
        Out(L"usage: s2cctl name CAM \"NAME\"   (\"\" or default = the default name)");
        return ExitUsage;
    }
    const wchar_t* name = _wcsicmp(argv[1], L"default") ? argv[1] : L"";
    if (wcslen(name) >= S2C_NAME_CHARS)
    {
        Out(L"the name is too long (at most %d characters)", S2C_NAME_CHARS - 1);
        return ExitUsage;
    }
    CamInfo cam;
    if (!FindCamera(list[0], &cam))
    {
        Out(L"camera %d is not there", list[0] + 1);
        return ExitFailed;
    }
    HANDLE h = CamOpen(cam.path);
    bool ok = h != INVALID_HANDLE_VALUE && CamSetName(h, name);
    DWORD err = GetLastError();
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok)
    {
        Out(L"could not rename camera %d (error %lu)", list[0] + 1, err);
        return ExitFailed;
    }
    // read back (the device interface's FriendlyName follows)
    wchar_t now[S2C_NAME_CHARS] = L"";
    for (int t = 0; t < 20; t++)
    {
        if (FindCamera(list[0], &cam)) wcscpy(now, cam.name);
        if (name[0] ? !wcscmp(now, name) : wcsstr(now, L"Show2Cam") != nullptr) break;
        Sleep(100);
    }
    CamConfigNotifyPanel(list[0]);
    Out(L"camera %d: name \"%ls\"", list[0] + 1, now);
    return ExitOk;
}

static int SaveAll(const int* list, int n, const CamConfig* configs)
{
    for (int i = 0; i < n; i++) CamConfigSave(list[i], configs[i]);
    CamConfigNotifyPanel(n == 1 ? list[0] : -1);
    for (int i = 0; i < n; i++)
    {
        wchar_t source[400], sound[300], format[100];
        ConfigText(list[i], configs[i], source, sound, format);
        Out(L"camera %d: %ls; %ls%ls%ls", list[i] + 1, source, format, configs[i].kind == SourceVideo ? L"; sound " : L"",
            configs[i].kind == SourceVideo ? sound : L"");
    }
    if (!CamPanelRunning()) Out(L"note: the control panel is not running; the cameras show their sources while it runs");
    return ExitOk;
}

static int CmdSource(int argc, wchar_t** argv)
{
    int list[S2C_MAX_CAMERAS_UI], n;
    const wchar_t* usage = L"usage: s2cctl source CAM text [\"TEXT\"] | images [FOLDER] | video [FOLDER] | stream URL | generator";
    if (argc < 2 || !ParseCameras(argv[0], list, &n, true))
    {
        Out(L"%ls", usage);
        return ExitUsage;
    }
    int kind = -1;
    for (int k = 0; k < SourceKindCount; k++)
        if (!_wcsicmp(argv[1], KindName(k))) kind = k;
    const wchar_t* value = argc >= 3 ? argv[2] : nullptr;
    if (kind < 0 || (kind == SourceStream && !value) || (kind == SourceGenerator && value))
    {
        Out(L"%ls", usage);
        return ExitUsage;
    }
    if (kind == SourceStream && _wcsnicmp(value, L"http://", 7) && _wcsnicmp(value, L"https://", 8))
    {
        Out(L"the stream address must start with http:// or https://");
        return ExitUsage;
    }
    if ((kind == SourceImages || kind == SourceVideo) && value && _wcsicmp(value, L"default"))
    {
        DWORD a = GetFileAttributesW(value);
        if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY))
        {
            Out(L"no such folder: %ls", value);
            return ExitFailed;
        }
    }
    if (value && wcslen(value) >= (kind == SourceText ? 256u : kind == SourceStream ? 512u : (unsigned)MAX_PATH))
    {
        Out(L"too long: %ls", value);
        return ExitUsage;
    }
    CamConfig configs[S2C_MAX_CAMERAS_UI];
    for (int i = 0; i < n; i++)
    {
        CamConfig& c = configs[i];
        CamConfigLoad(list[i], &c);
        c.kind = kind;
        if (kind == SourceText && value) wcscpy(c.text, value);
        if (kind == SourceImages && value)
        {
            if (!_wcsicmp(value, L"default")) c.imageFolder[0] = 0;
            else GetFullPathNameW(value, MAX_PATH, c.imageFolder, nullptr);
        }
        if (kind == SourceVideo && value)
        {
            if (!_wcsicmp(value, L"default")) c.videoFolder[0] = 0;
            else GetFullPathNameW(value, MAX_PATH, c.videoFolder, nullptr);
        }
        if (kind == SourceStream) wcscpy(c.url, value);
    }
    return SaveAll(list, n, configs);
}

static int CmdAudio(int argc, wchar_t** argv)
{
    int list[S2C_MAX_CAMERAS_UI], n;
    if (argc < 2 || !ParseCameras(argv[0], list, &n, true))
    {
        Out(L"usage: s2cctl audio CAM speak2mic|off|DEVICE   (DEVICE: part of a playback device's name, or its id)");
        return ExitUsage;
    }
    int mode;
    RenderDevice dev = {};
    if (!_wcsicmp(argv[1], L"speak2mic")) mode = AudioSpeak2Mic;
    else if (!_wcsicmp(argv[1], L"off")) mode = AudioOff;
    else
    {
        mode = AudioDevice;
        static RenderDevice devs[64];
        int count = ListRenderDevices(devs, 64), found = -1, matches = 0;
        for (int i = 0; i < count; i++)
        {
            if (!_wcsicmp(devs[i].id, argv[1])) { found = i; matches = 1; break; }
            wchar_t a[256], b[256];
            wcsncpy(a, devs[i].name, 255); a[255] = 0;
            wcsncpy(b, argv[1], 255); b[255] = 0;
            _wcslwr(a);
            _wcslwr(b);
            if (wcsstr(a, b)) { found = i; matches++; }
        }
        if (matches != 1)
        {
            Out(matches ? L"\"%ls\" matches several playback devices:" : L"no playback device \"%ls\"; there are:", argv[1]);
            for (int i = 0; i < count; i++) Out(L"  %ls", devs[i].name);
            return matches ? ExitUsage : ExitFailed;
        }
        dev = devs[found];
    }
    CamConfig configs[S2C_MAX_CAMERAS_UI];
    for (int i = 0; i < n; i++)
    {
        CamConfigLoad(list[i], &configs[i]);
        configs[i].audioMode = mode;
        if (mode == AudioDevice) wcscpy(configs[i].audioDevice, dev.id);
    }
    for (int i = 0; i < n; i++)
        Out(L"camera %d: video sound %ls", list[i] + 1, mode == AudioOff ? L"off" : mode == AudioDevice ? dev.name : L"Speak2Mic Speaker");
    for (int i = 0; i < n; i++) CamConfigSave(list[i], configs[i]);
    CamConfigNotifyPanel(n == 1 ? list[0] : -1);
    return ExitOk;
}

static int CmdFormat(int argc, wchar_t** argv)
{
    int list[S2C_MAX_CAMERAS_UI], n;
    const wchar_t* usage = L"usage: s2cctl format CAM source|WIDTHxHEIGHT [FPS|source]";
    if (argc < 2 || !ParseCameras(argv[0], list, &n, true))
    {
        Out(L"%ls", usage);
        return ExitUsage;
    }
    ULONG w = 0, h = 0, fps = 0;
    bool setFps = argc >= 3;
    if (_wcsicmp(argv[1], L"source"))
    {
        wchar_t buf[40];
        wcsncpy(buf, argv[1], 39);
        buf[39] = 0;
        wchar_t* x = wcschr(buf, L'x');
        if (!x) x = wcschr(buf, L'X');
        if (!x) x = wcschr(buf, L'*');
        if (!x) { Out(L"%ls", usage); return ExitUsage; }
        *x = 0;
        if (!ParseUlong(buf, &w) || !ParseUlong(x + 1, &h) || w < S2C_MIN_WIDTH || w > S2C_MAX_WIDTH || h < S2C_MIN_HEIGHT ||
            h > S2C_MAX_HEIGHT)
        {
            Out(L"the size must be %dx%d .. %dx%d", S2C_MIN_WIDTH, S2C_MIN_HEIGHT, S2C_MAX_WIDTH, S2C_MAX_HEIGHT);
            return ExitUsage;
        }
        if ((w | h) & 1)
        {
            Out(L"the width and height must be even");
            return ExitUsage;
        }
    }
    if (setFps && _wcsicmp(argv[2], L"source") && (!ParseUlong(argv[2], &fps) || fps < 1 || fps > S2C_MAX_FPS))
    {
        Out(L"the frame rate must be 1..%d or \"source\"", S2C_MAX_FPS);
        return ExitUsage;
    }
    CamConfig configs[S2C_MAX_CAMERAS_UI];
    for (int i = 0; i < n; i++)
    {
        CamConfigLoad(list[i], &configs[i]);
        configs[i].width = w;
        configs[i].height = h;
        if (setFps) configs[i].fps = fps;
    }
    int rc = SaveAll(list, n, configs);
    Out(L"note: a camera that a program has open gets the new format when it is closed");
    return rc;
}

static int CmdPause(int argc, wchar_t** argv)
{
    int list[S2C_MAX_CAMERAS_UI], n;
    if (argc < 2 || !ParseCameras(argv[0], list, &n, true) || (_wcsicmp(argv[1], L"on") && _wcsicmp(argv[1], L"off")))
    {
        Out(L"usage: s2cctl pause CAM on|off");
        return ExitUsage;
    }
    CamConfig configs[S2C_MAX_CAMERAS_UI];
    for (int i = 0; i < n; i++)
    {
        CamConfigLoad(list[i], &configs[i]);
        configs[i].paused = !_wcsicmp(argv[1], L"on");
    }
    return SaveAll(list, n, configs);
}

static int CmdReset(int argc, wchar_t** argv)
{
    int list[S2C_MAX_CAMERAS_UI], n;
    if (argc < 1 || !ParseCameras(argv[0], list, &n, true))
    {
        Out(L"usage: s2cctl reset CAM|all");
        return ExitUsage;
    }
    CamConfig configs[S2C_MAX_CAMERAS_UI];
    bool ok = true;
    for (int i = 0; i < n; i++)
    {
        CamConfigDefault(list[i], &configs[i]);
        CamInfo cam;
        if (FindCamera(list[i], &cam))
        {
            HANDLE h = CamOpen(cam.path);
            if (h == INVALID_HANDLE_VALUE || !CamSetName(h, L""))
            {
                Out(L"camera %d: could not set the default name (error %lu)", list[i] + 1, GetLastError());
                ok = false;
            }
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
    }
    SaveAll(list, n, configs);
    return ok ? ExitOk : ExitFailed;
}

static int CmdExport(int argc, wchar_t** argv)
{
    if (argc < 1)
    {
        Out(L"usage: s2cctl export FILE.ini");
        return ExitUsage;
    }
    wchar_t path[MAX_PATH];
    GetFullPathNameW(argv[0], MAX_PATH, path, nullptr);
    if (!CamIniCreate(path))
    {
        Out(L"cannot write %ls (error %lu)", path, GetLastError());
        return ExitFailed;
    }
    int count = CameraCount();
    CamInfo cams[S2C_MAX_CAMERAS_UI];
    int present = CamList(cams, S2C_MAX_CAMERAS_UI);
    CamIniWriteHeader(path, count);
    for (int i = 0; i < count; i++)
    {
        CamConfig c;
        CamConfigLoad(i, &c);
        const wchar_t* name = L"";
        for (int j = 0; j < present; j++)
            if (cams[j].index == i) name = cams[j].name;
        CamIniWriteCamera(path, i, name, c);
    }
    if (!CamIniFlush(path))
    {
        Out(L"cannot write %ls (error %lu)", path, GetLastError());
        return ExitFailed;
    }
    Out(L"settings of %d camera(s) exported to %ls", count, path);
    return ExitOk;
}

static int CmdImport(int argc, wchar_t** argv)
{
    if (argc < 1)
    {
        Out(L"usage: s2cctl import FILE.ini");
        return ExitUsage;
    }
    wchar_t path[MAX_PATH];
    GetFullPathNameW(argv[0], MAX_PATH, path, nullptr);
    int count = 0;
    if (!CamIniCheck(path, &count))
    {
        Out(L"%ls is not a Show2Cam settings file", path);
        return ExitFailed;
    }
    int now = CameraCount();
    if (count && count != now && !IsElevated())
    {
        Out(L"the file has %d camera(s), now there are %d: administrator rights needed to change that", count, now);
        return ExitAdmin;
    }
    int rc = ExitOk;
    if (count && count != now)
    {
        bool reboot = false;
        if (!SetupSetCameraCount(count, true, &reboot, [](void*, const wchar_t* line) { Out(L"  %ls", line); }, nullptr))
        {
            Out(L"could not set the number of cameras to %d", count);
            rc = ExitFailed;
        }
        for (int t = 0; t < 50; t++)
        {
            CamInfo cams[S2C_MAX_CAMERAS_UI];
            if (CamList(cams, S2C_MAX_CAMERAS_UI) == count) break;
            Sleep(200);
        }
    }
    int done = 0;
    for (int i = 0; i < S2C_MAX_CAMERAS_UI; i++)
    {
        CamConfig c;
        CamConfigLoad(i, &c);
        wchar_t name[S2C_NAME_CHARS];
        bool hasName = false;
        if (!CamIniReadCamera(path, i, &c, name, &hasName)) continue;
        CamConfigSave(i, c);
        done++;
        CamInfo cam;
        if (hasName && FindCamera(i, &cam) && wcscmp(cam.name, name))
        {
            HANDLE h = CamOpen(cam.path);
            if (h == INVALID_HANDLE_VALUE || !CamSetName(h, name))
            {
                Out(L"camera %d: could not rename to \"%ls\" (error %lu)", i + 1, name, GetLastError());
                rc = ExitFailed;
            }
            if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        }
    }
    CamConfigNotifyPanel(-1);
    Out(L"settings of %d camera(s) imported from %ls", done, path);
    return rc;
}

static int CmdPicture(int argc, wchar_t** argv)
{
    int list[S2C_MAX_CAMERAS_UI], n;
    if (argc < 2 || !ParseCameras(argv[0], list, &n, true))
    {
        Out(L"usage: s2cctl picture CAM FILE|pattern");
        return ExitUsage;
    }
    bool pattern = !_wcsicmp(argv[1], L"pattern");
    UINT iw = 0, ih = 0;
    if (!pattern && !ImageFileSize(argv[1], &iw, &ih))
    {
        Out(L"cannot read the picture %ls", argv[1]);
        return ExitFailed;
    }
    if (CamPanelRunning()) Out(L"note: the control panel is running: its sources replace the picture");
    int rc = ExitOk;
    for (int i = 0; i < n; i++)
    {
        CamInfo cam;
        if (!FindCamera(list[i], &cam))
        {
            Out(L"camera %d is not there", list[i] + 1);
            rc = ExitFailed;
            continue;
        }
        HANDLE h = CamOpen(cam.path);
        S2C_STATUS st = {};
        bool ok = h != INVALID_HANDLE_VALUE && CamGetStatus(h, &st);
        if (ok && pattern) ok = CamSendTestPattern(h);
        else if (ok)
        {
            CamFrame frame;
            UINT sw, sh;
            ok = frame.Resize(st.Width, st.Height) &&
                 RenderImageFile(argv[1], frame.Pixels(), (int)st.Width, (int)st.Height, &sw, &sh) && CamSendFrame(h, frame);
        }
        DWORD err = GetLastError();
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        if (ok) Out(L"camera %d: %ls", list[i] + 1, pattern ? L"test pattern" : argv[1]);
        else
        {
            Out(L"camera %d: failed (error %lu)", list[i] + 1, err);
            rc = ExitFailed;
        }
    }
    return rc;
}

static void Usage()
{
    Out(L"Show2Cam %ls - command line control", L"" S2C_VER_STR);
    Out(L"  s2cctl status");
    Out(L"  s2cctl count N                                 (1..%d; administrator)", S2C_MAX_CAMERAS_UI);
    Out(L"  s2cctl name CAM \"NAME\"                         (\"\" or default = default name)");
    Out(L"  s2cctl source CAM text [\"TEXT\"] | images [FOLDER] | video [FOLDER] | stream URL | generator");
    Out(L"  s2cctl audio CAM speak2mic|off|DEVICE");
    Out(L"  s2cctl format CAM source|WIDTHxHEIGHT [FPS|source]");
    Out(L"  s2cctl pause CAM on|off");
    Out(L"  s2cctl reset CAM");
    Out(L"  s2cctl export FILE.ini  |  s2cctl import FILE.ini");
    Out(L"  s2cctl picture CAM FILE|pattern");
    Out(L"CAM: camera number 1..%d, or \"all\" (not for name). Exit codes: 0 ok, 1 failed, 2 bad arguments,", S2C_MAX_CAMERAS_UI);
    Out(L"3 administrator rights needed, 4 Show2Cam cannot work here.");
}

int wmain(int argc, wchar_t** argv)
{
    _setmode(_fileno(stdout), _O_U8TEXT);
    AppLogOpen(L"ctl");
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        wchar_t cmd[1024] = L"";
        for (int i = 1; i < argc; i++)
        {
            wcsncat(cmd, L" ", 1023 - wcslen(cmd));
            wcsncat(cmd, argv[i], 1023 - wcslen(cmd));
        }
        AppLog(L"s2cctl%ls", cmd);
    }
    if (argc < 2 || !wcscmp(argv[1], L"/?") || !_wcsicmp(argv[1], L"help") || !_wcsicmp(argv[1], L"--help") || !wcscmp(argv[1], L"-h"))
    {
        Usage();
        return argc < 2 ? ExitUsage : ExitOk;
    }
    const wchar_t* c = argv[1];
#ifdef S2C_UI_TEST
    S2cNotReady why = S2cReadyOk;                   // Wine checks: pretend cameras (camdev.cpp)
#else
    S2cNotReady why = S2cCheckReady();
#endif
    if (why != S2cReadyOk && _wcsicmp(c, L"export"))
    {
        Out(L"%ls", S2cNotReadyTextEn(why));
        return ExitNotReady;
    }
    int rest = argc - 2;
    wchar_t** args = argv + 2;
    int rc;
    if (!_wcsicmp(c, L"status")) rc = CmdStatus();
    else if (!_wcsicmp(c, L"count")) rc = CmdCount(rest, args);
    else if (!_wcsicmp(c, L"name")) rc = CmdName(rest, args);
    else if (!_wcsicmp(c, L"source")) rc = CmdSource(rest, args);
    else if (!_wcsicmp(c, L"audio")) rc = CmdAudio(rest, args);
    else if (!_wcsicmp(c, L"format")) rc = CmdFormat(rest, args);
    else if (!_wcsicmp(c, L"pause")) rc = CmdPause(rest, args);
    else if (!_wcsicmp(c, L"reset")) rc = CmdReset(rest, args);
    else if (!_wcsicmp(c, L"export")) rc = CmdExport(rest, args);
    else if (!_wcsicmp(c, L"import")) rc = CmdImport(rest, args);
    else if (!_wcsicmp(c, L"picture")) rc = CmdPicture(rest, args);
    else
    {
        Out(L"unknown command \"%ls\"", c);
        Usage();
        rc = ExitUsage;
    }
    AppLog(L"exit code %d", rc);
    CoUninitialize();
    return rc;
}
