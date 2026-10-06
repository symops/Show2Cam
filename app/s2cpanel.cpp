// Show2Cam.exe - the control panel: what every Show2Cam camera shows.
//
//  * Cameras: the list of the cameras with what they show and whether a program uses them right now; the number
//    of cameras (a device restart with administrator rights, like "Apply" in Speak2Mic).
//  * The selected camera: its name in Windows, its source (text, a folder of images, a folder of videos, an MJPEG
//    stream), the sound of the videos (Speak2Mic Speaker by default), its size and frame rate ("as the source" or
//    chosen), "Check" (a live preview window) and "Play" / "Pause".
// The cameras show their sources while the panel runs (also minimised to the tray); without it they show the
// driver's test pattern.
#include "sources.h"
#include "audioout.h"
#include "camdev.h"
#include "lang.h"
#include "applog.h"
#include "devctl.h"
#include "ready.h"
#include "autostart.h"
#include "../driver/version.h"
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <mfapi.h>
#include <stdio.h>
#include <wchar.h>

enum
{
    IDC_RESET_ALL = 100, IDC_L_LANG, IDC_LANG,
    IDC_GROUP1, IDC_LIST, IDC_L_COUNT, IDC_COUNT, IDC_COUNT_APPLY,
    IDC_GROUP2, IDC_L_NAME, IDC_NAME, IDC_RENAME, IDC_L_SOURCE, IDC_SOURCE, IDC_L_PARAM, IDC_PARAM, IDC_BROWSE, IDC_OPENFOLDER,
    IDC_L_AUDIO, IDC_AUDIO, IDC_L_RES, IDC_RES, IDC_L_FPS, IDC_FPS, IDC_CAM_STATUS, IDC_TEST, IDC_PLAY, IDC_SELFVIEW,
    IDC_EVENTS, IDC_CLEARLOG, IDC_AUTOSTART,
};

enum { TIMER_STATUS = 1, TIMER_PARAM = 2, TIMER_RESCAN = 3 };
#define WM_APP_CAMEVENT (WM_APP + 30)
#define WM_APP_TRAY     (WM_APP + 31)
enum { IDM_TRAY_OPEN = 40001, IDM_TRAY_EXIT };
#define S2C_USER_KEY L"Software\\Show2Cam"

// ---------------------------------------------------------------------------
// State

static HINSTANCE g_inst;
static HWND      g_wnd;
static HFONT     g_font, g_fontBold;
static HICON     g_toolIcons[2];          // reset / clear log (resources 10, 11)
static UINT      g_dpi = 96;
static bool      g_elevated, g_updating;

struct Cam
{
    CamInfo       info;
    CamConfig     config;
    CameraRunner* runner;
    CamRunStatus  status;
    bool          audioMissingLogged;
    HWND          preview;                // its "Check" window (several may be open at once)
    Picture       previewPic;
};
static Cam  g_cams[S2C_MAX_CAMERAS_UI];
static int  g_camCount;
static int  g_sel = -1;                   // selected camera (index into g_cams)
static RenderDevice g_audioDevs[64];
static int  g_audioDevCount;

static HWND Ctl(int id) { return GetDlgItem(g_wnd, id); }
static int S(int v) { return MulDiv(v, (int)g_dpi, 96); }

static void SetText(int id, const wchar_t* text)
{
    wchar_t cur[1024];
    GetWindowTextW(Ctl(id), cur, 1024);
    if (wcscmp(cur, text) != 0) SetWindowTextW(Ctl(id), text);
}

static void ComboAdd(int id, const wchar_t* text, LPARAM data)
{
    int i = (int)SendMessageW(Ctl(id), CB_ADDSTRING, 0, (LPARAM)text);
    SendMessageW(Ctl(id), CB_SETITEMDATA, i, data);
}
static int ComboSel(int id) { return (int)SendMessageW(Ctl(id), CB_GETCURSEL, 0, 0); }
static LPARAM ComboData(int id) { int i = ComboSel(id); return i < 0 ? -1 : SendMessageW(Ctl(id), CB_GETITEMDATA, i, 0); }
static bool ComboSelectData(int id, LPARAM data)
{
    int n = (int)SendMessageW(Ctl(id), CB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; i++)
        if (SendMessageW(Ctl(id), CB_GETITEMDATA, i, 0) == data)
        {
            SendMessageW(Ctl(id), CB_SETCURSEL, i, 0);
            return true;
        }
    return false;
}

static const wchar_t* FileName(const wchar_t* path)
{
    const wchar_t* s = wcsrchr(path, L'\\');
    return s ? s + 1 : path;
}

// ---------------------------------------------------------------------------
// Settings of the cameras (per user): HKCU\Software\Show2Cam\Camera<N>

static void ConfigKey(int index, wchar_t* key)
{
    _snwprintf(key, 64, L"%ls\\Camera%d", S2C_USER_KEY, index + 1);
    key[63] = 0;
}

static void DefaultConfig(int index, CamConfig* c)
{
    *c = CamConfig();
    _snwprintf(c->text, 256, L"Camera %d", index + 1);
}

static void LoadConfig(int index, CamConfig* c)
{
    DefaultConfig(index, c);
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
        DWORD size = chars * sizeof(wchar_t);
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
    str(L"AudioDevice", c->audioDevice, 256);
    c->width = num(L"Width", 0);
    c->height = num(L"Height", 0);
    c->fps = num(L"Fps", 0);
    c->paused = num(L"Paused", 0) != 0;
    c->selfView = num(L"SelfView", 0) != 0;
    RegCloseKey(k);
}

static void SaveConfig(int index, const CamConfig& c)
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
    num(L"SelfView", c.selfView ? 1 : 0);
    RegCloseKey(k);
}

// ---------------------------------------------------------------------------
// Event list (as in Speak2Mic): a drop-down list of events with date and time, kept in
// %ProgramData%\Show2Cam\logs\events.log (UTF-16, newest at the bottom, the last kMaxEvents).

static const int kMaxEvents = 500;
static int g_eventsWidest;

static void EventsPath(wchar_t* path)
{
    _snwprintf(path, MAX_PATH, L"%ls\\events.log", AppLogDir());
    path[MAX_PATH - 1] = 0;
}

static void AppendEventFile(const wchar_t* line)
{
    wchar_t path[MAX_PATH];
    EventsPath(path);
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA | FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    LARGE_INTEGER size = {};
    GetFileSizeEx(f, &size);
    if (size.QuadPart == 0)
    {
        const WORD bom = 0xFEFF;
        WriteFile(f, &bom, sizeof(bom), &written, nullptr);
    }
    WriteFile(f, line, (DWORD)(wcslen(line) * sizeof(wchar_t)), &written, nullptr);
    WriteFile(f, L"\r\n", 2 * sizeof(wchar_t), &written, nullptr);
    CloseHandle(f);
}

static int EventTextWidth(const wchar_t* text)
{
    HWND box = Ctl(IDC_EVENTS);
    HDC dc = GetDC(box);
    HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(box, WM_GETFONT, 0, 0));
    SIZE sz = {};
    GetTextExtentPoint32W(dc, text, (int)wcslen(text), &sz);
    SelectObject(dc, old);
    ReleaseDC(box, dc);
    return sz.cx;
}

static void FitEventList(const wchar_t* added)
{
    HWND box = Ctl(IDC_EVENTS);
    if (!added)
    {
        g_eventsWidest = 0;
        int count = (int)SendMessageW(box, CB_GETCOUNT, 0, 0);
        wchar_t t[700];
        for (int i = 0; i < count; i++)
            if (SendMessageW(box, CB_GETLBTEXTLEN, i, 0) < 699 && SendMessageW(box, CB_GETLBTEXT, i, (LPARAM)t) >= 0)
            {
                int w = EventTextWidth(t);
                if (w > g_eventsWidest) g_eventsWidest = w;
            }
    }
    else
    {
        int w = EventTextWidth(added);
        if (w > g_eventsWidest) g_eventsWidest = w;
    }
    RECT r, work;
    GetWindowRect(box, &r);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int want = g_eventsWidest + GetSystemMetrics(SM_CXVSCROLL) + 12;
    int maxWidth = work.right - r.left;
    if (want > maxWidth) want = maxWidth;
    if (want < r.right - r.left) want = r.right - r.left;
    SendMessageW(box, CB_SETDROPPEDWIDTH, want, 0);
}

static void LoadEvents()
{
    wchar_t path[MAX_PATH];
    EventsPath(path);
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(f, nullptr), got = 0;
    wchar_t* text = size >= 2 && size < 8 * 1024 * 1024 ? (wchar_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 4) : nullptr;
    bool ok = text && ReadFile(f, text, size, &got, nullptr) && got == size;
    CloseHandle(f);
    if (!ok)
    {
        if (text) HeapFree(GetProcessHeap(), 0, text);
        return;
    }
    wchar_t* p = text[0] == 0xFEFF ? text + 1 : text;
    static wchar_t* lines[20000];
    int n = 0;
    for (wchar_t* q = p; *q && n < 20000;)
    {
        wchar_t* e = q;
        while (*e && *e != L'\r' && *e != L'\n') e++;
        bool end = !*e;
        *e = 0;
        if (*q) lines[n++] = q;
        if (end) break;
        q = e + 1;
        while (*q == L'\r' || *q == L'\n') q++;
    }
    int from = n > kMaxEvents ? n - kMaxEvents : 0;
    HWND box = Ctl(IDC_EVENTS);
    for (int i = from; i < n; i++) SendMessageW(box, CB_ADDSTRING, 0, (LPARAM)lines[i]);
    int count = (int)SendMessageW(box, CB_GETCOUNT, 0, 0);
    if (count > 0) SendMessageW(box, CB_SETCURSEL, count - 1, 0);
    if (from > 0)
    {
        HANDLE w = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (w != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            const WORD bom = 0xFEFF;
            WriteFile(w, &bom, sizeof(bom), &written, nullptr);
            for (int i = from; i < n; i++)
            {
                WriteFile(w, lines[i], (DWORD)(wcslen(lines[i]) * sizeof(wchar_t)), &written, nullptr);
                WriteFile(w, L"\r\n", 2 * sizeof(wchar_t), &written, nullptr);
            }
            CloseHandle(w);
        }
    }
    HeapFree(GetProcessHeap(), 0, text);
}

