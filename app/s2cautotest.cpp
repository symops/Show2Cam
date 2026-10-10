// s2cautotest - randomized end-to-end test of Show2Cam (driver + cameras + the functions the panel uses), as the
// programs see the cameras (Media Foundation).
//
//   s2cautotest [minutes] [--seed N]      default: 10 minutes, random seed
//
// Needs administrator rights (the manifest requests them). Closes a running control panel, remembers the current
// state (number of cameras, names, formats, the panel's camera settings), then for N minutes runs random actions and
// checks their results:
//   picture    a picture of four coloured quarters sent to a camera is what a program reads (colours, orientation,
//              size); while it is open: in use, streaming, the format cannot change; closed again afterwards
//   pattern    the driver's test pattern is not "empty" (programs like DLP agents drop even frames)
//   format     a random size / frame rate: set while closed, offered first, read at that size; invalid ones refused
//   sizes      the usual webcam sizes are offered (640x480 above all); 640x480 at 3 fps can be chosen
//   rename     random names (Latin, Cyrillic, long): the camera, its device (FriendlyName) and the programs see it
//   count      a random number of cameras (1..4): devices added / removed, one device per camera, names kept
//   sources    the panel's sources (text, generator) drive a camera: frames not empty, the generator moves
//   stress     open / close a camera many times
//   cli        s2cctl.exe with random commands: exit codes and the resulting state
//   text       random texts (Latin, Cyrillic, CJK, long) on the Text source: the picture is not empty and follows the text
//   images     pictures made in every format Windows encodes (PNG, JPEG, BMP, GIF, TIFF, JPEG XR): each one shown
//              with its colour; all together with a broken picture and a text file: they change, the junk is skipped
//   videos     clips made in several containers / codecs (MP4 H.264 + AAC stereo / 5.1 / silent, 3GP, WMV + WMA, the
//              MP4 as .mov / .m4v / .divx, a broken file next to a good one): the moving colour fields are shown, and
//              on the second pass the 1 kHz tone arrives on Speak2Mic Microphone (when Speak2Mic is installed)
//   mjpeg      a local MJPEG server: the stream's picture, a colour change, an outage ("No signal", reconnect), and
//              Basic authentication from the address
// The media are made in %TEMP%\s2cautotest-<pid> at the start and removed at the end, the server stopped.
// Everything is restored at the end (also after Ctrl+C). Every check is logged as PASS / FAIL / WARN to
// %ProgramData%\Show2Cam\logs\autotest.log; exit code 1 if anything failed.
#include "camcfg.h"
#include <psapi.h>
#include "atmedia.h"
#include "camdev.h"
#include "applog.h"
#include "devctl.h"
#include "setupcore.h"
#include "../driver/version.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <setupapi.h>
#include <shellapi.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <io.h>
#include <fcntl.h>

// ---------------------------------------------------------------------------
// Output and checks

static int g_pass, g_fail, g_warn;
static volatile LONG g_stop;
static unsigned long long g_rng = 88172645463325252ULL;    // xorshift64* state (no C++ library here)

static void Out(const wchar_t* fmt, ...)
{
    wchar_t buf[1024];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf(buf, 1024, fmt, args);
    va_end(args);
    buf[1023] = 0;
    wprintf(L"%ls\n", buf);
    AppLog(L"%ls", buf);
}

static bool Check(bool ok, const wchar_t* fmt, ...)
{
    wchar_t buf[900];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf(buf, 900, fmt, args);
    va_end(args);
    buf[899] = 0;
    Out(L"  %ls %ls", ok ? L"PASS" : L"FAIL", buf);
    if (ok) g_pass++; else g_fail++;
    return ok;
}

static void Warn(const wchar_t* fmt, ...)
{
    wchar_t buf[900];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf(buf, 900, fmt, args);
    va_end(args);
    buf[899] = 0;
    Out(L"  WARN %ls", buf);
    g_warn++;
}

static int Rand(int lo, int hi)
{
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    unsigned long long r = g_rng * 2685821657736338717ULL;
    return lo + (int)((r >> 33) % (unsigned long long)(hi - lo + 1));
}

static BOOL WINAPI CtrlHandler(DWORD)
{
    InterlockedExchange(&g_stop, 1);        // finish the current action, then restore everything
    return TRUE;
}

// ---------------------------------------------------------------------------
// Cameras

static int Cameras(CamInfo* out)
{
    return CamList(out, S2C_MAX_CAMERAS_UI);
}

static bool FindCam(int index, CamInfo* out)
{
    CamInfo cams[S2C_MAX_CAMERAS_UI];
    int n = Cameras(cams);
    for (int i = 0; i < n; i++)
        if (cams[i].index == index)
        {
            *out = cams[i];
            return true;
        }
    return false;
}

// A random camera that is there now.
static bool PickCam(CamInfo* out)
{
    CamInfo cams[S2C_MAX_CAMERAS_UI];
    int n = Cameras(cams);
    if (!n) return false;
    *out = cams[Rand(0, n - 1)];
    return true;
}

static bool Status(const CamInfo& cam, S2C_STATUS* st)
{
    HANDLE h = CamOpen(cam.path);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = CamGetStatus(h, st);
    CloseHandle(h);
    return ok;
}

// Who has the camera open now ("" when nobody): the program names the driver saw.
static ULONG CameraUsers(const CamInfo& cam, wchar_t* who, size_t len)
{
    who[0] = 0;
    S2C_STATUS st;
    if (!Status(cam, &st)) return 0;
    for (int u = 0; u < S2C_MAX_USERS; u++)
    {
        if (!st.UserPids[u]) continue;
        wchar_t one[64];
        WCHAR name[33];
        wcsncpy(name, st.UserNames[u], 32);
        name[32] = 0;
        _snwprintf(one, 64, L"%ls%ls (pid %lu)", who[0] ? L", " : L"", name[0] ? name : L"?", st.UserPids[u]);
        one[63] = 0;
        wcsncat(who, one, len - wcslen(who) - 1);
    }
    return st.PinsOpen;
}

static DWORD SetFormat(const CamInfo& cam, ULONG w, ULONG h, ULONG fps)
{
    HANDLE c = CamOpen(cam.path);
    if (c == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD err = CamSetFormat(c, w, h, fps);
    CloseHandle(c);
    return err;
}

static bool SetName(const CamInfo& cam, const wchar_t* name)
{
    HANDLE c = CamOpen(cam.path);
    if (c == INVALID_HANDLE_VALUE) return false;
    bool ok = CamSetName(c, name);
    CloseHandle(c);
    return ok;
}

static bool SendPattern(const CamInfo& cam)
{
    HANDLE c = CamOpen(cam.path);
    if (c == INVALID_HANDLE_VALUE) return false;
    bool ok = CamSendTestPattern(c);
    CloseHandle(c);
    return ok;
}

// Waits until no program has the camera open.
// Waits until at most `others` programs have the camera open (those that had it before the test opened it).
static bool WaitClosed(const CamInfo& cam, DWORD ms, ULONG others = 0)
{
    DWORD start = GetTickCount();
    S2C_STATUS st;
    do
    {
        if (Status(cam, &st) && st.PinsOpen <= others) return true;
        Sleep(100);
    } while (GetTickCount() - start < ms);
    return false;
}

// The FriendlyName of the ROOT\Show2Cam device of camera `index` (Device Parameters\CameraIndex): what DirectShow
// programs compare with the camera's name.
static bool DeviceName(int index, wchar_t* out, size_t len)
{
    out[0] = 0;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"ROOT", nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    SP_DEVINFO_DATA d = { sizeof(d) };
    for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(set, i, &d); i++)
    {
        wchar_t ids[512] = L"";
        if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_HARDWAREID, nullptr, (BYTE*)ids, sizeof(ids) - 4, nullptr)) continue;
        if (_wcsicmp(ids, S2C_HARDWARE_ID)) continue;
        HKEY k = SetupDiOpenDevRegKey(set, &d, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (k == INVALID_HANDLE_VALUE) continue;
        DWORD v = 0xFFFFFFFF, size = sizeof(v);
        RegGetValueW(k, nullptr, L"CameraIndex", RRF_RT_REG_DWORD, nullptr, &v, &size);
        RegCloseKey(k);
        if ((int)v != index) continue;
        found = SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_FRIENDLYNAME, nullptr, (BYTE*)out, (DWORD)(len * sizeof(wchar_t)),
                                                  nullptr) != FALSE;
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

// ---------------------------------------------------------------------------
// A camera as programs open it (Media Foundation), read as RGB32 (Windows converts the camera's YUY2 / NV12).

// The Media Foundation camera that is this device interface: the same device instance and reference string; the
// interface class GUID between them may differ.
static bool SameCamera(const wchar_t* a, const wchar_t* b)
{
    const wchar_t* ga = wcsstr(a, L"#{");
    const wchar_t* gb = wcsstr(b, L"#{");
    const wchar_t* ra = ga ? wcschr(ga, L'}') : nullptr;
    const wchar_t* rb = gb ? wcschr(gb, L'}') : nullptr;
    if (!ga || !gb || !ra || !rb) return _wcsicmp(a, b) == 0;
    return ga - a == gb - b && _wcsnicmp(a, b, ga - a) == 0 && _wcsicmp(ra + 1, rb + 1) == 0;
}

struct Capture
{
    IMFMediaSource*  source = nullptr;
    IMFSourceReader* reader = nullptr;
    UINT32 w = 0, h = 0;
    LONG   stride = 0;
    bool   native = false;                 // read in the camera's own format (no RGB32 conversion): timing only
    ULONG* px = nullptr;                   // the last frame, top-down
    CamInfo cam = {};                      // the camera (for the read watchdog's report)
    LONGLONG ts = 0;                       // its time stamp (100 ns)

    ~Capture() { Close(); }
    void Close()
    {
        if (reader) reader->Release();
        if (source)
        {
            source->Shutdown();
            source->Release();
        }
        reader = nullptr;
        source = nullptr;
        delete[] px;
        px = nullptr;
    }
};

static HRESULT ActivateCamera(const CamInfo& cam, IMFMediaSource** source)
{
    *source = nullptr;
    IMFAttributes* attr = nullptr;
    IMFActivate** devs = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFCreateAttributes(&attr, 1);
    if (SUCCEEDED(hr)) hr = attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (SUCCEEDED(hr)) hr = MFEnumDeviceSources(attr, &devs, &count);
    if (attr) attr->Release();
    if (FAILED(hr)) return hr;
    hr = MF_E_NOT_FOUND;
    for (UINT32 i = 0; i < count; i++)
    {
        WCHAR* link = nullptr;
        UINT32 len = 0;
        if (hr == (HRESULT)MF_E_NOT_FOUND &&
            SUCCEEDED(devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len)))
        {
            if (SameCamera(link, cam.path)) hr = devs[i]->ActivateObject(IID_PPV_ARGS(source));
            CoTaskMemFree(link);
        }
        devs[i]->Release();
    }
    CoTaskMemFree(devs);
    return hr;
}

// Opens the camera; wantW / wantH: that native size (else the camera's default), fps: that rate (0: as offered).
static HRESULT CaptureOpen(const CamInfo& cam, Capture* c, UINT32 wantW = 0, UINT32 wantH = 0, UINT32 fps = 0)
{
    c->Close();
    c->cam = cam;
    HRESULT hr = ActivateCamera(cam, &c->source);
    if (FAILED(hr)) return hr;
    bool typeSet = false;
    if (wantW && fps)
    {
        // A frame rate within the camera's range (not one of its listed types, e.g. 3 fps) is set on the device source
        // itself, as capture programs do; the reader then only converts the frames to RGB32 (setting it through the
        // reader made it look for a frame rate converter: 0xC00D5212).
        IMFPresentationDescriptor* pd = nullptr;
        IMFStreamDescriptor* sd = nullptr;
        IMFMediaTypeHandler* th = nullptr;
        BOOL selected = FALSE;
        HRESULT th_hr = c->source->CreatePresentationDescriptor(&pd);
        if (SUCCEEDED(th_hr)) th_hr = pd->GetStreamDescriptorByIndex(0, &selected, &sd);
        if (SUCCEEDED(th_hr)) th_hr = sd->GetMediaTypeHandler(&th);
        DWORD n = 0;
        if (SUCCEEDED(th_hr)) th->GetMediaTypeCount(&n);
        th_hr = MF_E_INVALIDMEDIATYPE;
        for (DWORD i = 0; i < n && FAILED(th_hr); i++)
        {
            IMFMediaType* t = nullptr;
            if (FAILED(th->GetMediaTypeByIndex(i, &t))) continue;
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h);
            if (w == wantW && h == wantH)
            {
                IMFMediaType* copy = nullptr;
                if (SUCCEEDED(MFCreateMediaType(&copy)) && SUCCEEDED(t->CopyAllItems(copy)))
                {
                    MFSetAttributeRatio(copy, MF_MT_FRAME_RATE, fps, 1);
                    th_hr = th->SetCurrentMediaType(copy);
                }
                if (copy) copy->Release();
            }
            t->Release();
        }
        typeSet = SUCCEEDED(th_hr);
        if (th) th->Release();
        if (sd) sd->Release();
        if (pd) pd->Release();
        if (!typeSet)
        {
            c->Close();
            return th_hr;
        }
    }
    IMFAttributes* attr = nullptr;
    hr = MFCreateAttributes(&attr, 1);
    if (SUCCEEDED(hr)) attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromMediaSource(c->source, attr, &c->reader);
    if (attr) attr->Release();
    const DWORD stream = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
    // The reader starts from the camera's own type: the wanted size is chosen on it as well (also after the source
    // took it - a reader created afterwards gave the camera's own size).
    bool nativeRgb = false;
    if (SUCCEEDED(hr) && wantW)
    {
        // The camera's own RGB32 type of that size (the driver offers RGB32 for every size): asking the reader for a
        // plain RGB32 afterwards made it pick the first native RGB32 type - the camera's own size - instead.
        hr = MF_E_INVALIDMEDIATYPE;
        for (int pass = 0; pass < 2 && hr == (HRESULT)MF_E_INVALIDMEDIATYPE; pass++)
        {
            IMFMediaType* t = nullptr;
            for (DWORD i = 0; hr == (HRESULT)MF_E_INVALIDMEDIATYPE && SUCCEEDED(c->reader->GetNativeMediaType(stream, i, &t)); i++)
            {
                UINT32 w = 0, h = 0;
                GUID sub = {};
                MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h);
                t->GetGUID(MF_MT_SUBTYPE, &sub);
                bool rgb = IsEqualGUID(sub, MFVideoFormat_RGB32);
                if (w == wantW && h == wantH && (pass == 1 || rgb))
                {
                    if (fps) MFSetAttributeRatio(t, MF_MT_FRAME_RATE, fps, 1);
                    hr = c->reader->SetCurrentMediaType(stream, nullptr, t);
                    nativeRgb = SUCCEEDED(hr) && rgb;
                }
                t->Release();
            }
        }
    }
    if (SUCCEEDED(hr) && !nativeRgb)
    {
        IMFMediaType* rgb = nullptr;
        hr = MFCreateMediaType(&rgb);
        if (SUCCEEDED(hr)) rgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) rgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (SUCCEEDED(hr)) hr = c->reader->SetCurrentMediaType(stream, nullptr, rgb);
        if (rgb) rgb->Release();
        // A frame rate the camera offers only as a range (3 fps): Media Foundation has no converter from that type to
        // RGB32 (0xC00D5212); the frames are read as they come (only their timing is looked at then).
        if (FAILED(hr) && wantW && fps)
        {
            c->native = true;
            hr = S_OK;
        }
    }
    if (SUCCEEDED(hr))
    {
        IMFMediaType* cur = nullptr;
        hr = c->reader->GetCurrentMediaType(stream, &cur);
        if (SUCCEEDED(hr))
        {
            MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &c->w, &c->h);
            UINT32 s = 0;
            c->stride = SUCCEEDED(cur->GetUINT32(MF_MT_DEFAULT_STRIDE, &s)) ? (LONG)s : (LONG)(c->w * 4);
            cur->Release();
        }
    }
    if (FAILED(hr)) c->Close();
    else c->px = new ULONG[(size_t)c->w * c->h];
    return hr;
}

// Reads the next frame into c->px (skipping stream ticks without a sample).
// Read watchdog: a synchronous ReadSample has no timeout - a camera that never sends a frame hung the autotest for good.
// After 20 s the reader's source is shut down (ReadSample returns an error: FAIL, the test goes on); 30 s later still
// stuck: everything is restored and the autotest ends (exit code 1).
static Capture* volatile g_reading;
static volatile LONG     g_readSince, g_readStage;
static void (*g_emergencyExit)();