static void AddEvent(const wchar_t* text)
{
    if (!text || !*text) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t date[40] = L"", time[40] = L"", item[700];
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE, &st, nullptr, date, 40, nullptr);
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, nullptr, time, 40);
    _snwprintf(item, 700, L"%ls %ls  %ls", date, time, text);
    item[699] = 0;
    for (wchar_t* c = item; *c; c++)
        if (*c == L'\r' || *c == L'\n') *c = L' ';
    HWND box = Ctl(IDC_EVENTS);
    int index = (int)SendMessageW(box, CB_ADDSTRING, 0, (LPARAM)item);
    FitEventList(item);
    SendMessageW(box, CB_SETCURSEL, index, 0);
    while (SendMessageW(box, CB_GETCOUNT, 0, 0) > kMaxEvents) SendMessageW(box, CB_DELETESTRING, 0, 0);
    AppendEventFile(item);
    AppLog(L"event: %ls", text);
}

static void AddEventF(const wchar_t* fmt, ...)
{
    wchar_t t[600];
    va_list a;
    va_start(a, fmt);
    _vsnwprintf(t, 600, fmt, a);
    va_end(a);
    t[599] = 0;
    AddEvent(t);
}

static void ClearEvents()
{
    SendMessageW(Ctl(IDC_EVENTS), CB_RESETCONTENT, 0, 0);
    g_eventsWidest = 0;
    wchar_t path[MAX_PATH];
    EventsPath(path);
    DeleteFileW(path);
    AppLog(L"event log cleared");
    AddEvent(TR(L"Журнал событий очищен."));
}

// Tooltip of the closed event list: the full text of the shown event when it does not fit.
static HWND    g_eventTip;
static wchar_t g_eventTipText[700];
static void EventTipText(NMTTDISPINFOW* info)
{
    HWND box = Ctl(IDC_EVENTS);
    g_eventTipText[0] = 0;
    int sel = (int)SendMessageW(box, CB_GETCURSEL, 0, 0);
    if (sel >= 0 && SendMessageW(box, CB_GETLBTEXTLEN, sel, 0) < 699)
    {
        SendMessageW(box, CB_GETLBTEXT, sel, (LPARAM)g_eventTipText);
        RECT r;
        GetClientRect(box, &r);
        int room = r.right - r.left - GetSystemMetrics(SM_CXVSCROLL) - 8;
        if (EventTextWidth(g_eventTipText) <= room) g_eventTipText[0] = 0;
    }
    info->lpszText = g_eventTipText;
}

// ---------------------------------------------------------------------------
// Texts

static const wchar_t* SourceName(int kind)
{
    switch (kind)
    {
    case SourceImages: return TR(L"Изображения");
    case SourceVideo:  return TR(L"Видео");
    case SourceStream: return TR(L"MJPEG-поток");
    default:           return TR(L"Текст");
    }
}

// "1280×720, 30 кадр/с"
static void FormatText(wchar_t* t, size_t n, ULONG w, ULONG h, ULONG fps)
{
    _snwprintf(t, n, TR(L"%lu×%lu, %lu кадр/с"), w, h, fps);
    t[n - 1] = 0;
}

static bool InUse(const CamRunStatus& s)
{
    return s.deviceOpen && (s.driver.Streaming > 0 || s.driver.PinsOpen > 0);
}

// ---------------------------------------------------------------------------
// Who uses a camera. The driver tells which processes opened it: a DirectShow program itself, or the Windows Frame
// Server for Media Foundation programs (Teams, browsers, the Camera app, our self-view). For the latter the program is
// what Windows' camera privacy records as using a camera right now (LastUsedTimeStop = 0; per user, not per camera).

static wchar_t g_appsNow[16][64];
static int     g_appsNowCount;

static void CameraAppsNow()
{
    g_appsNowCount = 0;
    const wchar_t* base = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\webcam";
    auto scan = [](HKEY parent, bool nonPackaged) {
        wchar_t sub[512];
        for (DWORD i = 0; g_appsNowCount < 16; i++)
        {
            DWORD len = 512;
            if (RegEnumKeyExW(parent, i, sub, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            if (!nonPackaged && !_wcsicmp(sub, L"NonPackaged")) continue;
            ULONGLONG start = 0, stop = 0;
            DWORD sz = sizeof(start);
            RegGetValueW(parent, sub, L"LastUsedTimeStart", RRF_RT_REG_QWORD, nullptr, &start, &sz);
            sz = sizeof(stop);
            RegGetValueW(parent, sub, L"LastUsedTimeStop", RRF_RT_REG_QWORD, nullptr, &stop, &sz);
            if (!start || stop) continue;
            // "C:#Program Files#App#app.exe" -> "app.exe"; "Microsoft.WindowsCamera_8wekyb3d8bbwe" -> "Microsoft.WindowsCamera"
            const wchar_t* name = nonPackaged ? wcsrchr(sub, L'#') : nullptr;
            name = name ? name + 1 : sub;
            wchar_t* out = g_appsNow[g_appsNowCount];
            wcsncpy(out, name, 63);
            out[63] = 0;
            if (!nonPackaged)
            {
                wchar_t* u = wcschr(out, L'_');
                if (u) *u = 0;
            }
            g_appsNowCount++;
        }
    };
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, base, 0, KEY_READ, &k) == ERROR_SUCCESS)
    {
        scan(k, false);
        HKEY np;
        if (RegOpenKeyExW(k, L"NonPackaged", 0, KEY_READ, &np) == ERROR_SUCCESS)
        {
            scan(np, true);
            RegCloseKey(np);
        }
        RegCloseKey(k);
    }
}

static void ProcessName(DWORD pid, wchar_t* out, size_t len)
{
    out[0] = 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return;
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    if (QueryFullProcessImageNameW(h, 0, path, &n))
    {
        const wchar_t* f = wcsrchr(path, L'\\');
        wcsncpy(out, f ? f + 1 : path, len - 1);
        out[len - 1] = 0;
    }
    CloseHandle(h);
}

static void AddName(wchar_t* list, size_t len, const wchar_t* name)
{
    if (!name[0]) return;
    // no duplicates
    size_t n = wcslen(name);
    for (const wchar_t* p = wcsstr(list, name); p; p = wcsstr(p + 1, name))
        if ((p == list || p[-1] == L' ') && (p[n] == 0 || p[n] == L','))
            return;
    if (list[0]) wcsncat(list, L", ", len - wcslen(list) - 1);
    wcsncat(list, name, len - wcslen(list) - 1);
}

// "самотрансляция, Teams.exe" / "" when not in use.
static void UsersText(const CamRunStatus& s, wchar_t* out, size_t len)
{
    out[0] = 0;
    if (!InUse(s)) return;
    bool self = s.selfView && s.self.open;
    if (self) AddName(out, len, TR(L"самотрансляция"));
    int frameServer = 0;
    for (ULONG i = 0; i < S2C_MAX_USERS; i++)
    {
        DWORD pid = s.driver.UserPids[i];
        if (!pid) continue;
        wchar_t name[MAX_PATH];
        ProcessName(pid, name, MAX_PATH);
        if (!name[0] || !_wcsicmp(name, L"svchost.exe")) frameServer++;      // Frame Server (or not readable)
        else if (pid != GetCurrentProcessId()) AddName(out, len, name);
    }
    // The Frame Server's clients: Windows' current camera users, but only when this camera's Frame Server stream is not
    // just our self-view (the record is for all cameras).
    if (self) frameServer--;
    if (frameServer > 0)
        for (int i = 0; i < g_appsNowCount; i++)
            if (_wcsicmp(g_appsNow[i], L"Show2Cam.exe") != 0) AddName(out, len, g_appsNow[i]);
    if (!out[0]) wcsncpy(out, TR(L"используется"), len - 1);
    out[len - 1] = 0;
}

// ---------------------------------------------------------------------------
// Cameras: the runners follow the cameras the driver offers

static void ApplyConfig(int i)
{
    SaveConfig(g_cams[i].info.index, g_cams[i].config);
    g_cams[i].audioMissingLogged = false;
    if (g_cams[i].runner) RunnerConfigure(g_cams[i].runner, g_cams[i].config);
}

static void CloseAllPreviews();
static void ClosePreview(int i);
static void ShowSelected();
static void FillList();
static void PreviewTitle(int i);

static void StopRunners()
{
    CloseAllPreviews();
    for (int i = 0; i < g_camCount; i++)
    {
        RunnerStop(g_cams[i].runner);
        g_cams[i].runner = nullptr;
    }
}