static DWORD WINAPI ReadWatchdog(LPVOID)
{
    for (;;)
    {
        Sleep(1000);
        Capture* c = g_reading;
        if (!c) continue;
        DWORD t = GetTickCount() - (DWORD)g_readSince;
        if (t > 20000 && InterlockedCompareExchange(&g_readStage, 1, 0) == 0)
        {
            S2C_STATUS a = {}, b = {};
            bool okA = Status(c->cam, &a);
            Sleep(1000);
            bool okB = Status(c->cam, &b);
            Check(false, L"no frame from camera %d for 20 s: ReadSample does not return (driver: %ls, streaming %lu, open %lu, "
                         L"frames delivered %llu -> %llu in 1 s, dropped %llu)", c->cam.index + 1, okA && okB ? L"answers" : L"NO ANSWER",
                  b.Streaming, b.PinsOpen, a.FramesDelivered, b.FramesDelivered, b.FramesDropped);
            if (c->source) c->source->Shutdown();
        }
        else if (t > 50000 && InterlockedCompareExchange(&g_readStage, 2, 1) == 1)
        {
            Out(L"FAIL the autotest is stuck in ReadSample (camera %d): restoring and stopping.", c->cam.index + 1);
            if (g_emergencyExit) g_emergencyExit();
            ExitProcess(1);
        }
    }
}

static HRESULT ReadSampleWatched(Capture* c, DWORD* streamIndex, DWORD* flags, LONGLONG* ts, IMFSample** sample)
{
    InterlockedExchange(&g_readSince, (LONG)GetTickCount());
    InterlockedExchange(&g_readStage, 0);
    g_reading = c;
    HRESULT hr = c->reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, streamIndex, flags, ts, sample);
    g_reading = nullptr;
    return hr;
}

static HRESULT CaptureRead(Capture* c)
{
    for (int tries = 0; tries < 50; tries++)
    {
        DWORD streamIndex = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample* sample = nullptr;
        HRESULT hr = ReadSampleWatched(c, &streamIndex, &flags, &ts, &sample);
        if (FAILED(hr)) return hr;
        if (sample && c->native)
        {
            c->ts = ts;
            sample->Release();
            return S_OK;
        }
        if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))
        {
            if (sample) sample->Release();
            return MF_E_END_OF_STREAM;
        }
        if (!sample) continue;
        IMFMediaBuffer* buf = nullptr;
        hr = sample->ConvertToContiguousBuffer(&buf);
        sample->Release();
        if (FAILED(hr)) return hr;
        BYTE* data = nullptr;
        DWORD len = 0;
        hr = buf->Lock(&data, nullptr, &len);
        if (SUCCEEDED(hr))
        {
            LONG pitch = c->stride < 0 ? -c->stride : c->stride;
            if (pitch < (LONG)(c->w * 4)) pitch = (LONG)(c->w * 4);
            if ((DWORD)pitch * c->h > len) hr = E_UNEXPECTED;
            else
                for (UINT32 y = 0; y < c->h; y++)
                {
                    const BYTE* row = data + (size_t)(c->stride < 0 ? c->h - 1 - y : y) * pitch;
                    memcpy(c->px + (size_t)y * c->w, row, (size_t)c->w * 4);
                }
            buf->Unlock();
        }
        buf->Release();
        c->ts = ts;
        return hr;
    }
    return MF_E_NO_SAMPLE_TIMESTAMP;
}

static ULONG PixelAt(const Capture& c, double fx, double fy)
{
    UINT32 x = (UINT32)(fx * c.w), y = (UINT32)(fy * c.h);
    if (x >= c.w) x = c.w - 1;
    if (y >= c.h) y = c.h - 1;
    return c.px[(size_t)y * c.w + x] & 0xFFFFFF;
}

static bool Near(ULONG got, ULONG want, int tol)
{
    for (int s = 0; s < 24; s += 8)
    {
        int a = (int)((got >> s) & 0xFF), b = (int)((want >> s) & 0xFF);
        if (a - b > tol || b - a > tol) return false;
    }
    return true;
}

// Not "empty" the way SearchInform's camera module checks: 24 sample points, not all (nearly) the same.
static bool NotEmpty(const Capture& c)
{
    ULONG first = PixelAt(c, 0.1, 0.1);
    for (int i = 0; i < 24; i++)
        if (!Near(PixelAt(c, 0.1 + 0.8 * (i % 6) / 5.0, 0.1 + 0.8 * (i / 6) / 3.0), first, 12)) return true;
    return false;
}

static double Difference(const ULONG* a, const ULONG* b, size_t n)
{
    unsigned long long sum = 0;
    for (size_t i = 0; i < n; i += 7)
        for (int s = 0; s < 24; s += 8)
        {
            int d = (int)((a[i] >> s) & 0xFF) - (int)((b[i] >> s) & 0xFF);
            sum += d < 0 ? -d : d;
        }
    return (double)sum / ((n / 7 + 1) * 3.0);
}

// ---------------------------------------------------------------------------
// Actions

static const ULONG kQuarters[4] = { 0xE02020, 0x20C040, 0x3040E0, 0xF0F0F0 };    // red, green, blue, white (BGRA)

static void ActionPicture()
{
    CamInfo cam;
    if (!PickCam(&cam)) return;
    S2C_STATUS st;
    if (!Check(Status(cam, &st), L"camera %d: status", cam.index + 1)) return;
    Out(L"[picture] camera %d \"%ls\", %lux%lu %lu fps", cam.index + 1, cam.name, st.Width, st.Height, st.Fps);
    {
        wchar_t who[400];
        if (CameraUsers(cam, who, 400)) Out(L"  info camera %d already open in: %ls", cam.index + 1, who);
    }
    HANDLE h = CamOpen(cam.path);
    CamFrame frame;
    bool sent = h != INVALID_HANDLE_VALUE && frame.Resize(st.Width, st.Height);
    if (sent)
    {
        for (ULONG y = 0; y < st.Height; y++)
            for (ULONG x = 0; x < st.Width; x++)
                frame.Pixels()[y * st.Width + x] = 0xFF000000 | kQuarters[(y >= st.Height / 2 ? 2 : 0) + (x >= st.Width / 2 ? 1 : 0)];
        sent = CamSendFrame(h, frame);
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!Check(sent, L"picture sent")) return;

    Capture c;
    HRESULT hr = CaptureOpen(cam, &c);
    if (!Check(SUCCEEDED(hr), L"opened by Media Foundation (0x%08lX)", (unsigned long)hr)) return;
    Check(c.w == st.Width && c.h == st.Height, L"default size %ux%u = the camera's %lux%lu", c.w, c.h, st.Width, st.Height);
    bool read = true;
    for (int i = 0; i < 5 && read; i++) read = SUCCEEDED(hr = CaptureRead(&c));
    if (Check(read, L"5 frames read (0x%08lX)", (unsigned long)hr))
    {
        const double fx[4] = { 0.25, 0.75, 0.25, 0.75 }, fy[4] = { 0.25, 0.25, 0.75, 0.75 };
        const wchar_t* names[4] = { L"top left red", L"top right green", L"bottom left blue", L"bottom right white" };
        for (int q = 0; q < 4; q++)
        {
            ULONG got = PixelAt(c, fx[q], fy[q]);
            Check(Near(got, kQuarters[q], 48), L"%ls: %06lX (sent %06lX)", names[q], got, kQuarters[q]);
        }
    }
    S2C_STATUS open;
    if (Check(Status(cam, &open), L"status while open"))
    {
        Check(open.PinsOpen >= 1 && open.Streaming >= 1, L"in use while open (programs %lu, streaming %lu)", open.PinsOpen, open.Streaming);
        bool user = false;
        for (int u = 0; u < S2C_MAX_USERS; u++) user = user || open.UserPids[u] != 0;
        Check(user, L"the driver knows who uses it");
        Check(open.FramesDelivered > st.FramesDelivered, L"frames delivered %llu -> %llu", st.FramesDelivered, open.FramesDelivered);
        DWORD err = SetFormat(cam, 640, 480, 15);
        Check(err == ERROR_BUSY || (open.Width == 640 && open.Height == 480 && open.Fps == 15 && err == ERROR_SUCCESS),
              L"format change refused while open (%lu)", err);
    }
    c.Close();
    Check(WaitClosed(cam, 5000, st.PinsOpen), st.PinsOpen ? L"back to the %lu program(s) that had it open before" :
          L"not in use after closing", st.PinsOpen);
    SendPattern(cam);
}

static void ActionPattern()
{
    CamInfo cam;
    if (!PickCam(&cam)) return;
    Out(L"[pattern] camera %d", cam.index + 1);
    Check(SendPattern(cam), L"test pattern set");
    Capture c;
    HRESULT hr = CaptureOpen(cam, &c);
    if (!Check(SUCCEEDED(hr), L"opened (0x%08lX)", (unsigned long)hr)) return;
    hr = CaptureRead(&c);
    if (SUCCEEDED(hr)) hr = CaptureRead(&c);
    if (Check(SUCCEEDED(hr), L"frames read (0x%08lX)", (unsigned long)hr)) Check(NotEmpty(c), L"the test pattern is not empty");
    c.Close();
    WaitClosed(cam, 5000);
}