// Looks for the cameras again; restarts the runners when they changed. Returns the number of cameras.
static int ScanCameras(bool log)
{
    static CamInfo found[S2C_MAX_CAMERAS_UI];
    int n = CamList(found, S2C_MAX_CAMERAS_UI);
    bool same = n == g_camCount;
    for (int i = 0; same && i < n; i++)
        same = found[i].index == g_cams[i].info.index && wcscmp(found[i].path, g_cams[i].info.path) == 0;
    if (same)
    {
        for (int i = 0; i < n; i++) wcscpy(g_cams[i].info.name, found[i].name);    // names may have changed
        return n;
    }
    int keepSel = g_sel >= 0 && g_sel < g_camCount ? g_cams[g_sel].info.index : 0;
    // The cameras that stay keep running (their source, preview and self-view are not interrupted); usually only the
    // last ones come or go (the number of cameras changed).
    int keep = 0;
    while (keep < n && keep < g_camCount && found[keep].index == g_cams[keep].info.index &&
           wcscmp(found[keep].path, g_cams[keep].info.path) == 0)
    {
        wcscpy(g_cams[keep].info.name, found[keep].name);
        keep++;
    }
    for (int i = keep; i < g_camCount; i++)
    {
        ClosePreview(i);
        RunnerStop(g_cams[i].runner);
        g_cams[i].runner = nullptr;
    }
    g_camCount = n;
    g_sel = -1;
    for (int i = 0; i < keep; i++)
        if (g_cams[i].info.index == keepSel) g_sel = i;
    for (int i = keep; i < n; i++)
    {
        Cam& c = g_cams[i];
        c.info = found[i];
        LoadConfig(c.info.index, &c.config);
        c.audioMissingLogged = false;
        ZeroMemory(&c.status, sizeof(c.status));
        c.runner = RunnerStart(i, c.info.path, c.config, g_wnd, WM_APP_CAMEVENT);
        if (c.info.index == keepSel) g_sel = i;
    }
    if (g_sel < 0 && n > 0) g_sel = 0;
    if (log) AppLog(L"cameras: %d", n);
    FillList();
    ShowSelected();
    return n;
}

// ---------------------------------------------------------------------------
// Camera list (list view: name, source, format, in use)

static void ListSetText(int row, int col, const wchar_t* text)
{
    wchar_t cur[300] = L"";
    ListView_GetItemText(Ctl(IDC_LIST), row, col, cur, 300);
    if (wcscmp(cur, text) != 0) ListView_SetItemText(Ctl(IDC_LIST), row, col, (LPWSTR)text);
}

static void UpdateListRow(int i)
{
    Cam& c = g_cams[i];
    wchar_t t[300];
    ListSetText(i, 0, c.info.name);
    const wchar_t* what = SourceName(c.config.kind);
    if (c.config.kind == SourceText) _snwprintf(t, 300, L"%ls: %ls", what, c.config.text);
    else if (c.config.kind == SourceStream) _snwprintf(t, 300, L"%ls: %ls", what, c.config.url);
    else if (c.status.current[0]) _snwprintf(t, 300, L"%ls: %ls", what, FileName(c.status.current));
    else _snwprintf(t, 300, L"%ls", what);
    t[299] = 0;
    if (c.config.paused) { wcsncat(t, L" — ", 299 - wcslen(t)); wcsncat(t, TR(L"пауза"), 299 - wcslen(t)); }
    if (c.config.selfView) { wcsncat(t, L" · ", 299 - wcslen(t)); wcsncat(t, TR(L"самотрансляция"), 299 - wcslen(t)); }
    ListSetText(i, 1, t);
    if (c.status.deviceOpen) FormatText(t, 300, c.status.driver.Width, c.status.driver.Height, c.status.driver.Fps);
    else wcscpy(t, L"—");
    ListSetText(i, 2, t);
    wchar_t users[300], state[320];
    UsersText(c.status, users, 300);
    if (!c.status.deviceOpen) wcscpy(state, TR(L"нет связи"));
    else if (users[0]) _snwprintf(state, 320, L"● %ls", users);
    else wcscpy(state, TR(L"○ не используется"));
    state[319] = 0;
    ListSetText(i, 3, state);
}