static void ActionFormat()
{
    CamInfo cam;
    if (!PickCam(&cam)) return;
    static const ULONG kSizes[][2] = { { 640, 480 }, { 1280, 720 }, { 1920, 1080 }, { 800, 600 }, { 320, 240 }, { 1024, 768 },
                                       { 1600, 900 }, { 720, 576 }, { 2560, 1440 }, { 176, 144 } };
    static const ULONG kRates[] = { 1, 3, 5, 10, 15, 24, 25, 30, 50, 60 };
    int s = Rand(0, (int)(sizeof(kSizes) / sizeof(kSizes[0])) - 1);
    ULONG w = kSizes[s][0], h = kSizes[s][1], fps = kRates[Rand(0, (int)(sizeof(kRates) / sizeof(kRates[0])) - 1)];
    Out(L"[format] camera %d: %lux%lu %lu fps", cam.index + 1, w, h, fps);
    if (!WaitClosed(cam, 3000))
    {
        wchar_t who[400];
        CameraUsers(cam, who, 400);
        Warn(L"camera %d is in use by another program (%ls): skipped", cam.index + 1, who[0] ? who : L"?");
        return;
    }
    DWORD err = SetFormat(cam, w, h, fps);
    if (!Check(err == ERROR_SUCCESS, L"format set (%lu)", err)) return;
    S2C_STATUS st;
    Check(Status(cam, &st) && st.Width == w && st.Height == h && st.Fps == fps, L"status %lux%lu %lu fps", st.Width, st.Height, st.Fps);
    Capture c;
    HRESULT hr = CaptureOpen(cam, &c);
    if (Check(SUCCEEDED(hr), L"opened (0x%08lX)", (unsigned long)hr))
    {
        Check(c.w == w && c.h == h, L"offered first: %ux%u", c.w, c.h);
        hr = CaptureRead(&c);
        Check(SUCCEEDED(hr), L"frame read (0x%08lX)", (unsigned long)hr);
    }
    c.Close();
    WaitClosed(cam, 5000);
    // invalid ones
    err = SetFormat(cam, w + 1, h, fps);
    Check(err != ERROR_SUCCESS, L"odd width %lu refused (%lu)", w + 1, err);
    err = SetFormat(cam, S2C_MAX_WIDTH + 2, h, fps);
    Check(err != ERROR_SUCCESS, L"width %d refused (%lu)", S2C_MAX_WIDTH + 2, err);
    err = SetFormat(cam, w, h, S2C_MAX_FPS + 1);
    Check(err != ERROR_SUCCESS, L"%d fps refused (%lu)", S2C_MAX_FPS + 1, err);
    Check(Status(cam, &st) && st.Width == w && st.Height == h && st.Fps == fps, L"format unchanged by the refused ones");
}

static void ActionSizes()
{
    CamInfo cam;
    if (!PickCam(&cam)) return;
    Out(L"[sizes] camera %d", cam.index + 1);
    IMFMediaSource* source = nullptr;
    HRESULT hr = ActivateCamera(cam, &source);
    if (!Check(SUCCEEDED(hr), L"found by Media Foundation (0x%08lX)", (unsigned long)hr)) return;
    IMFSourceReader* reader = nullptr;
    hr = MFCreateSourceReaderFromMediaSource(source, nullptr, &reader);
    bool usual[7] = {};
    static const UINT32 kUsual[7][2] = { { 1920, 1080 }, { 1280, 720 }, { 960, 540 }, { 800, 600 }, { 640, 480 }, { 640, 360 }, { 320, 240 } };
    int types = 0;
    bool valid = true;
    if (SUCCEEDED(hr))
    {
        IMFMediaType* t = nullptr;
        for (DWORD i = 0; SUCCEEDED(reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, i, &t)); i++)
        {
            UINT32 w = 0, h = 0;
            MFGetAttributeSize(t, MF_MT_FRAME_SIZE, &w, &h);
            types++;
            valid = valid && w >= S2C_MIN_WIDTH && w <= S2C_MAX_WIDTH && h >= S2C_MIN_HEIGHT && h <= S2C_MAX_HEIGHT && !(w & 1) && !(h & 1);
            for (int u = 0; u < 7; u++) usual[u] = usual[u] || (w == kUsual[u][0] && h == kUsual[u][1]);
            t->Release();
        }
        reader->Release();
    }
    source->Shutdown();
    source->Release();
    Check(types > 0 && valid, L"%d formats offered, all within the limits", types);
    for (int u = 0; u < 7; u++) Check(usual[u], L"%ux%u offered", kUsual[u][0], kUsual[u][1]);
    WaitClosed(cam, 5000);

    // What DLP camera modules ask for: 640x480 at 3 fps.
    Capture c;
    hr = CaptureOpen(cam, &c, 640, 480, 3);
    if (FAILED(hr))
    {
        // Media Foundation's camera source keeps to the rates of its listed types; programs that ask for another one
        // (SearchInform: 3 fps) use DirectShow, where the driver takes 1-60 fps.
        Out(L"  info 640x480 at 3 fps: Media Foundation keeps the listed frame rates (0x%08lX)", (unsigned long)hr);
        return;
    }
    LONGLONG t0 = 0;
    hr = CaptureRead(&c);
    if (SUCCEEDED(hr)) t0 = c.ts;
    if (SUCCEEDED(hr)) hr = CaptureRead(&c);
    wchar_t who[400];
    ULONG others = CameraUsers(cam, who, 400) > 1 ? 1 : 0;      // besides this test's own open
    if (SUCCEEDED(hr) && (c.w != 640 || c.h != 480) && others)
    {
        // A camera another program streams is shared: Windows keeps that program's format for every client.
        Out(L"  info 640x480 at 3 fps: %ux%u - the camera is shared with %ls, Windows keeps its format", c.w, c.h, who);
        c.Close();
        WaitClosed(cam, 5000, others);
        return;
    }
    if (Check(SUCCEEDED(hr) && c.w == 640 && c.h == 480, L"640x480 frames at 3 fps: %ux%u (0x%08lX)", c.w, c.h, (unsigned long)hr))
    {
        double ms = (c.ts - t0) / 10000.0;
        if (ms > 200 && ms < 500) Check(true, L"3 fps: %.0f ms between frames", ms);
        else Warn(L"3 fps asked, %.0f ms between frames", ms);
    }
    c.Close();
    WaitClosed(cam, 5000);
}

static void RandomName(wchar_t* out, int index)
{
    static const wchar_t* kWords[] = { L"Studio", L"Камера", L"Desk", L"Конференция", L"Show2Cam", L"Test", L"Зал", L"Cam",
                                       L"Окно", L"Room" };
    int kind = Rand(0, 2);
    if (kind == 2)
    {
        // the longest name: S2C_NAME_CHARS - 1 characters
        for (int i = 0; i < S2C_NAME_CHARS - 1; i++) out[i] = (wchar_t)(i % 3 ? L'а' + Rand(0, 31) : L'A' + Rand(0, 25));
        out[S2C_NAME_CHARS - 1] = 0;
        return;
    }
    _snwprintf(out, S2C_NAME_CHARS, L"%ls %ls %d", kWords[Rand(0, 9)], kWords[Rand(0, 9)], index + Rand(1, 999));
    out[S2C_NAME_CHARS - 1] = 0;
}

static bool WaitName(int index, const wchar_t* name, CamInfo* cam)
{
    for (int t = 0; t < 30; t++)
    {
        if (FindCam(index, cam) && !wcscmp(cam->name, name)) return true;
        Sleep(100);
    }
    return false;
}

static void ActionRename()
{
    CamInfo cam;
    if (!PickCam(&cam)) return;
    wchar_t name[S2C_NAME_CHARS];
    RandomName(name, cam.index);
    Out(L"[rename] camera %d \"%ls\" -> \"%ls\"", cam.index + 1, cam.name, name);
    if (!Check(SetName(cam, name), L"name set")) return;
    Check(WaitName(cam.index, name, &cam), L"the camera's name (device interface) is \"%ls\"", cam.name);
    wchar_t dev[256];
    for (int t = 0; t < 30 && !(DeviceName(cam.index, dev, 256) && !wcscmp(dev, name)); t++) Sleep(100);
    Check(!wcscmp(dev, name), L"the device's FriendlyName is \"%ls\"", dev);
}