static void FillList()
{
    HWND list = Ctl(IDC_LIST);
    g_updating = true;
    ListView_DeleteAllItems(list);
    for (int i = 0; i < g_camCount; i++)
    {
        LVITEMW it = {};
        it.mask = LVIF_TEXT;
        it.iItem = i;
        it.pszText = g_cams[i].info.name;
        ListView_InsertItem(list, &it);
        UpdateListRow(i);
    }
    if (g_sel >= 0) ListView_SetItemState(list, g_sel, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    g_updating = false;
}

// ---------------------------------------------------------------------------
// Selected camera

static const ULONG kSizes[][2] = { { 640, 360 }, { 640, 480 }, { 800, 600 }, { 960, 540 }, { 1024, 768 }, { 1280, 720 },
                                   { 1280, 960 }, { 1600, 900 }, { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
static const ULONG kRates[] = { 5, 10, 15, 20, 24, 25, 30, 50, 60 };

static LPARAM SizeData(ULONG w, ULONG h) { return (LPARAM)((w << 16) | h); }

static void FolderOf(const CamConfig& c, wchar_t* folder)
{
    const wchar_t* f = c.kind == SourceImages ? c.imageFolder : c.videoFolder;
    if (f[0]) wcscpy(folder, f);
    else DefaultMediaFolder(c.kind == SourceImages ? MediaImages : MediaVideo, folder);
}

static void FillAudioCombo(const CamConfig& c)
{
    g_audioDevCount = ListRenderDevices(g_audioDevs, 64);
    HWND box = Ctl(IDC_AUDIO);
    SendMessageW(box, CB_RESETCONTENT, 0, 0);
    // Speak2Mic Speaker (by its adapter: also when renamed)
    RenderDevice s2m;
    wchar_t t[400];
    if (FindSpeak2MicSpeaker(&s2m)) _snwprintf(t, 400, TR(L"Speak2Mic: %ls (по умолчанию)"), s2m.name);
    else _snwprintf(t, 400, L"%ls", TR(L"Speak2Mic Speaker (не найден: Speak2Mic не установлен?)"));
    t[399] = 0;
    ComboAdd(IDC_AUDIO, t, -1);
    ComboAdd(IDC_AUDIO, TR(L"Без звука"), -2);
    for (int i = 0; i < g_audioDevCount; i++)
        if (_wcsicmp(g_audioDevs[i].adapter, L"Speak2Mic") != 0) ComboAdd(IDC_AUDIO, g_audioDevs[i].name, i);
    if (c.audioMode == AudioOff) ComboSelectData(IDC_AUDIO, -2);
    else if (c.audioMode == AudioDevice)
    {
        bool found = false;
        for (int i = 0; i < g_audioDevCount && !found; i++)
            if (_wcsicmp(g_audioDevs[i].id, c.audioDevice) == 0) found = ComboSelectData(IDC_AUDIO, i);
        if (!found)
        {
            ComboAdd(IDC_AUDIO, TR(L"Выбранное устройство (сейчас не подключено)"), -3);
            ComboSelectData(IDC_AUDIO, -3);
        }
    }
    else ComboSelectData(IDC_AUDIO, -1);
}

// The parameter row and the sound row depend on the source.
static void LayoutParamRow(int kind)
{
    bool folder = kind == SourceImages || kind == SourceVideo;
    ShowWindow(Ctl(IDC_BROWSE), folder ? SW_SHOW : SW_HIDE);
    ShowWindow(Ctl(IDC_OPENFOLDER), folder ? SW_SHOW : SW_HIDE);
    SetWindowPos(Ctl(IDC_PARAM), nullptr, S(130), S(360), S(folder ? 282 : 442), S(23), SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(Ctl(IDC_L_AUDIO), kind == SourceVideo ? SW_SHOW : SW_HIDE);
    ShowWindow(Ctl(IDC_AUDIO), kind == SourceVideo ? SW_SHOW : SW_HIDE);
    SetText(IDC_L_PARAM, kind == SourceText ? TR(L"Текст:") : (kind == SourceStream ? TR(L"Адрес:") : TR(L"Папка:")));
    SendMessageW(Ctl(IDC_PARAM), EM_SETCUEBANNER, TRUE,
                 (LPARAM)(kind == SourceStream ? L"http://10.0.28.101:8001" : L""));
}

static void UpdatePlayButton()
{
    bool ok = g_sel >= 0;
    SetText(IDC_PLAY, ok && g_cams[g_sel].config.paused ? TR(L"Играть") : TR(L"Пауза"));
    EnableWindow(Ctl(IDC_PLAY), ok);
    SetText(IDC_TEST, ok && g_cams[g_sel].preview ? TR(L"Закрыть проверку") : TR(L"Проверка"));
    EnableWindow(Ctl(IDC_TEST), ok);
}

static void UpdateRenameButton()
{
    if (g_sel < 0)
    {
        EnableWindow(Ctl(IDC_RENAME), FALSE);
        return;
    }
    wchar_t want[S2C_NAME_CHARS];
    GetWindowTextW(Ctl(IDC_NAME), want, S2C_NAME_CHARS);
    EnableWindow(Ctl(IDC_RENAME), wcscmp(want, g_cams[g_sel].info.name) != 0);
}

// The selected camera's status line.
static void ShowCamStatus()
{
    if (g_sel < 0)
    {
        SetText(IDC_CAM_STATUS, g_camCount ? L"" : TR(L"Камеры Show2Cam не найдены: драйвер не установлен или перезапускается."));
        return;
    }
    const CamRunStatus& s = g_cams[g_sel].status;
    wchar_t line1[400], line2[400] = L"", fmt[80];
    if (!s.deviceOpen) wcscpy(line1, TR(L"Нет связи с камерой (драйвер перезапускается?)."));
    else
    {
        FormatText(fmt, 80, s.driver.Width, s.driver.Height, s.driver.Fps);
        wchar_t users[300], inUse[340];
        UsersText(s, users, 300);
        if (users[0]) _snwprintf(inUse, 340, TR(L"используется: %ls"), users);
        else wcscpy(inUse, TR(L"не используется"));
        inUse[339] = 0;
        _snwprintf(line1, 400, L"%ls: %ls · %ls", TR(L"Камера"), fmt, inUse);
        if (s.formatWaiting)
        {
            wchar_t want[80];
            FormatText(want, 80, s.wantW, s.wantH, s.wantFps);
            wcsncat(line1, L" · ", 399 - wcslen(line1));
            _snwprintf(line1 + wcslen(line1), 400 - wcslen(line1), TR(L"станет %ls, когда камеру закроют"), want);
        }
    }
    line1[399] = 0;
    switch (s.state)
    {
    case StateNoSignal: _snwprintf(line2, 400, TR(L"Нет сигнала: %ls"), s.detail[0] ? s.detail : s.current); break;
    case StateNoFiles:  wcscpy(line2, TR(L"В папке нет подходящих файлов.")); break;
    case StateError:    _snwprintf(line2, 400, TR(L"Ошибка: %ls"), s.detail); break;
    case StateOk:
        if (s.sourceW && g_cams[g_sel].config.kind != SourceText)
        {
            _snwprintf(line2, 400, TR(L"Источник: %lu×%lu"), s.sourceW, s.sourceH);
            if (s.current[0] && g_cams[g_sel].config.kind != SourceStream)
            {
                wcsncat(line2, L" · ", 399 - wcslen(line2));
                wcsncat(line2, FileName(s.current), 399 - wcslen(line2));
            }
        }
        if (g_cams[g_sel].config.kind == SourceVideo)
        {
            wcsncat(line2, L" · ", 399 - wcslen(line2));
            if (s.audio[0])
            {
                wchar_t a[300];
                _snwprintf(a, 300, TR(L"звук: %ls"), s.audio);
                a[299] = 0;
                wcsncat(line2, a, 399 - wcslen(line2));
            }
            else wcsncat(line2, TR(L"без звука"), 399 - wcslen(line2));
        }
        break;
    default: break;
    }
    if (s.selfView)
    {
        wchar_t sv[120];
        if (s.self.open) _snwprintf(sv, 120, TR(L"самотрансляция: %lu кадр/с"), s.self.fps);
        else if (FAILED(s.self.error)) _snwprintf(sv, 120, TR(L"самотрансляция: ошибка 0x%08lX"), (unsigned long)s.self.error);
        else _snwprintf(sv, 120, L"%ls", TR(L"самотрансляция"));
        sv[119] = 0;
        if (line2[0]) wcsncat(line2, L" · ", 399 - wcslen(line2));
        wcsncat(line2, sv, 399 - wcslen(line2));
    }
    line2[399] = 0;
    wchar_t all[820];
    _snwprintf(all, 820, line2[0] ? L"%ls\n%ls" : L"%ls", line1, line2);
    all[819] = 0;
    SetText(IDC_CAM_STATUS, all);
}

static void ShowSelected()
{
    bool ok = g_sel >= 0 && g_sel < g_camCount;
    const int ids[] = { IDC_NAME, IDC_SOURCE, IDC_PARAM, IDC_BROWSE, IDC_OPENFOLDER, IDC_AUDIO, IDC_RES, IDC_FPS, IDC_SELFVIEW };
    for (int id : ids) EnableWindow(Ctl(id), ok);
    wchar_t title[200];
    if (ok) _snwprintf(title, 200, TR(L"Камера %d: %ls"), g_cams[g_sel].info.index + 1, g_cams[g_sel].info.name);
    else wcscpy(title, TR(L"Камера"));
    title[199] = 0;
    SetText(IDC_GROUP2, title);
    if (!ok)
    {
        UpdatePlayButton();
        UpdateRenameButton();
        ShowCamStatus();
        return;
    }
    const CamConfig& c = g_cams[g_sel].config;
    g_updating = true;
    SetWindowTextW(Ctl(IDC_NAME), g_cams[g_sel].info.name);
    SendMessageW(Ctl(IDC_SELFVIEW), BM_SETCHECK, c.selfView ? BST_CHECKED : BST_UNCHECKED, 0);
    ComboSelectData(IDC_SOURCE, c.kind);
    LayoutParamRow(c.kind);
    wchar_t param[MAX_PATH];
    if (c.kind == SourceText) wcscpy(param, c.text);
    else if (c.kind == SourceStream) wcscpy(param, c.url);
    else FolderOf(c, param);
    SetWindowTextW(Ctl(IDC_PARAM), param);
    if (c.kind == SourceVideo) FillAudioCombo(c);
    // size: a size that is not in the list (set elsewhere) is added
    if (c.width && c.height && !ComboSelectData(IDC_RES, SizeData(c.width, c.height)))
    {
        wchar_t t[40];
        _snwprintf(t, 40, L"%lu×%lu", c.width, c.height);
        ComboAdd(IDC_RES, t, SizeData(c.width, c.height));
        ComboSelectData(IDC_RES, SizeData(c.width, c.height));
    }
    else if (!c.width || !c.height) ComboSelectData(IDC_RES, 0);
    if (!ComboSelectData(IDC_FPS, (LPARAM)c.fps))
    {
        wchar_t t[20];
        _snwprintf(t, 20, L"%lu", c.fps);
        ComboAdd(IDC_FPS, t, (LPARAM)c.fps);
        ComboSelectData(IDC_FPS, (LPARAM)c.fps);
    }
    g_updating = false;
    UpdatePlayButton();
    UpdateRenameButton();
    ShowCamStatus();
}

// The parameter box was edited: applied after a short pause in typing.
static void ApplyParam()
{
    if (g_sel < 0) return;
    CamConfig& c = g_cams[g_sel].config;
    wchar_t text[MAX_PATH];
    GetWindowTextW(Ctl(IDC_PARAM), text, MAX_PATH);
    if (c.kind == SourceText)
    {
        if (!wcscmp(c.text, text)) return;
        wcsncpy(c.text, text, 255);
        c.text[255] = 0;
    }
    else if (c.kind == SourceStream)
    {
        // trimmed
        wchar_t* b = text;
        while (*b == L' ') b++;
        size_t n = wcslen(b);
        while (n && b[n - 1] == L' ') b[--n] = 0;
        if (!wcscmp(c.url, b)) return;
        wcsncpy(c.url, b, 511);
        c.url[511] = 0;
        AddEventF(TR(L"Камера «%ls»: MJPEG-поток %ls."), g_cams[g_sel].info.name, c.url);
    }
    else
    {
        wchar_t def[MAX_PATH];
        DefaultMediaFolder(c.kind == SourceImages ? MediaImages : MediaVideo, def);
        wchar_t* folder = c.kind == SourceImages ? c.imageFolder : c.videoFolder;
        const wchar_t* want = _wcsicmp(text, def) == 0 ? L"" : text;
        if (!wcscmp(folder, want)) return;
        wcscpy(folder, want);
    }
    ApplyConfig(g_sel);
    UpdateListRow(g_sel);
}

static void OnSourceChanged()
{
    if (g_sel < 0) return;
    int kind = (int)ComboData(IDC_SOURCE);
    if (kind < 0 || kind == g_cams[g_sel].config.kind) return;
    KillTimer(g_wnd, TIMER_PARAM);
    ApplyParam();                                   // what was typed for the old source
    g_cams[g_sel].config.kind = kind;
    ApplyConfig(g_sel);
    AddEventF(TR(L"Камера «%ls»: источник — %ls."), g_cams[g_sel].info.name, SourceName(kind));
    ShowSelected();
    UpdateListRow(g_sel);
}

static void OnAudioChanged()
{
    if (g_sel < 0) return;
    CamConfig& c = g_cams[g_sel].config;
    LPARAM d = ComboData(IDC_AUDIO);
    if (d == -3) return;
    if (d == -1) c.audioMode = AudioSpeak2Mic;
    else if (d == -2) c.audioMode = AudioOff;
    else if (d >= 0 && d < g_audioDevCount)
    {
        c.audioMode = AudioDevice;
        wcscpy(c.audioDevice, g_audioDevs[d].id);
    }
    ApplyConfig(g_sel);
}

static void OnFormatChanged()
{
    if (g_sel < 0) return;
    CamConfig& c = g_cams[g_sel].config;
    LPARAM size = ComboData(IDC_RES), fps = ComboData(IDC_FPS);
    c.width = size > 0 ? (ULONG)(size >> 16) : 0;
    c.height = size > 0 ? (ULONG)(size & 0xFFFF) : 0;
    c.fps = fps > 0 ? (ULONG)fps : 0;
    ApplyConfig(g_sel);
}

// Self-view: the panel uses the camera itself, like any webcam program (see selfview.h).
static void OnSelfView()
{
    if (g_sel < 0) return;
    CamConfig& c = g_cams[g_sel].config;
    c.selfView = SendMessageW(Ctl(IDC_SELFVIEW), BM_GETCHECK, 0, 0) == BST_CHECKED;
    ApplyConfig(g_sel);
    if (!c.selfView) AddEventF(TR(L"Камера «%ls»: самотрансляция выключена."), g_cams[g_sel].info.name);
    UpdateListRow(g_sel);
}

static void OnPlay()
{
    if (g_sel < 0) return;
    CamConfig& c = g_cams[g_sel].config;
    c.paused = !c.paused;
    ApplyConfig(g_sel);
    AddEventF(c.paused ? TR(L"Камера «%ls»: пауза.") : TR(L"Камера «%ls»: воспроизведение."), g_cams[g_sel].info.name);
    UpdatePlayButton();
    UpdateListRow(g_sel);
}

static void OnRename()
{
    if (g_sel < 0) return;
    wchar_t want[S2C_NAME_CHARS];
    GetWindowTextW(Ctl(IDC_NAME), want, S2C_NAME_CHARS);
    wchar_t* b = want;
    while (*b == L' ') b++;
    size_t n = wcslen(b);
    while (n && b[n - 1] == L' ') b[--n] = 0;
    // The driver puts the name on the camera's interfaces and keeps it (no administrator rights needed).
    HANDLE h = CamOpen(g_cams[g_sel].info.path);
    bool ok = h != INVALID_HANDLE_VALUE && CamSetName(h, b);
    DWORD err = ok ? 0 : GetLastError();
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    AppLog(L"rename camera %d to \"%ls\": %ls (%lu)", g_cams[g_sel].info.index + 1, b, ok ? L"ok" : L"FAILED", err);
    wchar_t old[S2C_NAME_CHARS];
    wcscpy(old, g_cams[g_sel].info.name);
    ScanCameras(false);                              // reads the names back
    if (ok) AddEventF(TR(L"Камера «%ls» переименована в «%ls»."), old, g_sel >= 0 ? g_cams[g_sel].info.name : b);
    else AddEventF(TR(L"Не удалось переименовать камеру (код %lu)."), err);
    for (int i = 0; i < g_camCount; i++)
    {
        UpdateListRow(i);
        PreviewTitle(i);
    }
    ShowSelected();
}

static void OnBrowse()
{
    if (g_sel < 0) return;
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    DWORD opts = 0;
    dlg->GetOptions(&opts);
    dlg->SetOptions(opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    wchar_t cur[MAX_PATH];
    FolderOf(g_cams[g_sel].config, cur);
    IShellItem* start = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(cur, nullptr, IID_PPV_ARGS(&start))))
    {
        dlg->SetFolder(start);
        start->Release();
    }
    if (SUCCEEDED(dlg->Show(g_wnd)))
    {
        IShellItem* item = nullptr;
        PWSTR path = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
        {
            SetWindowTextW(Ctl(IDC_PARAM), path);       // EN_CHANGE -> applied
            KillTimer(g_wnd, TIMER_PARAM);
            ApplyParam();
            AddEventF(TR(L"Камера «%ls»: папка %ls."), g_cams[g_sel].info.name, path);
            CoTaskMemFree(path);
        }
        if (item) item->Release();
    }
    dlg->Release();
}

static void OnOpenFolder()
{
    if (g_sel < 0) return;
    wchar_t folder[MAX_PATH];
    FolderOf(g_cams[g_sel].config, folder);
    ShellExecuteW(g_wnd, L"open", folder, nullptr, nullptr, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
// Preview window ("Check"): the frames the selected camera sends, live

static void PreviewTitle(int i)
{
    if (i < 0 || i >= g_camCount || !g_cams[i].preview) return;
    wchar_t t[200];
    _snwprintf(t, 200, TR(L"Проверка — %ls"), g_cams[i].info.name);
    t[199] = 0;
    SetWindowTextW(g_cams[i].preview, t);
}

static void ClosePreview(int i)
{
    if (i < 0 || i >= g_camCount) return;
    Cam& c = g_cams[i];
    if (c.runner) RunnerSetPreview(c.runner, false);
    c.previewPic.Free();
    if (c.preview)
    {
        HWND w = c.preview;
        c.preview = nullptr;
        DestroyWindow(w);
    }
    if (g_wnd) UpdatePlayButton();
}

static void CloseAllPreviews()
{
    for (int i = 0; i < g_camCount; i++) ClosePreview(i);
}

static int OpenPreviews()
{
    int n = 0;
    for (int i = 0; i < g_camCount; i++)
        if (g_cams[i].preview) n++;
    return n;
}

static LRESULT CALLBACK PreviewProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    int i = (int)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    Picture* pic = i >= 0 && i < g_camCount && g_cams[i].preview == hwnd ? &g_cams[i].previewPic : nullptr;
    switch (msg)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        int W = rc.right, H = rc.bottom;
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        if (pic && pic->px && W > 0 && H > 0)
        {
            int pw = pic->w, ph = pic->h, w = W, h = H;
            if ((LONGLONG)pw * H > (LONGLONG)ph * W) h = (int)((LONGLONG)ph * W / pw);
            else w = (int)((LONGLONG)pw * H / ph);
            int x = (W - w) / 2, y = (H - h) / 2;
            BITMAPINFO bi = {};
            bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
            bi.bmiHeader.biWidth = pw;
            bi.bmiHeader.biHeight = -ph;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchDIBits(dc, x, y, w, h, 0, 0, pw, ph, pic->px, &bi, DIB_RGB_COLORS, SRCCOPY);
            RECT bars[4] = { { 0, 0, W, y }, { 0, y + h, W, H }, { 0, y, x, y + h }, { x + w, y, W, y + h } };
            for (RECT& b : bars) FillRect(dc, &b, black);
        }
        else
        {
            FillRect(dc, &rc, black);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(200, 200, 200));
            HGDIOBJ old = SelectObject(dc, g_font);
            DrawTextW(dc, TR(L"Ожидание кадра…"), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        if (pic) ClosePreview(i);
        else DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// "Check": opens (or closes) the preview window of the selected camera; the other cameras' windows stay open.
static void OnTest()
{
    if (g_sel < 0) return;
    Cam& c = g_cams[g_sel];
    if (c.preview)
    {
        ClosePreview(g_sel);
        return;
    }
    c.previewPic.Free();
    RECT r = { 0, 0, S(640), S(360) };
    DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRectExForDpi(&r, style, FALSE, 0, g_dpi);
    RECT main;
    GetWindowRect(g_wnd, &main);
    int step = S(28) * OpenPreviews();          // cascade the windows
    c.preview = CreateWindowExW(0, L"S2cPreview", L"", style, main.right + S(8) + step, main.top + step, r.right - r.left,
                                r.bottom - r.top, g_wnd, nullptr, g_inst, nullptr);
    if (!c.preview) return;
    SetWindowLongPtrW(c.preview, GWLP_USERDATA, (LONG_PTR)g_sel);
    PreviewTitle(g_sel);
    ShowWindow(c.preview, SW_SHOWNOACTIVATE);
    RunnerSetPreview(c.runner, true);
    AppLog(L"preview of camera %d (%d open)", c.info.index + 1, OpenPreviews());
    UpdatePlayButton();
}

// ---------------------------------------------------------------------------
// Number of cameras (driver setting CameraCount + device restart: administrator rights)

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

// 0 ok, 2 driver not installed, 3 device not found, 4 restart failed, 5 reboot needed
static int ApplyCount(int count)
{
    if (!S2cSetParam(L"CameraCount", (DWORD)count)) return 2;
    bool found = false, reboot = false;
    bool ok = S2cRestartDevice(&found, &reboot);
    int rc = !found ? 3 : (!ok ? 4 : (reboot ? 5 : 0));
    AppLog(L"camera count %d -> result %d", count, rc);
    return rc;
}

static int ApplyCountElevated(int count)
{
    wchar_t exe[MAX_PATH], args[64];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    _snwprintf(args, 64, L"--set-count %d", count);
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = g_wnd;
    sei.lpVerb = L"runas";
    sei.lpFile = exe;
    sei.lpParameters = args;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return -1;
    WaitForSingleObject(sei.hProcess, 60000);
    DWORD code = 4;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return (int)code;
}

static void UpdateCountButton()
{
    int want = (int)ComboData(IDC_COUNT);
    EnableWindow(Ctl(IDC_COUNT_APPLY), want > 0 && want != (int)S2cGetParam(L"CameraCount", 1));
}

static void OnCountApply()
{
    int want = (int)ComboData(IDC_COUNT);
    if (want < 1) return;
    // The driver changes the number of cameras at once (no device restart, no administrator rights): the cameras that
    // stay keep running.
    HANDLE h = g_camCount ? CamOpen(g_cams[0].info.path) : INVALID_HANDLE_VALUE;
    ULONG count = (ULONG)want;
    bool ok = h != INVALID_HANDLE_VALUE && CamSetCount(h, count);
    DWORD err = ok ? 0 : GetLastError();
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    AppLog(L"camera count %d through the driver: %ls (%lu)", want, ok ? L"ok" : L"not possible", err);
    if (ok)
    {
        AddEventF(TR(L"Количество камер: %d."), want);
        SetTimer(g_wnd, TIMER_RESCAN, 800, nullptr);
        return;
    }
    // An older driver: setting + device restart (administrator rights).
    // The panel's own camera handles would keep the device from restarting.
    StopRunners();
    g_camCount = 0;
    g_sel = -1;
    FillList();
    ShowSelected();
    SetText(IDC_CAM_STATUS, TR(L"Перезапуск устройства Show2Cam…"));
    UpdateWindow(g_wnd);
    int rc = g_elevated ? ApplyCount(want) : ApplyCountElevated(want);
    switch (rc)
    {
    case 0:  AddEventF(TR(L"Количество камер: %d."), want); break;
    case -1: AddEvent(TR(L"Нужны права администратора: запрос был отклонён.")); break;
    case 5:  AddEvent(TR(L"Количество камер сохранено. Windows не смогла перезапустить устройство на ходу (камеру держит программа?) — перезагрузите компьютер.")); break;
    default: AddEventF(TR(L"Не удалось изменить количество камер (код %d)."), rc); break;
    }
    SetTimer(g_wnd, TIMER_RESCAN, 1500, nullptr);
    UpdateCountButton();
}

// ---------------------------------------------------------------------------
// Reset everything: every camera back to its text "Camera N", sizes "as the source", default names

static void OnResetAll()
{
    if (MessageBoxW(g_wnd, TR(L"Сбросить настройки всех камер Show2Cam?\n\nИсточник — текст «Camera N», разрешение — как у источника, имена — по умолчанию."),
                    L"Show2Cam", MB_YESNO | MB_ICONQUESTION) != IDYES)
        return;
    AppLog(L"reset all settings requested");
    for (int i = 0; i < g_camCount; i++)
    {
        HANDLE h = CamOpen(g_cams[i].info.path);
        if (h != INVALID_HANDLE_VALUE)
        {
            CamSetName(h, L"");
            CloseHandle(h);
        }
        DefaultConfig(g_cams[i].info.index, &g_cams[i].config);
        ApplyConfig(i);
    }
    ScanCameras(false);
    for (int i = 0; i < g_camCount; i++)
    {
        UpdateListRow(i);
        PreviewTitle(i);
    }
    ShowSelected();
    AddEvent(TR(L"Все настройки сброшены к стандартным."));
}

// ---------------------------------------------------------------------------
// Camera events

static void OnCamEvent(int i, int ev)
{
    if (i < 0 || i >= g_camCount) return;
    Cam& c = g_cams[i];
    if (ev == EvPreviewFrame)
    {
        if (c.preview && RunnerGetPreviewFrame(c.runner, &c.previewPic)) InvalidateRect(c.preview, nullptr, FALSE);
        return;
    }
    RunnerGetStatus(c.runner, &c.status);
    const wchar_t* name = c.info.name;
    wchar_t fmt[80];
    switch (ev)
    {
    case EvNoSignal:
        AddEventF(TR(L"Камера «%ls»: нет сигнала от %ls — переподключение…"), name, c.config.url);
        break;
    case EvSignal:
        AddEventF(TR(L"Камера «%ls»: сигнал есть."), name);
        break;
    case EvNoFiles:
    {
        wchar_t folder[MAX_PATH];
        FolderOf(c.config, folder);
        AddEventF(TR(L"Камера «%ls»: в папке нет подходящих файлов (%ls)."), name, folder);
        break;
    }
    case EvVideoFile:
        AddEventF(TR(L"Камера «%ls»: видео «%ls»."), name, FileName(c.status.current));
        break;
    case EvVideoError:
        AddEventF(TR(L"Камера «%ls»: не удалось воспроизвести видео (%ls)."), name, c.status.detail);
        break;
    case EvFormatChanged:
        FormatText(fmt, 80, c.status.driver.Width, c.status.driver.Height, c.status.driver.Fps);
        AddEventF(TR(L"Камера «%ls»: формат %ls."), name, fmt);
        break;
    case EvFormatWaiting:
        FormatText(fmt, 80, c.status.wantW, c.status.wantH, c.status.wantFps);
        AddEventF(TR(L"Камера «%ls»: формат %ls будет установлен, когда камеру закроют все программы."), name, fmt);
        break;
    case EvSelfViewOn:
        AddEventF(TR(L"Камера «%ls»: самотрансляция включена — Windows видит камеру используемой."), name);
        break;
    case EvSelfViewError:
        if (c.status.self.error == E_ACCESSDENIED)
            AddEventF(TR(L"Камера «%ls»: самотрансляции запрещён доступ к камере — разрешите классическим приложениям доступ к камере в параметрах конфиденциальности Windows."), name);
        else
            AddEventF(TR(L"Камера «%ls»: самотрансляция не может открыть камеру (0x%08lX)."), name, (unsigned long)c.status.self.error);
        break;
    case EvAudioMissing:
        if (c.audioMissingLogged) break;
        c.audioMissingLogged = true;
        if (c.config.audioMode == AudioSpeak2Mic)
            AddEventF(TR(L"Камера «%ls»: Speak2Mic Speaker не найден — видео без звука."), name);
        else
            AddEventF(TR(L"Камера «%ls»: выбранное устройство звука не найдено — видео без звука."), name);
        break;
    }
    UpdateListRow(i);
    if (i == g_sel) ShowCamStatus();
}

// ---------------------------------------------------------------------------
// Layout

static void Place(int id, int x, int y, int w, int h)
{
    SetWindowPos(Ctl(id), nullptr, S(x), S(y), S(w), S(h), SWP_NOZORDER | SWP_NOACTIVATE);
}

static void ApplyToolIcons()
{
    const int ids[2] = { IDC_RESET_ALL, IDC_CLEARLOG };
    int size = MulDiv(20, (int)g_dpi, 96);
    for (int i = 0; i < 2; i++)
    {
        HICON old = g_toolIcons[i];
        g_toolIcons[i] = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(10 + i), IMAGE_ICON, size, size, 0);
        SendMessageW(Ctl(ids[i]), BM_SETIMAGE, IMAGE_ICON, (LPARAM)g_toolIcons[i]);
        if (old) DestroyIcon(old);
    }
}

static void ApplyFonts()
{
    if (g_font) DeleteObject(g_font);
    if (g_fontBold) DeleteObject(g_fontBold);
    g_font = CreateFontW(-MulDiv(9, (int)g_dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontBold = CreateFontW(-MulDiv(9, (int)g_dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    for (HWND c = GetWindow(g_wnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) SendMessageW(c, WM_SETFONT, (WPARAM)g_font, TRUE);
}

static void ListColumns()
{
    HWND list = Ctl(IDC_LIST);
    const int widths[4] = { 128, 160, 128, 124 };
    for (int i = 0; i < 4; i++) ListView_SetColumnWidth(list, i, S(widths[i]));
}

static void Layout()
{
    Place(IDC_RESET_ALL, 12, 7, 32, 28);
    Place(IDC_L_LANG, 300, 12, 120, 20);   Place(IDC_LANG, 428, 8, 160, 400);

    Place(IDC_GROUP1, 12, 42, 576, 222);
    Place(IDC_LIST, 24, 64, 552, 154);
    Place(IDC_L_COUNT, 24, 230, 150, 20);  Place(IDC_COUNT, 180, 226, 70, 300);
    Place(IDC_COUNT_APPLY, 260, 225, 140, 27);
    ListColumns();

    Place(IDC_GROUP2, 12, 272, 576, 262);
    Place(IDC_L_NAME, 24, 296, 100, 20);   Place(IDC_NAME, 130, 292, 282, 23);   Place(IDC_RENAME, 420, 291, 152, 27);
    Place(IDC_L_SOURCE, 24, 330, 100, 20); Place(IDC_SOURCE, 130, 326, 282, 300);
    Place(IDC_L_PARAM, 24, 364, 100, 20);  Place(IDC_PARAM, 130, 360, 282, 23);
    Place(IDC_BROWSE, 420, 359, 74, 27);   Place(IDC_OPENFOLDER, 498, 359, 74, 27);
    Place(IDC_L_AUDIO, 24, 398, 100, 20);  Place(IDC_AUDIO, 130, 394, 442, 300);
    Place(IDC_L_RES, 24, 432, 100, 20);    Place(IDC_RES, 130, 428, 170, 300);
    Place(IDC_L_FPS, 304, 432, 140, 20);   Place(IDC_FPS, 450, 428, 122, 300);
    Place(IDC_CAM_STATUS, 24, 462, 548, 34);
    Place(IDC_TEST, 24, 498, 160, 28);     Place(IDC_PLAY, 192, 498, 120, 28);
    Place(IDC_SELFVIEW, 330, 500, 242, 24);
    if (g_sel >= 0) LayoutParamRow(g_cams[g_sel].config.kind);

    Place(IDC_EVENTS, 24, 544, 512, 300);
    Place(IDC_CLEARLOG, 542, 542, 32, 26);
    Place(IDC_AUTOSTART, 24, 576, 548, 22);
    if (g_eventsWidest) FitEventList(nullptr);
}

static HWND Create(const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD exStyle = 0)
{
    HWND h = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, g_wnd, (HMENU)(INT_PTR)id, g_inst, nullptr);
    SendMessageW(h, WM_SETFONT, (WPARAM)g_font, FALSE);
    return h;
}

static void AddTip(HWND tip, int id, const wchar_t* text)
{
    TOOLINFOW ti = {};
    ti.cbSize = sizeof(ti);
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = g_wnd;
    ti.uId = (UINT_PTR)Ctl(id);
    ti.lpszText = (LPWSTR)text;
    SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
}

static void CreateControls()
{
    Create(L"BUTTON", TR(L"Сбросить все настройки"), BS_PUSHBUTTON | BS_ICON | WS_TABSTOP, IDC_RESET_ALL);
    Create(L"STATIC", TR(L"Язык:"), SS_RIGHT, IDC_L_LANG);
    Create(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IDC_LANG);
    LangFillCombo(Ctl(IDC_LANG));

    Create(L"BUTTON", TR(L"Камеры"), BS_GROUPBOX, IDC_GROUP1);
    HWND list = Create(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_TABSTOP, IDC_LIST,
                       WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    const wchar_t* cols[4] = { TR(L"Камера"), TR(L"Источник"), TR(L"Формат"), TR(L"Состояние") };
    for (int i = 0; i < 4; i++)
    {
        LVCOLUMNW col = {};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = (LPWSTR)cols[i];
        col.cx = 100;
        col.iSubItem = i;
        ListView_InsertColumn(list, i, &col);
    }
    Create(L"STATIC", TR(L"Количество камер:"), 0, IDC_L_COUNT);
    Create(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IDC_COUNT);
    for (int n = 1; n <= S2C_MAX_CAMERAS_UI; n++)
    {
        wchar_t t[8];
        _snwprintf(t, 8, L"%d", n);
        ComboAdd(IDC_COUNT, t, n);
    }
    Create(L"BUTTON", TR(L"Применить"), BS_PUSHBUTTON | WS_TABSTOP, IDC_COUNT_APPLY);

    Create(L"BUTTON", TR(L"Камера"), BS_GROUPBOX, IDC_GROUP2);
    Create(L"STATIC", TR(L"Имя:"), 0, IDC_L_NAME);
    Create(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_NAME, WS_EX_CLIENTEDGE);
    SendMessageW(Ctl(IDC_NAME), EM_LIMITTEXT, S2C_NAME_CHARS - 1, 0);
    Create(L"BUTTON", TR(L"Переименовать"), BS_PUSHBUTTON | WS_TABSTOP, IDC_RENAME);
    Create(L"STATIC", TR(L"Источник:"), 0, IDC_L_SOURCE);
    Create(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, IDC_SOURCE);
    ComboAdd(IDC_SOURCE, TR(L"Текст"), SourceText);
    ComboAdd(IDC_SOURCE, TR(L"Изображения из папки (смена каждые 5 с)"), SourceImages);
    ComboAdd(IDC_SOURCE, TR(L"Видео из папки"), SourceVideo);
    ComboAdd(IDC_SOURCE, TR(L"MJPEG-поток по сети"), SourceStream);
    Create(L"STATIC", L"", 0, IDC_L_PARAM);
    Create(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_PARAM, WS_EX_CLIENTEDGE);
    SendMessageW(Ctl(IDC_PARAM), EM_LIMITTEXT, MAX_PATH - 1, 0);
    Create(L"BUTTON", TR(L"Обзор…"), BS_PUSHBUTTON | WS_TABSTOP, IDC_BROWSE);
    Create(L"BUTTON", TR(L"Открыть"), BS_PUSHBUTTON | WS_TABSTOP, IDC_OPENFOLDER);
    Create(L"STATIC", TR(L"Звук видео:"), 0, IDC_L_AUDIO);
    Create(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IDC_AUDIO);
    Create(L"STATIC", TR(L"Разрешение:"), 0, IDC_L_RES);
    Create(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IDC_RES);
    ComboAdd(IDC_RES, TR(L"Как у источника"), 0);
    for (auto& s : kSizes)
    {
        wchar_t t[40];
        _snwprintf(t, 40, L"%lu×%lu", s[0], s[1]);
        ComboAdd(IDC_RES, t, SizeData(s[0], s[1]));
    }
    Create(L"STATIC", TR(L"Кадров в секунду:"), SS_RIGHT, IDC_L_FPS);
    Create(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IDC_FPS);
    ComboAdd(IDC_FPS, TR(L"Как у источника"), 0);
    for (ULONG r : kRates)
    {
        wchar_t t[12];
        _snwprintf(t, 12, L"%lu", r);
        ComboAdd(IDC_FPS, t, (LPARAM)r);
    }
    Create(L"STATIC", L"", 0, IDC_CAM_STATUS);
    Create(L"BUTTON", TR(L"Проверка"), BS_PUSHBUTTON | WS_TABSTOP, IDC_TEST);
    Create(L"BUTTON", TR(L"Пауза"), BS_PUSHBUTTON | WS_TABSTOP, IDC_PLAY);
    Create(L"BUTTON", TR(L"Самотрансляция"), BS_AUTOCHECKBOX | WS_TABSTOP, IDC_SELFVIEW);

    Create(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, IDC_EVENTS);
    Create(L"BUTTON", TR(L"Очистить журнал событий"), BS_PUSHBUTTON | BS_ICON | WS_TABSTOP, IDC_CLEARLOG);
    Create(L"BUTTON", TR(L"Запускать панель управления вместе с Windows (в трее)"), BS_AUTOCHECKBOX | WS_TABSTOP, IDC_AUTOSTART);

    HWND tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, g_wnd, nullptr, g_inst, nullptr);
    if (tip)
    {
        AddTip(tip, IDC_RESET_ALL, TR(L"Сбросить все настройки"));
        AddTip(tip, IDC_CLEARLOG, TR(L"Очистить журнал событий"));
        AddTip(tip, IDC_TEST, TR(L"Окно с тем, что сейчас показывает камера"));
        AddTip(tip, IDC_SELFVIEW, TR(L"Панель сама использует камеру, как обычная программа: Windows показывает камеру включённой"));
    }
    g_eventTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
                                 CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, g_wnd, nullptr, g_inst, nullptr);
    if (g_eventTip)
    {
        TOOLINFOW ti = {};
        ti.cbSize = sizeof(ti);
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = g_wnd;
        ti.uId = (UINT_PTR)Ctl(IDC_EVENTS);
        ti.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(g_eventTip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        SendMessageW(g_eventTip, TTM_SETMAXTIPWIDTH, 0, 600);
        SendMessageW(g_eventTip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
    }
    ApplyToolIcons();
}

// ---------------------------------------------------------------------------
// Tray (as Speak2Mic): "Minimize" hides the window to the notification area; the cameras keep their sources.

static bool g_inTray;
static UINT g_msgTaskbarCreated, g_msgShowPanel;

static void TrayIcon(DWORD op)
{
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = g_wnd;
    nid.uID = 1;
    if (op != NIM_DELETE)
    {
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        nid.uCallbackMessage = WM_APP_TRAY;
        nid.hIcon = (HICON)LoadImageW(g_inst, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
        _snwprintf(nid.szTip, 128, L"%ls", TR(L"Show2Cam — панель управления"));
        nid.szTip[127] = 0;
    }
    Shell_NotifyIconW(op, &nid);
    if (nid.hIcon) DestroyIcon(nid.hIcon);
}

static void ToTray()
{
    if (!g_inTray) TrayIcon(NIM_ADD);
    g_inTray = true;
    ShowWindow(g_wnd, SW_HIDE);
    for (int i = 0; i < g_camCount; i++)
        if (g_cams[i].preview) ShowWindow(g_cams[i].preview, SW_HIDE);
    AppLog(L"panel minimized to the tray");
}

static void FromTray()
{
    ShowWindow(g_wnd, SW_SHOW);
    if (IsIconic(g_wnd)) ShowWindow(g_wnd, SW_RESTORE);
    for (int i = 0; i < g_camCount; i++)
        if (g_cams[i].preview) ShowWindow(g_cams[i].preview, SW_SHOWNOACTIVATE);
    SetForegroundWindow(g_wnd);
    if (g_inTray) TrayIcon(NIM_DELETE);
    g_inTray = false;
}

static void TrayMenu()
{
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_TRAY_OPEN, TR(L"Открыть панель управления"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_TRAY_EXIT, TR(L"Выход"));
    SetMenuDefaultItem(menu, IDM_TRAY_OPEN, FALSE);
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_wnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_wnd, nullptr);
    PostMessageW(g_wnd, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

// ---------------------------------------------------------------------------
// Window procedure

static LRESULT ListCustomDraw(NMLVCUSTOMDRAW* cd)
{
    switch (cd->nmcd.dwDrawStage)
    {
    case CDDS_PREPAINT:
        return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT:
        // The selection is drawn here (light blue, normal text): the system's selection colours (blue, or grey
        // without focus) would replace the green / red of the state column.
        cd->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS);
        return CDRF_NOTIFYSUBITEMDRAW;
    case CDDS_ITEMPREPAINT | CDDS_SUBITEM:
    {
        int row = (int)cd->nmcd.dwItemSpec;
        bool selected = ListView_GetItemState(cd->nmcd.hdr.hwndFrom, row, LVIS_SELECTED) != 0;
        cd->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS);
        cd->clrTextBk = selected ? RGB(204, 228, 247) : GetSysColor(COLOR_WINDOW);
        cd->clrText = GetSysColor(COLOR_WINDOWTEXT);
        if (cd->iSubItem == 3 && row < g_camCount)
        {
            const CamRunStatus& s = g_cams[row].status;
            cd->clrText = !s.deviceOpen ? RGB(190, 60, 50) : (InUse(s) ? RGB(16, 140, 56) : RGB(120, 120, 120));
        }
        return CDRF_DODEFAULT;
    }
    }
    return CDRF_DODEFAULT;
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg && msg == g_msgTaskbarCreated)
    {
        if (g_inTray) TrayIcon(NIM_ADD);
        return 0;
    }
    if (msg && msg == g_msgShowPanel)
    {
        FromTray();
        return 0;
    }
    switch (msg)
    {
    case WM_CREATE:
    {
        g_wnd = hwnd;
        g_dpi = GetDpiForWindow(hwnd);
        CreateControls();
        LoadEvents();
        FitEventList(nullptr);
        wchar_t started[160];
        _snwprintf(started, 160, TR(L"Панель управления Show2Cam запущена, версия %ls"), L"" S2C_VER_STR);
        started[159] = 0;
        AddEvent(started);
        ApplyFonts();
        Layout();
        ComboSelectData(IDC_COUNT, (LPARAM)S2cGetParam(L"CameraCount", 1));
        UpdateCountButton();
        SendMessageW(Ctl(IDC_AUTOSTART), BM_SETCHECK, S2cAutostartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
        ScanCameras(true);
        SetTimer(hwnd, TIMER_STATUS, 500, nullptr);
#ifdef S2C_UI_TEST
        wchar_t sel[8];
        if (GetEnvironmentVariableW(L"S2C_TEST_SELECT", sel, 8) && _wtoi(sel) < g_camCount)
        {
            g_sel = _wtoi(sel);
            FillList();
            ShowSelected();
        }
        wchar_t pv[8] = L"";
        if (GetEnvironmentVariableW(L"S2C_TEST_PREVIEW", pv, 8) && !wcscmp(pv, L"all"))
        {
            int keep = g_sel;
            for (int i = 0; i < g_camCount; i++)
            {
                g_sel = i;
                OnTest();
            }
            g_sel = keep;
            UpdatePlayButton();
        }
        else if (pv[0]) PostMessageW(hwnd, WM_COMMAND, MAKEWPARAM(IDC_TEST, BN_CLICKED), 0);
#endif
        return 0;
    }

    case WM_DPICHANGED:
    {
        g_dpi = HIWORD(wp);
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        ApplyFonts();
        ApplyToolIcons();
        Layout();
        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_STATUS)
        {
            CameraAppsNow();
            for (int i = 0; i < g_camCount; i++)
            {
                RunnerGetStatus(g_cams[i].runner, &g_cams[i].status);
                UpdateListRow(i);
            }
            ShowCamStatus();
            // The cameras changed (driver installed / restarted elsewhere): look again every ~3 s.
            static int tick;
            if (++tick % 6 == 0) ScanCameras(true);
        }
        else if (wp == TIMER_PARAM)
        {
            KillTimer(hwnd, TIMER_PARAM);
            ApplyParam();
        }
        else if (wp == TIMER_RESCAN)
        {
            KillTimer(hwnd, TIMER_RESCAN);
            ScanCameras(true);
            ComboSelectData(IDC_COUNT, (LPARAM)S2cGetParam(L"CameraCount", 1));
            UpdateCountButton();
        }
        return 0;

    case WM_APP_CAMEVENT:
        OnCamEvent((int)wp, (int)lp);
        return 0;

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_MINIMIZE)
        {
            ToTray();
            return 0;
        }
        break;

    case WM_APP_TRAY:
        if (lp == WM_LBUTTONUP || lp == WM_LBUTTONDBLCLK) FromTray();
        else if (lp == WM_RBUTTONUP || lp == WM_CONTEXTMENU) TrayMenu();
        return 0;

    case WM_NOTIFY:
    {
        NMHDR* h = (NMHDR*)lp;
        if (h->code == TTN_GETDISPINFOW && h->hwndFrom == g_eventTip && h->idFrom == (UINT_PTR)Ctl(IDC_EVENTS))
        {
            EventTipText((NMTTDISPINFOW*)lp);
            return 0;
        }
        if (h->idFrom == IDC_LIST && h->code == NM_CUSTOMDRAW) return ListCustomDraw((NMLVCUSTOMDRAW*)lp);
        if (h->idFrom == IDC_LIST && h->code == LVN_ITEMCHANGED && !g_updating)
        {
            NMLISTVIEW* nm = (NMLISTVIEW*)lp;
            if ((nm->uNewState & LVIS_SELECTED) && nm->iItem >= 0 && nm->iItem != g_sel)
            {
                KillTimer(hwnd, TIMER_PARAM);
                ApplyParam();
                g_sel = nm->iItem;
                ShowSelected();
            }
        }
        break;
    }

    case WM_COMMAND:
    {
        int id = LOWORD(wp), code = HIWORD(wp);
        if (id == IDM_TRAY_OPEN) { FromTray(); return 0; }
        if (id == IDM_TRAY_EXIT) { PostMessageW(hwnd, WM_CLOSE, 0, 0); return 0; }
        if (g_updating) return 0;
        if (code == CBN_SELCHANGE)
        {
            if (id == IDC_LANG)
            {
                if (LangApplyCombo(Ctl(IDC_LANG))) PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            else if (id == IDC_SOURCE) OnSourceChanged();
            else if (id == IDC_AUDIO) OnAudioChanged();
            else if (id == IDC_RES || id == IDC_FPS) OnFormatChanged();
            else if (id == IDC_COUNT) UpdateCountButton();
        }
        else if (code == CBN_DROPDOWN && id == IDC_AUDIO && g_sel >= 0)
        {
            g_updating = true;
            FillAudioCombo(g_cams[g_sel].config);          // devices may have come and gone
            g_updating = false;
        }
        else if (code == EN_CHANGE && id == IDC_PARAM)
        {
            SetTimer(hwnd, TIMER_PARAM, g_sel >= 0 && g_cams[g_sel].config.kind == SourceText ? 400 : 1200, nullptr);
        }
        else if (code == EN_CHANGE && id == IDC_NAME) UpdateRenameButton();
        else if (code == EN_KILLFOCUS && id == IDC_PARAM)
        {
            KillTimer(hwnd, TIMER_PARAM);
            ApplyParam();
        }
        else if (code == BN_CLICKED)
        {
            if (id == IDC_RENAME) OnRename();
            else if (id == IDC_BROWSE) OnBrowse();
            else if (id == IDC_OPENFOLDER) OnOpenFolder();
            else if (id == IDC_TEST) OnTest();
            else if (id == IDC_PLAY) OnPlay();
            else if (id == IDC_SELFVIEW) OnSelfView();
            else if (id == IDC_COUNT_APPLY) OnCountApply();
            else if (id == IDC_RESET_ALL) OnResetAll();
            else if (id == IDC_CLEARLOG) ClearEvents();
            else if (id == IDC_AUTOSTART)
            {
                bool on = SendMessageW(Ctl(IDC_AUTOSTART), BM_GETCHECK, 0, 0) == BST_CHECKED;
                wchar_t exe[MAX_PATH];
                GetModuleFileNameW(nullptr, exe, MAX_PATH);
                if (S2cSetAutostart(on, exe, true))
                    AddEvent(on ? TR(L"Автозагрузка панели управления включена.") : TR(L"Автозагрузка панели управления выключена."));
                SendMessageW(Ctl(IDC_AUTOSTART), BM_SETCHECK, S2cAutostartEnabled() ? BST_CHECKED : BST_UNCHECKED, 0);
            }
        }
        return 0;
    }

    case WM_CLOSE:
        if (g_inTray) TrayIcon(NIM_DELETE);
        g_inTray = false;
        KillTimer(hwnd, TIMER_STATUS);
        KillTimer(hwnd, TIMER_PARAM);
        ApplyParam();
        StopRunners();                       // the cameras go back to the test pattern
        AppLog(L"panel closed");
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------

static HANDLE g_instanceMutex;
static bool   g_startInTray;

static bool AcquireSingleInstance(bool waitForPrevious)
{
    g_instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\Show2Cam.ControlPanel");
    DWORD err = GetLastError();
    if (g_instanceMutex && err != ERROR_ALREADY_EXISTS) return true;
    if (g_instanceMutex && waitForPrevious)
    {
        DWORD w = WaitForSingleObject(g_instanceMutex, 15000);
        if (w == WAIT_OBJECT_0 || w == WAIT_ABANDONED) return true;
    }
    HWND other = FindWindowW(L"S2cPanel", nullptr);
    if (other && !g_startInTray)
    {
        AllowSetForegroundWindow(ASFW_ANY);
        PostMessageW(other, g_msgShowPanel, 0, 0);
    }
    return false;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show)
{
    g_inst = inst;
    AppLogOpen(L"panel");
    AppLog(L"Show2Cam control panel " S2C_VER_STR);

    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    // Elevated helper: Show2Cam.exe --set-count <N>
    if (argv && argc == 3 && _wcsicmp(argv[1], L"--set-count") == 0)
    {
        int n = _wtoi(argv[2]);
        LocalFree(argv);
        if (n < 1 || n > S2C_MAX_CAMERAS_UI) return 4;
        return ApplyCount(n);
    }
    bool restarted = false, tray = false;
    for (int i = 1; argv && i < argc; i++)
    {
        if (_wcsicmp(argv[i], L"--restarted") == 0) restarted = true;
        if (_wcsicmp(argv[i], L"/t") == 0 || _wcsicmp(argv[i], L"-t") == 0 || _wcsicmp(argv[i], L"--tray") == 0) tray = true;
    }
    if (argv) LocalFree(argv);
    g_msgTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    g_msgShowPanel = RegisterWindowMessageW(L"Show2Cam.ShowPanel");
    g_startInTray = tray;

    // Nothing works without the driver, and the driver loads only in test signing mode (Secure Boot off).
#ifdef S2C_UI_TEST
    S2cNotReady notReady = S2cReadyOk;      // layout check under Wine (no driver there); never in a release build
#else
    S2cNotReady notReady = S2cCheckReady();
#endif
    if (notReady != S2cReadyOk && g_startInTray)
    {
        AppLog(L"started in the tray (autostart) but not ready: exiting without a message");
        return 1;
    }
    if (notReady != S2cReadyOk)
    {
        wchar_t setup[MAX_PATH], text[1200];
        GetModuleFileNameW(nullptr, setup, MAX_PATH);
        wchar_t* slash = wcsrchr(setup, L'\\');
        wcscpy(slash ? slash + 1 : setup, L"Show2Cam-Setup.exe");
        bool haveSetup = GetFileAttributesW(setup) != INVALID_FILE_ATTRIBUTES;
        _snwprintf(text, 1200, haveSetup ? L"%ls\n\n%ls" : L"%ls", S2cNotReadyText(notReady), TR(L"Открыть установщик Show2Cam сейчас?"));
        text[1199] = 0;
        if (MessageBoxW(nullptr, text, L"Show2Cam", (haveSetup ? MB_YESNO : MB_OK) | MB_ICONERROR) == IDYES)
            ShellExecuteW(nullptr, L"open", setup, nullptr, nullptr, SW_SHOWNORMAL);
        return 1;
    }
    if (!AcquireSingleInstance(restarted))
    {
        AppLog(L"another control panel is running: activating it");
        return 0;
    }

    g_elevated = IsElevated();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_STANDARD_CLASSES | ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSEXW pc = { sizeof(pc) };
    pc.lpfnWndProc = PreviewProc;
    pc.hInstance = inst;
    pc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    pc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    pc.lpszClassName = L"S2cPreview";
    RegisterClassExW(&pc);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"S2cPanel";
    RegisterClassExW(&wc);

    UINT dpi = GetDpiForSystem();
    RECT r = { 0, 0, MulDiv(600, (int)dpi, 96), MulDiv(608, (int)dpi, 96) };
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    AdjustWindowRectExForDpi(&r, style, FALSE, WS_EX_CONTROLPARENT, dpi);
    wchar_t title[160];
    _snwprintf(title, 160, L"%ls %ls", TR(L"Show2Cam — панель управления"), L"" S2C_VER_STR);
    title[159] = 0;
    HWND hwnd = CreateWindowExW(WS_EX_CONTROLPARENT, L"S2cPanel", title, style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                                r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;
    if (g_startInTray) ToTray();
    else ShowWindow(hwnd, show);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(hwnd, &m))
        {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    MFShutdown();
    CoUninitialize();
    return 0;
}