static void ActionCount()
{
    CamInfo cams[S2C_MAX_CAMERAS_UI];
    int before = Cameras(cams);
    int want = Rand(1, 4);
    if (want == before) want = before == 1 ? 2 : before - 1;
    wchar_t names[S2C_MAX_CAMERAS_UI][S2C_NAME_CHARS] = {};
    for (int i = 0; i < before; i++) wcscpy(names[cams[i].index], cams[i].name);
    Out(L"[count] %d -> %d camera(s)", before, want);
    bool reboot = false;
    bool ok = SetupSetCameraCount(want, true, &reboot, [](void*, const wchar_t* line) { AppLog(L"  setup: %ls", line); }, nullptr);
    if (!Check(ok, L"number of cameras set")) return;
    if (reboot) Warn(L"Windows asks for a restart");
    int n = 0;
    for (int t = 0; t < 100 && (n = Cameras(cams)) != want; t++) Sleep(200);
    Check(n == want, L"%d camera(s) there", n);
    Check(SetupDeviceCount() == want, L"%d device(s): one per camera", SetupDeviceCount());
    bool numbers = true;
    for (int i = 0; i < n; i++) numbers = numbers && cams[i].index == i;
    Check(numbers, L"cameras numbered 1..%d", n);
    for (int i = 0; i < n; i++)
    {
        if (names[i][0]) Check(!wcscmp(cams[i].name, names[i]), L"camera %d kept its name \"%ls\"", i + 1, cams[i].name);
        wchar_t dev[256];
        bool got = false;
        for (int t = 0; t < 30 && !(got = DeviceName(i, dev, 256) && !wcscmp(dev, cams[i].name)); t++) Sleep(100);
        Check(got, L"camera %d: device FriendlyName = camera name \"%ls\"", i + 1, cams[i].name);
    }
}

static void ActionSources()
{
    CamInfo cam;
    if (!PickCam(&cam)) return;
    CamConfig cfg;
    CamConfigDefault(cam.index, &cfg);
    bool generator = Rand(0, 1) == 1;
    cfg.kind = generator ? SourceGenerator : SourceText;
    _snwprintf(cfg.text, 256, L"Autotest %d · Автотест", Rand(1, 9999));
    cfg.width = 640;
    cfg.height = 480;
    cfg.fps = 15;
    Out(L"[sources] camera %d: %ls", cam.index + 1, generator ? L"generator" : cfg.text);
    if (!WaitClosed(cam, 3000))
    {
        wchar_t who[400];
        CameraUsers(cam, who, 400);
        Warn(L"camera %d is in use by another program (%ls): skipped", cam.index + 1, who[0] ? who : L"?");
        return;
    }
    CameraRunner* r = RunnerStart(cam.index, cam.path, cfg, nullptr, 0);
    if (!Check(r != nullptr, L"source started")) return;
    S2C_STATUS st = {};
    for (int t = 0; t < 50 && !(Status(cam, &st) && st.Width == 640 && st.Height == 480 && st.SourceWidth); t++) Sleep(100);
    Check(st.Width == 640 && st.Height == 480 && st.Fps == 15, L"the source set the format: %lux%lu %lu fps", st.Width, st.Height, st.Fps);
    Check(st.SourceWidth != 0, L"the driver gets pictures (%lux%lu)", st.SourceWidth, st.SourceHeight);
    Capture c;
    HRESULT hr = CaptureOpen(cam, &c);
    if (Check(SUCCEEDED(hr), L"opened (0x%08lX)", (unsigned long)hr))
    {
        hr = CaptureRead(&c);
        if (SUCCEEDED(hr)) hr = CaptureRead(&c);
        if (Check(SUCCEEDED(hr), L"frame read (0x%08lX)", (unsigned long)hr))
        {
            Check(NotEmpty(c), L"the picture is not empty");
            if (generator)
            {
                size_t n = (size_t)c.w * c.h;
                ULONG* first = new ULONG[n];
                memcpy(first, c.px, n * 4);
                Sleep(1500);
                for (int i = 0; i < 20 && SUCCEEDED(hr); i++) hr = CaptureRead(&c);     // drain the queued frames
                double d = SUCCEEDED(hr) ? Difference(first, c.px, n) : 0;
                Check(d > 1.0, L"the generator moves (mean difference %.1f)", d);
                delete[] first;
            }
        }
    }
    c.Close();
    RunnerStop(r);
    WaitClosed(cam, 5000);
}

static void ActionStress()
{
    CamInfo cam;
    if (!PickCam(&cam)) return;
    int n = Rand(5, 15), ok = 0;
    wchar_t who[400];
    ULONG others = CameraUsers(cam, who, 400);
    Out(L"[stress] camera %d opened and closed %d times%ls%ls", cam.index + 1, n, others ? L"; already open in: " : L"", who);
    for (int i = 0; i < n && !g_stop; i++)
    {
        Capture c;
        if (SUCCEEDED(CaptureOpen(cam, &c)) && (Rand(0, 1) || SUCCEEDED(CaptureRead(&c)))) ok++;
    }
    Check(ok == n, L"%d of %d opened", ok, n);
    Check(WaitClosed(cam, 5000, others), others ? L"back to the %lu program(s) that had it open before" : L"not in use afterwards",
          others);
}


// ---------------------------------------------------------------------------
// Generated media (atmedia.h): text, pictures of every format, video clips, a local MJPEG server

static TestMedia g_media;
static bool      g_haveMedia, g_mjOk;
static USHORT    g_mjPort;

static void MediaLog(const wchar_t* line) { Out(L"%ls", line); }
static void MediaWarn(const wchar_t* line) { Warn(L"%ls", line); }

// The average colour of a 9x9 patch (compression noise).
static ULONG AvgColor(const Capture& c, double fx, double fy)
{
    int cx = (int)(fx * c.w), cy = (int)(fy * c.h), n = 0;
    unsigned r = 0, g = 0, b = 0;
    for (int y = cy - 4; y <= cy + 4; y++)
        for (int x = cx - 4; x <= cx + 4; x++)
        {
            if (x < 0 || y < 0 || x >= (int)c.w || y >= (int)c.h) continue;
            ULONG p = c.px[(size_t)y * c.w + x];
            r += (p >> 16) & 255;
            g += (p >> 8) & 255;
            b += p & 255;
            n++;
        }
    if (!n) return 0;
    return ((r / n) << 16) | ((g / n) << 8) | (b / n);
}

// Which of the first `count` test colours it is (-1: none within the tolerance).
static int ColorIndex(ULONG got, int count, int tol)
{
    for (int i = 0; i < count; i++)
        if (Near(got, kTestColors[i], tol)) return i;
    return -1;
}

// The newest frame (the reader's queue drained).
static bool Fresh(Capture* c, int frames = 3)
{
    for (int i = 0; i < frames; i++)
        if (FAILED(CaptureRead(c))) return false;
    return true;
}

// A source of the panel on the camera, and the camera opened as programs open it.
static CameraRunner* StartSource(const CamInfo& cam, const CamConfig& cfg, Capture* c)
{
    if (!WaitClosed(cam, 3000))
    {
        wchar_t who[400];
        CameraUsers(cam, who, 400);
        Warn(L"camera %d is in use by another program (%ls): skipped", cam.index + 1, who[0] ? who : L"?");
        return nullptr;
    }
    CameraRunner* r = RunnerStart(cam.index, cam.path, cfg, nullptr, 0);
    if (!Check(r != nullptr, L"source started")) return nullptr;
    S2C_STATUS st = {};
    for (int t = 0; t < 60 && !(Status(cam, &st) && st.SourceWidth && st.Width == cfg.width && st.Height == cfg.height); t++) Sleep(100);
    HRESULT hr = CaptureOpen(cam, c);
    if (!Check(SUCCEEDED(hr), L"camera opened (0x%08lX)", (unsigned long)hr))
    {
        RunnerStop(r);
        return nullptr;
    }
    return r;
}

static void StopSource(CameraRunner* r, const CamInfo& cam, Capture* c)
{
    c->Close();
    RunnerStop(r);
    WaitClosed(cam, 5000);
}

static CamConfig TestConfig(int index, int kind)
{
    CamConfig cfg;
    CamConfigDefault(index, &cfg);
    cfg.kind = kind;
    cfg.width = kTestW;
    cfg.height = kTestH;
    cfg.fps = 15;
    cfg.audioMode = AudioSpeak2Mic;
    return cfg;
}

static void RandomText(wchar_t* out, size_t len, int avoid = -1, int* used = nullptr)
{
    static const wchar_t* kTexts[] = { L"Autotest", L"Проверка камеры Show2Cam", L"日本語のテキスト · 中文字幕", L"Ünïcödé — ñ ç ø ß",
                                       L"1234567890", L"Камера №%d: съешь же ещё этих мягких французских булок", L"A", L"%d" };
    int k;
    do k = Rand(0, (int)(sizeof(kTexts) / sizeof(kTexts[0]))); while (k == avoid);
    if (used) *used = k;
    if (k == (int)(sizeof(kTexts) / sizeof(kTexts[0])))
    {
        // a long one (wraps)
        size_t n = 0;
        while (n + 12 < len && n < 200) n += _snwprintf(out + n, len - n, L"слово%d word ", Rand(0, 99));
        return;
    }
    _snwprintf(out, len, kTexts[k], Rand(1, 9999));
    out[len - 1] = 0;
}

static void ActionTextGen()
{
    CamInfo cam;
    if (!PickCam(&cam)) return;
    CamConfig a = TestConfig(cam.index, SourceText), b = a;
    int ka = -1;
    // the second text from another template: two of the same one (another number) may look almost alike
    RandomText(a.text, 256, -1, &ka);
    do RandomText(b.text, 256, ka); while (!wcscmp(a.text, b.text));
    Out(L"[text] camera %d: \"%ls\" then \"%ls\"", cam.index + 1, a.text, b.text);
    Capture c;
    CameraRunner* r = StartSource(cam, a, &c);
    if (!r) return;
    ULONG* first = nullptr;
    if (Check(Fresh(&c), L"frames read"))
    {
        Check(NotEmpty(c), L"text picture not empty");
        first = new ULONG[(size_t)c.w * c.h];
        memcpy(first, c.px, (size_t)c.w * c.h * 4);
        RunnerConfigure(r, b);
        Sleep(1000);
        if (Check(Fresh(&c, 5), L"frames read after the change"))
        {
            Check(NotEmpty(c), L"new text picture not empty");
            double d = Difference(first, c.px, (size_t)c.w * c.h);
            Check(d > 1.0, L"the picture changed with the text (mean difference %.1f)", d);
        }
    }
    delete[] first;
    StopSource(r, cam, &c);
}

static void ActionImages()
{
    if (!g_media.imageCount) return;
    CamInfo cam;
    if (!PickCam(&cam)) return;
    CamConfig cfg = TestConfig(cam.index, SourceImages);
    Capture c;
    if (Rand(0, 2))
    {
        const TestImage& img = g_media.images[Rand(0, g_media.imageCount - 1)];
        wcscpy(cfg.imageFolder, img.folder);
        Out(L"[images] camera %d: one %ls picture", cam.index + 1, img.format);
        CameraRunner* r = StartSource(cam, cfg, &c);
        if (!r) return;
        Sleep(1000);
        if (Check(Fresh(&c), L"frames read"))
        {
            ULONG got = AvgColor(c, 0.5, 0.5);
            Check(Near(got, img.color, 40), L"%ls picture shown: centre %06lX (made %06lX)", img.format, got, img.color);
            Check(NotEmpty(c), L"picture not empty");
        }
        StopSource(r, cam, &c);
        return;
    }
    // every format together, with a broken picture and a text file among them: changes every 5 s
    wcscpy(cfg.imageFolder, g_media.allImages);
    Out(L"[images] camera %d: every generated format + broken pictures, 13 s", cam.index + 1);
    CameraRunner* r = StartSource(cam, cfg, &c);
    if (!r) return;
    bool seen[8] = {};
    int samples = 0, unknown = 0, distinct = 0;
    for (int t = 0; t < 13 && !g_stop; t++)
    {
        Sleep(800);
        if (!Fresh(&c)) break;
        samples++;
        ULONG got = AvgColor(c, 0.5, 0.5);
        int k = -1;
        for (int i = 0; i < g_media.imageCount && k < 0; i++)
            if (Near(got, g_media.images[i].color, 40)) k = i;
        if (k < 0) unknown++;
        else if (!seen[k]) { seen[k] = true; distinct++; }
    }
    Check(samples >= 10, L"%d samples", samples);
    Check(distinct >= 2, L"%d different pictures shown", distinct);
    Check(unknown == 0, L"%d sample(s) showed no test picture (a broken file or the text file not skipped at once?)", unknown);
    StopSource(r, cam, &c);
}

static void ActionVideos()
{
    if (!g_media.videoCount) return;
    CamInfo cam;
    if (!PickCam(&cam)) return;
    const TestVideo& v = g_media.videos[Rand(0, g_media.videoCount - 1)];
    CamConfig cfg = TestConfig(cam.index, SourceVideo);
    wcscpy(cfg.videoFolder, v.folder);
    Out(L"[videos] camera %d: %ls (%ls)", cam.index + 1, v.name, v.sound ? (v.channels == 6 ? L"5.1 sound" : L"sound") : L"no sound");
    Capture c;
    CameraRunner* r = StartSource(cam, cfg, &c);
    if (!r) return;
    // the picture over the first pass, the sound during the second one (it used to stop after the first)
    {
        // the samples start when a clip plays (skipping broken files first takes a moment each)
        CamRunStatus rs = {};
        DWORD t0 = GetTickCount();
        for (; GetTickCount() - t0 < 8000; Sleep(100))
        {
            RunnerGetStatus(r, &rs);
            if (rs.state == StateOk && rs.current[0]) break;
        }
        if (GetTickCount() - t0 > 1000) Out(L"  info the clip started after %lu ms", GetTickCount() - t0);
        Fresh(&c, 2);
    }
    bool seen[4] = {};
    int samples = 0, matched = 0, distinct = 0;
    DWORD start = GetTickCount();
    auto sample = [&](DWORD until) {
        while (GetTickCount() - start < until && !g_stop)
        {
            if (!Fresh(&c, 2)) return;
            samples++;
            int k = ColorIndex(AvgColor(c, 0.5, 0.6), 4, 45);
            if (k >= 0)
            {
                matched++;
                if (!seen[k]) { seen[k] = true; distinct++; }
            }
        }
    };
    sample((DWORD)(kClipSeconds * 1000) + 700);
    MicResult mic = {};
    if (v.sound) MicMeasure(1500, &mic);
    sample((DWORD)(kClipSeconds * 2000) + 1500);
    Check(samples >= 10 && matched * 10 >= samples * 8, L"clip colours shown: %d of %d samples", matched, samples);
    Check(distinct >= v.minColors, L"the picture moves (%d of 4 colour fields seen, %d wanted)", distinct, v.minColors);
    if (v.sound)
    {
        if (!mic.found) Warn(L"Speak2Mic Microphone not found: the clip's sound not checked");
        else if (FAILED(mic.hr)) Warn(L"Speak2Mic Microphone cannot be recorded (0x%08lX)", (unsigned long)mic.hr);
        else
        {
            if (mic.rms <= 0.003 && v.optional)
                Warn(L"no sound on the second pass (level %.4f): Windows does not offer this clip's sound track (Vorbis / Opus in MKV or WebM: not read by Media Foundation, even with Web Media Extensions)", mic.rms);
            else
                Check(mic.rms > 0.003, mic.rms > 0.003 ? L"sound on the second pass: level %.4f" :
                  L"sound on the second pass: level %.4f (microphone muted or at 0 %% in Speak2Mic?)", mic.rms);
            if (mic.rms > 0.003)
            {
                if (mic.tone > 0.3) Check(true, L"the clip's 1 kHz tone: %.0f %% of the sound", mic.tone * 100);
                else if (mic.peakHz >= 960 && mic.peakHz <= 1040)
                    // a short clip played over and over: gaps between the passes, but the sound is the tone
                    Check(true, L"the clip's 1 kHz tone is the strongest frequency (~%d Hz, %.0f %% of the sound)", mic.peakHz,
                          mic.tone * 100);
                else Warn(L"the 1 kHz tone is only %.0f %% of the sound; strongest frequency ~%d Hz (microphone %u Hz %u ch)",
                          mic.tone * 100, mic.peakHz, mic.rate, mic.channels);
            }
        }
    }
    StopSource(r, cam, &c);
}

// Waits until the camera's centre is that colour (or, `notColor`, is not it any more).
static bool WaitColor(Capture* c, ULONG color, DWORD ms, bool notColor = false)
{
    DWORD start = GetTickCount();
    while (GetTickCount() - start < ms && !g_stop)
    {
        if (!Fresh(c, 2)) return false;
        bool same = Near(AvgColor(*c, 0.5, 0.5), color, 40);
        if (same != notColor) return true;
    }
    return false;
}

static void ActionMjpeg()
{
    if (!g_mjOk) return;
    CamInfo cam;
    if (!PickCam(&cam)) return;
    int mode = Rand(0, 2);           // 0: colour change, 1: server outage and reconnect, 2: authentication
    if (mode == 2 && !MjpegStart(&g_mjPort, true))
    {
        Warn(L"MJPEG test server with authentication not started");
        return;
    }
    CamConfig cfg = TestConfig(cam.index, SourceStream);
    _snwprintf(cfg.url, 512, mode == 2 ? L"http://s2c:test@127.0.0.1:%u/" : L"http://127.0.0.1:%u/", g_mjPort);
    ULONG a = kTestColors[Rand(0, 7)];
    MjpegSetColor(a);
    Out(L"[mjpeg] camera %d: %ls, %ls", cam.index + 1, cfg.url, mode == 0 ? L"colour change" : mode == 1 ? L"outage" : L"authentication");
    Capture c;
    CameraRunner* r = StartSource(cam, cfg, &c);
    if (r)
    {
        ULONGLONG sent = MjpegFramesSent();
        bool shown = Check(WaitColor(&c, a, 8000), L"stream picture shown (colour %06lX)", a);
        Check(MjpegFramesSent() > sent, L"the server sent pictures (%lu client(s))", (unsigned long)MjpegClients());
        if (shown && mode == 0)
        {
            ULONG b;
            do b = kTestColors[Rand(0, 7)]; while (b == a);
            MjpegSetColor(b);
            Check(WaitColor(&c, b, 4000), L"the new stream colour %06lX within 4 s", b);
        }
        else if (shown && mode == 1)
        {
            USHORT port = g_mjPort;
            MjpegStop();
            Check(WaitColor(&c, a, 16000, true), L"\"No signal\" within 16 s after the server stopped");
            g_mjOk = MjpegStart(&port, false);
            if (!g_mjOk || port != g_mjPort) Warn(L"MJPEG test server not restarted on port %u", g_mjPort);
            else Check(WaitColor(&c, a, 12000), L"reconnected within 12 s after the server came back");
        }
        StopSource(r, cam, &c);
    }
    if (mode == 2) g_mjOk = MjpegStart(&g_mjPort, false);
}

static int RunCtl(const wchar_t* args)
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    wcscpy(slash ? slash + 1 : exe, L"s2cctl.exe");
    wchar_t cmd[1200];
    _snwprintf(cmd, 1200, L"\"%ls\" %ls", exe, args);
    cmd[1199] = 0;
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;          // its output stays out of this console (it logs to ctl.log)
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        Out(L"  cannot start %ls (%lu)", exe, GetLastError());
        return -1;
    }
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = (DWORD)-1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return (int)code;
}

static void ActionCli()
{
    CamInfo cam;
    if (!PickCam(&cam)) return;
    int n = cam.index + 1;
    wchar_t args[400];
    int rc;
    switch (Rand(0, 8))
    {
    case 0:
        Out(L"[cli] status");
        rc = RunCtl(L"status");
        Check(rc == 0, L"s2cctl status: %d", rc);
        break;
    case 1:
    {
        wchar_t text[64];
        _snwprintf(text, 64, L"Текст %d", Rand(1, 99999));
        _snwprintf(args, 400, L"source %d text \"%ls\"", n, text);
        Out(L"[cli] %ls", args);
        rc = RunCtl(args);
        CamConfig c;
        CamConfigLoad(cam.index, &c);
        Check(rc == 0 && c.kind == SourceText && !wcscmp(c.text, text), L"s2cctl %ls: %d, stored \"%ls\"", args, rc, c.text);
        break;
    }
    case 2:
    {
        static const wchar_t* kBad[] = { L"format 1 641x480", L"format 1 10x10", L"source 1 stream ftp://x", L"source 11 text x",
                                         L"pause 1 maybe", L"bogus", L"name", L"count 0", L"format 1 640x480 61" };
        const wchar_t* bad = kBad[Rand(0, 8)];
        Out(L"[cli] %ls (invalid)", bad);
        rc = RunCtl(bad);
        Check(rc == 2, L"s2cctl %ls: %d (2 expected)", bad, rc);
        break;
    }
    case 3:
    {
        wchar_t name[S2C_NAME_CHARS];
        RandomName(name, cam.index);
        _snwprintf(args, 400, L"name %d \"%ls\"", n, name);
        Out(L"[cli] %ls", args);
        rc = RunCtl(args);
        Check(rc == 0 && WaitName(cam.index, name, &cam), L"s2cctl %ls: %d, name \"%ls\"", args, rc, cam.name);
        break;
    }
    case 4:
    {
        ULONG w = Rand(0, 1) ? 1280 : 640, h = w == 1280 ? 720 : 480, fps = (ULONG)Rand(1, 60);
        _snwprintf(args, 400, L"format %d %lux%lu %lu", n, w, h, fps);
        Out(L"[cli] %ls", args);
        rc = RunCtl(args);
        CamConfig c;
        CamConfigLoad(cam.index, &c);
        Check(rc == 0 && c.width == w && c.height == h && c.fps == fps, L"s2cctl %ls: %d, stored %lux%lu %lu", args, rc, c.width,
              c.height, c.fps);
        break;
    }
    case 5:
    {
        wchar_t path[MAX_PATH];
        GetTempPathW(MAX_PATH - 40, path);
        wcscat(path, L"s2cautotest.ini");
        CamConfig before;
        CamConfigLoad(cam.index, &before);
        _snwprintf(args, 400, L"export \"%ls\"", path);
        Out(L"[cli] export / change / import");
        rc = RunCtl(args);
        Check(rc == 0, L"s2cctl export: %d", rc);
        _snwprintf(args, 400, L"source %d generator", n);
        RunCtl(args);
        _snwprintf(args, 400, L"import \"%ls\"", path);
        rc = RunCtl(args);
        CamConfig after;
        CamConfigLoad(cam.index, &after);
        Check(rc == 0 && after.kind == before.kind && !wcscmp(after.text, before.text) && after.width == before.width &&
              after.fps == before.fps, L"s2cctl import: %d, camera %d settings back", rc, n);
        DeleteFileW(path);
        break;
    }
    case 6:
        _snwprintf(args, 400, L"picture %d pattern", n);
        Out(L"[cli] %ls", args);
        rc = RunCtl(args);
        Check(rc == 0, L"s2cctl %ls: %d", args, rc);
        break;
    case 7:
        _snwprintf(args, 400, L"pause all %ls", Rand(0, 1) ? L"on" : L"off");
        Out(L"[cli] %ls", args);
        rc = RunCtl(args);
        Check(rc == 0, L"s2cctl %ls: %d", args, rc);
        break;
    default:
        _snwprintf(args, 400, L"reset %d", n);
        Out(L"[cli] %ls", args);
        rc = RunCtl(args);
        {
            CamConfig c, def;
            CamConfigLoad(cam.index, &c);
            CamConfigDefault(cam.index, &def);
            wchar_t want[S2C_NAME_CHARS];
            _snwprintf(want, S2C_NAME_CHARS, L"Show2Cam Camera %d", n);
            Check(rc == 0 && c.kind == def.kind && !wcscmp(c.text, def.text) && WaitName(cam.index, want, &cam),
                  L"s2cctl %ls: %d, defaults, name \"%ls\"", args, rc, cam.name);
        }
        break;
    }
}

// ---------------------------------------------------------------------------
// State to restore

struct Snapshot
{
    int count;
    wchar_t names[S2C_MAX_CAMERAS_UI][S2C_NAME_CHARS];
    ULONG w[S2C_MAX_CAMERAS_UI], h[S2C_MAX_CAMERAS_UI], fps[S2C_MAX_CAMERAS_UI];
    bool have[S2C_MAX_CAMERAS_UI];
    CamConfig config[S2C_MAX_CAMERAS_UI];
};

static void TakeSnapshot(Snapshot* s)
{
    ZeroMemory(s->names, sizeof(s->names));
    ZeroMemory(s->have, sizeof(s->have));
    CamInfo cams[S2C_MAX_CAMERAS_UI];
    s->count = Cameras(cams);
    for (int i = 0; i < s->count; i++)
    {
        int k = cams[i].index;
        S2C_STATUS st;
        wcscpy(s->names[k], cams[i].name);
        if (Status(cams[i], &st))
        {
            s->have[k] = true;
            s->w[k] = st.Width;
            s->h[k] = st.Height;
            s->fps[k] = st.Fps;
        }
    }
    for (int i = 0; i < S2C_MAX_CAMERAS_UI; i++) CamConfigLoad(i, &s->config[i]);
}

static void Restore(const Snapshot& s)
{
    Out(L"Restoring: %d camera(s), their names, formats and settings.", s.count);
    CamInfo cams[S2C_MAX_CAMERAS_UI];
    if (Cameras(cams) != s.count && s.count > 0)
    {
        bool reboot = false;
        SetupSetCameraCount(s.count, true, &reboot, [](void*, const wchar_t* line) { AppLog(L"  setup: %ls", line); }, nullptr);
        for (int t = 0; t < 100 && Cameras(cams) != s.count; t++) Sleep(200);
    }
    int n = Cameras(cams);
    for (int i = 0; i < n; i++)
    {
        int k = cams[i].index;
        wchar_t def[S2C_NAME_CHARS];
        _snwprintf(def, S2C_NAME_CHARS, L"Show2Cam Camera %d", k + 1);
        if (s.names[k][0] && wcscmp(cams[i].name, s.names[k])) SetName(cams[i], wcscmp(s.names[k], def) ? s.names[k] : L"");
        WaitClosed(cams[i], 3000);
        if (s.have[k]) SetFormat(cams[i], s.w[k], s.h[k], s.fps[k]);
        SendPattern(cams[i]);
    }
    for (int i = 0; i < S2C_MAX_CAMERAS_UI; i++) CamConfigSave(i, s.config[i]);
    Check(Cameras(cams) == s.count, L"restored: %d camera(s)", Cameras(cams));
}

// The panel was running: started again as the desktop user (not elevated, through Explorer).
static void RestartPanel()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t* slash = wcsrchr(exe, L'\\');
    wcscpy(slash ? slash + 1 : exe, L"Show2Cam.exe");
    if (GetFileAttributesW(exe) == INVALID_FILE_ATTRIBUTES) return;
    wchar_t arg[MAX_PATH + 4];
    _snwprintf(arg, MAX_PATH + 4, L"\"%ls\"", exe);
    arg[MAX_PATH + 3] = 0;
    ShellExecuteW(nullptr, L"open", L"explorer.exe", arg, nullptr, SW_SHOWNORMAL);
    Out(L"Control panel started again.");
}

// The process' memory (private bytes, MB) and handles: logged with every action, grown per action type summed up.
static double ProcessMb(DWORD* handles)
{
    PROCESS_MEMORY_COUNTERS_EX m = {};
    m.cb = sizeof(m);
    GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&m, sizeof(m));
    if (handles) GetProcessHandleCount(GetCurrentProcess(), handles);
    return m.PrivateUsage / 1048576.0;
}

static Snapshot* g_snapshot;            // for the read watchdog's emergency stop

int wmain(int argc, wchar_t** argv)
{
    _setmode(_fileno(stdout), _O_U16TEXT);
    AppLogOpen(L"autotest");
    SetConsoleTitleW(L"Show2Cam autotest");
    int minutes = 10;
    unsigned seed = (unsigned)GetTickCount() ^ (unsigned)GetCurrentProcessId() * 2654435761u;
    for (int i = 1; i < argc; i++)
    {
        if (_wcsicmp(argv[i], L"--seed") == 0 && i + 1 < argc) seed = (unsigned)wcstoul(argv[++i], nullptr, 10);
        else if (_wtoi(argv[i]) > 0) minutes = _wtoi(argv[i]);
        else
        {
            Out(L"Usage: s2cautotest [minutes] [--seed N]");
            return 2;
        }
    }
    g_rng = 0x9E3779B97F4A7C15ULL ^ ((unsigned long long)seed << 1 | 1);
    SetConsoleCtrlHandler(CtrlHandler, TRUE);
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    S2cNotReady notReady = S2cCheckReady();
    if (notReady != S2cReadyOk)
    {
        Out(L"%ls", S2cNotReadyTextEn(notReady));
        DWORD procs[4];
        if (GetConsoleProcessList(procs, 4) <= 1)
        {
            Out(L"Press Enter to exit.");
            getwchar();
        }
        return 3;
    }
    Out(L"Show2Cam autotest %ls: %d minute(s), seed %u (repeat with --seed %u). Ctrl+C stops and restores.", L"" S2C_VER_STR,
        minutes, seed, seed);
    Out(L"Log: %ls", AppLogPath());

    // The control panel would send its own pictures to the cameras: close it (no questions), start it again at the end.
    bool panelWasRunning = false;
    for (int tries = 0; tries < 3; tries++)
    {
        HWND panel = FindWindowW(S2C_PANEL_CLASS, nullptr);
        if (!panel) break;
        panelWasRunning = true;
        Out(L"Closing the Show2Cam control panel.");
        DWORD pid = 0;
        GetWindowThreadProcessId(panel, &pid);
        HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
        PostMessageW(panel, WM_CLOSE, 0x53324331 /* S2C_CLOSE_FOR_SETUP */, 0);
        if (proc)
        {
            WaitForSingleObject(proc, 5000);
            CloseHandle(proc);
        }
        else Sleep(1500);
    }

    CamInfo cams[S2C_MAX_CAMERAS_UI];
    if (!Cameras(cams))
    {
        Out(L"FAIL no Show2Cam cameras: install Show2Cam first.");
        return 1;
    }
    Snapshot snap;
    TakeSnapshot(&snap);
    g_snapshot = &snap;
    g_emergencyExit = [] {
        MjpegStop();
        TestMediaDelete(&g_media);
        Restore(*g_snapshot);
        DriverLogWriterStop();
        Out(L"==== stopped: stuck in ReadSample; %d passed, %d FAILED, %d warning(s) ====", g_pass, g_fail, g_warn);
    };
    // driver.log as the panel writes it (the autotest closes the panel); the read watchdog
    DriverLogWriterStart();
    CloseHandle(CreateThread(nullptr, 0, ReadWatchdog, nullptr, 0, nullptr));
    // the test media: made now, removed at the end
    g_haveMedia = TestMediaCreate(&g_media, MediaLog, MediaWarn);
    g_mjPort = 0;
    g_mjOk = MjpegStart(&g_mjPort, false);
    if (g_mjOk) Out(L"MJPEG test server: http://127.0.0.1:%u/", g_mjPort);
    else Warn(L"MJPEG test server not started: stream checks skipped");
    Out(L"Start: %d camera(s)", snap.count);
    for (int i = 0; i < S2C_MAX_CAMERAS_UI; i++)
        if (snap.names[i][0]) Out(L"  camera %d \"%ls\" %lux%lu %lu fps", i + 1, snap.names[i], snap.w[i], snap.h[i], snap.fps[i]);

    // Weighted random actions.
    struct { void (*fn)(); int weight; const wchar_t* name; double grown; int runs; } actions[] = {
        { ActionPicture, 20, L"picture" }, { ActionPattern, 8, L"pattern" }, { ActionFormat, 14, L"format" },
        { ActionSizes, 8, L"sizes" }, { ActionRename, 12, L"rename" }, { ActionCount, 5, L"count" },
        { ActionSources, 10, L"sources" }, { ActionStress, 6, L"stress" }, { ActionCli, 15, L"cli" },
        { ActionTextGen, 8, L"text" }, { ActionImages, 10, L"images" }, { ActionVideos, 12, L"videos" },
        { ActionMjpeg, 10, L"mjpeg" },
    };
    int total = 0;
    for (auto& a : actions) total += a.weight;
    DWORD end = GetTickCount() + (DWORD)minutes * 60000;
    int iteration = 0;
    while (!g_stop && (LONG)(end - GetTickCount()) > 0)
    {
        if (!Cameras(cams))
        {
            Sleep(3000);
            if (!Check(Cameras(cams) > 0, L"Show2Cam cameras there")) break;
        }
        int pick = Rand(1, total);
        iteration++;
        wprintf(L"\n");
        AppLog(L"");
        DWORD handles = 0;
        double mb = ProcessMb(&handles);
        Out(L"#%d (%d s left; memory %.1f MB, %lu handles)", iteration, (int)((LONG)(end - GetTickCount()) / 1000), mb, handles);
        for (auto& a : actions)
        {
            if (pick <= a.weight)
            {
                a.fn();
                a.grown += ProcessMb(nullptr) - mb;
                a.runs++;
                break;
            }
            pick -= a.weight;
        }
        if (iteration % 200 == 0)
        {
            // which actions the memory grew with (a leak shows as one steadily growing line)
            wchar_t line[1200] = L"", part[100];
            for (auto& a : actions)
                if (a.runs)
                {
                    _snwprintf(part, 100, L" %ls %+.1f MB (%d);", a.name, a.grown, a.runs);
                    part[99] = 0;
                    wcsncat(line, part, 1199 - wcslen(line));
                }
            Out(L"memory %.1f MB; grown with:%ls", ProcessMb(nullptr), line);
        }
    }
    Out(L"");
    DriverLogWriterStop();
    MjpegStop();
    TestMediaDelete(&g_media);
    Out(L"Test media removed.");
    Restore(snap);
    if (panelWasRunning) RestartPanel();
    Out(L"");
    Out(L"==== %d action(s): %d passed, %d FAILED, %d warning(s); seed %u ====", iteration, g_pass, g_fail, g_warn, seed);
    Out(L"Log: %ls", AppLogPath());
    MediaThreadEnd();
    MFShutdown();
    CoUninitialize();

    DWORD procs[4];
    if (GetConsoleProcessList(procs, 4) <= 1)
    {
        Out(L"Press Enter to exit.");
        getwchar();
    }
    return g_fail ? 1 : 0;
}
