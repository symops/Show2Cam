// s2ccamdiag - how programs see the cameras (all of them: Show2Cam and others, e.g. e2eSoft VCam), to find why a
// program sees one camera and not another. Every way a Windows program finds and opens a webcam:
//   1. Windows registration: the device interfaces (KSCATEGORY_VIDEO / CAPTURE / VIDEO_CAMERA) with FriendlyName,
//      CLSID (KsProxy) and FilterData (the format cache some DirectShow programs read before opening the camera)
//   2. Camera privacy settings (desktop apps may be denied the camera)
//   3. DirectShow ("Video Capture Sources"): open, the formats (IAMStreamConfig), choosing 640x480 / 1280x720 /
//      1920x1080 the way programs do (SetFormat), and a capture graph running for 3 s
//   4. Media Foundation: the formats and 15 frames read
//   5. Video for Windows (very old programs)
// Output on the console and in %ProgramData%\Show2Cam\logs\camdiag-<x64|x86>.log. Build it for x64 and x86: a 32-bit
// program uses the 32-bit DirectShow / Media Foundation, so run s2ccamdiag32.exe for 32-bit programs.
//   s2ccamdiag.exe [--no-run]     (--no-run: do not open / run the cameras, registration and formats only)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dshow.h>
#include <dvdmedia.h>     // VIDEOINFOHEADER2
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <setupapi.h>
#include <shlobj.h>
#include <vfw.h>
#include <stdio.h>
#include <stdarg.h>
#include <wchar.h>
#include <wctype.h>
#include "camdev.h"
#include "../driver/version.h"

static FILE* g_log;

static void Out(const wchar_t* fmt, ...)
{
    wchar_t buf[2048];
    va_list a;
    va_start(a, fmt);
    _vsnwprintf(buf, 2048, fmt, a);
    va_end(a);
    buf[2047] = 0;
    wprintf(L"%ls\n", buf);
    if (g_log)
    {
        fwprintf(g_log, L"%ls\n", buf);
        fflush(g_log);
    }
}

static void Fourcc(const GUID& g, wchar_t* out)
{
    // Video subtypes are FOURCC GUIDs {XXXXXXXX-0000-0010-8000-00AA00389B71}; RGB ones are named.
    const struct { GUID g; const wchar_t* n; } known[] = {
        { MEDIASUBTYPE_RGB24, L"RGB24" }, { MEDIASUBTYPE_RGB32, L"RGB32" }, { MEDIASUBTYPE_ARGB32, L"ARGB32" },
        { MEDIASUBTYPE_RGB565, L"RGB565" }, { MEDIASUBTYPE_RGB555, L"RGB555" },
    };
    for (auto& k : known)
        if (IsEqualGUID(g, k.g)) { wcscpy(out, k.n); return; }
    if (g.Data2 == 0 && g.Data3 == 0x0010 && g.Data4[0] == 0x80 && g.Data4[1] == 0x00 && g.Data4[2] == 0x00 && g.Data4[3] == 0xAA)
    {
        for (int i = 0; i < 4; i++)
        {
            wchar_t c = (wchar_t)((g.Data1 >> (8 * i)) & 0xFF);
            out[i] = c >= 32 && c < 127 ? c : L'?';
        }
        out[4] = 0;
        return;
    }
    _snwprintf(out, 40, L"{%08lX-...}", g.Data1);
}

static const wchar_t* Bitness()
{
#ifdef _WIN64
    return L"x64";
#else
    return L"x86";
#endif
}

// ---------------------------------------------------------------------------
// 1. Windows registration

static void RegistrationCategory(const GUID& cat, const wchar_t* name)
{
    Out(L"  %ls:", name);
    HDEVINFO set = SetupDiGetClassDevsW(&cat, nullptr, nullptr, DIGCF_DEVICEINTERFACE | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) { Out(L"    (cannot list: %lu)", GetLastError()); return; }
    SP_DEVICE_INTERFACE_DATA di = { sizeof(di) };
    int n = 0;
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &cat, i, &di); i++, n++)
    {
        BYTE buf[2048];
        auto* d = (SP_DEVICE_INTERFACE_DETAIL_DATA_W*)buf;
        d->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        SP_DEVINFO_DATA info = { sizeof(info) };
        if (!SetupDiGetDeviceInterfaceDetailW(set, &di, d, sizeof(buf), nullptr, &info)) continue;
        wchar_t inst[256] = L"", service[64] = L"", desc[256] = L"", fname[256] = L"", clsid[64] = L"";
        SetupDiGetDeviceInstanceIdW(set, &info, inst, 256, nullptr);
        SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_SERVICE, nullptr, (BYTE*)service, sizeof(service) - 2, nullptr);
        SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_DEVICEDESC, nullptr, (BYTE*)desc, sizeof(desc) - 2, nullptr);
        DWORD filterData = 0;
        HKEY k = SetupDiOpenDeviceInterfaceRegKey(set, &di, 0, KEY_READ);
        if (k != INVALID_HANDLE_VALUE)
        {
            DWORD sz = sizeof(fname);
            RegGetValueW(k, nullptr, L"FriendlyName", RRF_RT_REG_SZ, nullptr, fname, &sz);
            sz = sizeof(clsid);
            RegGetValueW(k, nullptr, L"CLSID", RRF_RT_REG_SZ, nullptr, clsid, &sz);
            if (RegQueryValueExW(k, L"FilterData", nullptr, nullptr, nullptr, &filterData) != ERROR_SUCCESS) filterData = 0;
            RegCloseKey(k);
        }
        Out(L"    \"%ls\"  device %ls (%ls, service %ls)", fname[0] ? fname : L"(no FriendlyName)", inst, desc, service);
        Out(L"      CLSID %ls, FilterData %lu bytes, %ls", clsid[0] ? clsid : L"(none)", filterData, d->DevicePath);
    }
    if (!n) Out(L"    (none)");
    SetupDiDestroyDeviceInfoList(set);
}

static void Registration()
{
    static const GUID kVideo = { 0x6994ad05, 0x93ef, 0x11d0, { 0xa3, 0xcc, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 } };
    static const GUID kCapture = { 0x65e8773d, 0x8f56, 0x11d0, { 0xa3, 0xb9, 0x00, 0xa0, 0xc9, 0x22, 0x31, 0x96 } };
    static const GUID kCamera = { 0xe5323777, 0xf976, 0x4f5b, { 0x9b, 0x55, 0xb9, 0x46, 0x99, 0xc4, 0x6e, 0x44 } };
    static const GUID kSensor = { 0x24e552d7, 0x6523, 0x47f7, { 0xa6, 0x47, 0xd3, 0x46, 0x5b, 0xf1, 0xf5, 0xca } };
    Out(L"");
    Out(L"[1] Windows registration (device interfaces)");
    RegistrationCategory(kVideo, L"KSCATEGORY_VIDEO");
    RegistrationCategory(kCapture, L"KSCATEGORY_CAPTURE");
    RegistrationCategory(kCamera, L"KSCATEGORY_VIDEO_CAMERA");
    RegistrationCategory(kSensor, L"KSCATEGORY_SENSOR_CAMERA");
}

// ---------------------------------------------------------------------------
// 2. Privacy

static void PrivacyValue(HKEY root, const wchar_t* rootName, const wchar_t* sub)
{
    wchar_t v[32] = L"";
    DWORD size = sizeof(v);
    LSTATUS rs = RegGetValueW(root, sub, L"Value", RRF_RT_REG_SZ, nullptr, v, &size);
    Out(L"  %ls\\...\\%ls: %ls", rootName, wcsrchr(sub, L'\\') + 1, rs == ERROR_SUCCESS ? v : L"(not set)");
}

static void Privacy()
{
    Out(L"");
    Out(L"[2] Camera privacy (Allow = programs may use the camera)");
    const wchar_t* base = L"Software\\Microsoft\\Windows\\CurrentVersion\\CapabilityAccessManager\\ConsentStore\\webcam";
    wchar_t nonPackaged[300];
    _snwprintf(nonPackaged, 300, L"%ls\\NonPackaged", base);
    PrivacyValue(HKEY_LOCAL_MACHINE, L"HKLM", base);
    PrivacyValue(HKEY_CURRENT_USER, L"HKCU", base);
    PrivacyValue(HKEY_CURRENT_USER, L"HKCU (desktop apps)", nonPackaged);
}

// ---------------------------------------------------------------------------
// 3. DirectShow

static const CLSID kNullRenderer = { 0xC1F400A4, 0x3F08, 0x11d3, { 0x9F, 0x0B, 0x00, 0x60, 0x08, 0x03, 0x9E, 0x37 } };

static void FreeMt(AM_MEDIA_TYPE* mt)
{
    if (!mt) return;
    if (mt->cbFormat) CoTaskMemFree(mt->pbFormat);
    if (mt->pUnk) mt->pUnk->Release();
    CoTaskMemFree(mt);
}

static void DescribeMt(const AM_MEDIA_TYPE* mt, wchar_t* out, size_t len)
{
    wchar_t sub[48];
    Fourcc(mt->subtype, sub);
    LONG w = 0, h = 0;
    REFERENCE_TIME avg = 0;
    if (IsEqualGUID(mt->formattype, FORMAT_VideoInfo) && mt->cbFormat >= sizeof(VIDEOINFOHEADER))
    {
        auto* v = (VIDEOINFOHEADER*)mt->pbFormat;
        w = v->bmiHeader.biWidth; h = v->bmiHeader.biHeight; avg = v->AvgTimePerFrame;
    }
    else if (IsEqualGUID(mt->formattype, FORMAT_VideoInfo2) && mt->cbFormat >= sizeof(VIDEOINFOHEADER2))
    {
        auto* v = (VIDEOINFOHEADER2*)mt->pbFormat;
        w = v->bmiHeader.biWidth; h = v->bmiHeader.biHeight; avg = v->AvgTimePerFrame;
    }
    _snwprintf(out, len, L"%ls %ldx%ld %.2f fps%ls", sub, w, h, avg ? 10000000.0 / avg : 0.0,
               IsEqualGUID(mt->formattype, FORMAT_VideoInfo2) ? L" (VIDEOINFOHEADER2)" : L"");
    out[len - 1] = 0;
}

// What a program does when it wants a size: the first offered format of that size, else the first format changed
// to that size (many programs do the latter and give up if the camera refuses).
static void TrySizes(IAMStreamConfig* cfg)
{
    static const LONG sizes[][2] = { { 640, 480 }, { 1280, 720 }, { 1920, 1080 }, { 320, 240 } };
    int count = 0, size = 0;
    if (FAILED(cfg->GetNumberOfCapabilities(&count, &size)) || count <= 0 || size > 4096) return;
    BYTE scc[4096];
    for (auto& s : sizes)
    {
        AM_MEDIA_TYPE* pick = nullptr;
        for (int i = 0; i < count && !pick; i++)
        {
            AM_MEDIA_TYPE* mt = nullptr;
            if (FAILED(cfg->GetStreamCaps(i, &mt, scc)) || !mt) continue;
            if (IsEqualGUID(mt->formattype, FORMAT_VideoInfo) && mt->cbFormat >= sizeof(VIDEOINFOHEADER) &&
                ((VIDEOINFOHEADER*)mt->pbFormat)->bmiHeader.biWidth == s[0] &&
                labs(((VIDEOINFOHEADER*)mt->pbFormat)->bmiHeader.biHeight) == s[1])
                pick = mt;
            else
                FreeMt(mt);
        }
        bool offered = pick != nullptr;
        if (!pick && SUCCEEDED(cfg->GetStreamCaps(0, &pick, scc)) && pick && IsEqualGUID(pick->formattype, FORMAT_VideoInfo))
        {
            auto* v = (VIDEOINFOHEADER*)pick->pbFormat;
            v->bmiHeader.biWidth = s[0];
            v->bmiHeader.biHeight = v->bmiHeader.biHeight < 0 ? -s[1] : s[1];
            v->bmiHeader.biSizeImage = (DWORD)(s[0] * s[1] * (v->bmiHeader.biBitCount ? v->bmiHeader.biBitCount : 16) / 8);
            pick->lSampleSize = v->bmiHeader.biSizeImage;
        }
        if (pick)
        {
            HRESULT hr = cfg->SetFormat(pick);
            Out(L"      SetFormat %ldx%ld (%ls): 0x%08lX%ls", s[0], s[1], offered ? L"offered" : L"not offered, first format resized",
                (unsigned long)hr, SUCCEEDED(hr) ? L" ok" : L" REFUSED");
            FreeMt(pick);
        }
    }
    // back to the camera's default
    AM_MEDIA_TYPE* first = nullptr;
    if (SUCCEEDED(cfg->GetStreamCaps(0, &first, scc)) && first)
    {
        cfg->SetFormat(first);
        FreeMt(first);
    }
}

// Runs capture -> Null Renderer for 3 s (as a program that shows or records the camera). For a Show2Cam camera the
// driver's frame counter tells how many frames were delivered.
static void RunGraph(IBaseFilter* cam, const wchar_t* name, const wchar_t* devicePath)
{
    IGraphBuilder* graph = nullptr;
    ICaptureGraphBuilder2* cgb = nullptr;
    IBaseFilter* nullr = nullptr;
    IMediaControl* mc = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph));
    if (SUCCEEDED(hr)) hr = CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cgb));
    if (SUCCEEDED(hr)) hr = cgb->SetFiltergraph(graph);
    if (SUCCEEDED(hr)) hr = graph->AddFilter(cam, name);
    HRESULT nr = CoCreateInstance(kNullRenderer, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&nullr));
    if (FAILED(nr)) Out(L"      Null Renderer (qedit.dll) not available: 0x%08lX, the graph test uses the default renderer", (unsigned long)nr);
    if (SUCCEEDED(hr) && nullr) hr = graph->AddFilter(nullr, L"Null Renderer");
    if (SUCCEEDED(hr))
    {
        hr = cgb->RenderStream(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, cam, nullptr, nullr);
        Out(L"      RenderStream (capture pin): 0x%08lX%ls", (unsigned long)hr, SUCCEEDED(hr) ? L" ok" : L" FAILED");
    }
    // Show2Cam: the driver's own count of delivered frames
    HANDLE s2c = INVALID_HANDLE_VALUE;
    S2C_STATUS before = {}, after = {};
    if (devicePath && wcsstr(devicePath, L"\\camera"))
    {
        CamInfo cams[S2C_MAX_CAMERAS_UI];
        int n = CamList(cams, S2C_MAX_CAMERAS_UI);
        for (int i = 0; i < n; i++)
            if (!_wcsicmp(cams[i].path, devicePath) || !_wcsicmp(wcsrchr(cams[i].path, L'\\'), wcsrchr(devicePath, L'\\')))
            {
                s2c = CamOpen(cams[i].path);
                break;
            }
        if (s2c != INVALID_HANDLE_VALUE) CamGetStatus(s2c, &before);
    }
    if (SUCCEEDED(hr)) hr = graph->QueryInterface(IID_PPV_ARGS(&mc));
    if (SUCCEEDED(hr))
    {
        hr = mc->Run();
        OAFilterState st = State_Stopped;
        HRESULT gs = mc->GetState(3000, &st);
        Sleep(3000);
        Out(L"      Run: 0x%08lX, state %d (2 = running), GetState 0x%08lX", (unsigned long)hr, (int)st, (unsigned long)gs);
        mc->Stop();
    }
    if (s2c != INVALID_HANDLE_VALUE)
    {
        CamGetStatus(s2c, &after);
        Out(L"      Show2Cam driver: %llu frames delivered in 3 s (dropped %llu), %lux%lu %lu fps", after.FramesDelivered - before.FramesDelivered,
            after.FramesDropped - before.FramesDropped, after.Width, after.Height, after.Fps);
        CloseHandle(s2c);
    }
    if (mc) mc->Release();
    if (nullr) { graph->RemoveFilter(nullr); nullr->Release(); }
    if (graph) graph->RemoveFilter(cam);
    if (cgb) cgb->Release();
    if (graph) graph->Release();
}

static void DirectShow(bool run)
{
    Out(L"");
    Out(L"[3] DirectShow \"Video Capture Sources\" (%ls)", Bitness());
    ICreateDevEnum* de = nullptr;
    IEnumMoniker* em = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&de));
    if (SUCCEEDED(hr)) hr = de->CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &em, 0);
    if (hr != S_OK)
    {
        Out(L"  no video capture sources (0x%08lX)", (unsigned long)hr);
        if (de) de->Release();
        return;
    }
    IMoniker* m = nullptr;
    int index = 0;
    while (em->Next(1, &m, nullptr) == S_OK)
    {
        IPropertyBag* bag = nullptr;
        wchar_t name[256] = L"?", path[512] = L"", clsid[64] = L"";
        LONG filterData = -1;
        if (SUCCEEDED(m->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&bag))))
        {
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(bag->Read(L"FriendlyName", &v, nullptr)) && v.vt == VT_BSTR) wcsncpy(name, v.bstrVal, 255);
            VariantClear(&v);
            if (SUCCEEDED(bag->Read(L"DevicePath", &v, nullptr)) && v.vt == VT_BSTR) wcsncpy(path, v.bstrVal, 511);
            VariantClear(&v);
            if (SUCCEEDED(bag->Read(L"CLSID", &v, nullptr)) && v.vt == VT_BSTR) wcsncpy(clsid, v.bstrVal, 63);
            VariantClear(&v);
            if (SUCCEEDED(bag->Read(L"FilterData", &v, nullptr)) && (v.vt & VT_ARRAY) && v.parray)
            {
                LONG lo = 0, hi = -1;
                SafeArrayGetLBound(v.parray, 1, &lo);
                SafeArrayGetUBound(v.parray, 1, &hi);
                filterData = hi - lo + 1;
            }
            VariantClear(&v);
            bag->Release();
        }
        Out(L"  #%d \"%ls\"", index++, name);
        Out(L"      DevicePath %ls", path[0] ? path : L"(none: a user-mode / software camera)");
        Out(L"      CLSID %ls, FilterData %ld bytes%ls", clsid[0] ? clsid : L"(none)", filterData < 0 ? 0 : filterData,
            filterData < 0 ? L" (MISSING)" : L"");
        IBaseFilter* f = nullptr;
        hr = m->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&f));
        Out(L"      open (BindToObject): 0x%08lX%ls", (unsigned long)hr, SUCCEEDED(hr) ? L" ok" : L" FAILED");
        if (SUCCEEDED(hr))
        {
            // pins
            IEnumPins* ep = nullptr;
            IPin* p = nullptr;
            if (SUCCEEDED(f->EnumPins(&ep)))
            {
                while (ep->Next(1, &p, nullptr) == S_OK)
                {
                    PIN_INFO pi = {};
                    p->QueryPinInfo(&pi);
                    GUID cat = GUID_NULL;
                    IKsPropertySet* ks = nullptr;
                    DWORD got = 0;
                    if (SUCCEEDED(p->QueryInterface(IID_PPV_ARGS(&ks))))
                    {
                        ks->Get(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY, nullptr, 0, &cat, sizeof(cat), &got);
                        ks->Release();
                    }
                    const wchar_t* c = IsEqualGUID(cat, PIN_CATEGORY_CAPTURE) ? L"capture" : IsEqualGUID(cat, PIN_CATEGORY_PREVIEW) ? L"preview"
                                      : IsEqualGUID(cat, PIN_CATEGORY_STILL) ? L"still" : L"other";
                    Out(L"      pin \"%ls\" %ls, category %ls", pi.achName, pi.dir == PINDIR_OUTPUT ? L"out" : L"in", c);
                    if (pi.pFilter) pi.pFilter->Release();
                    p->Release();
                }
                ep->Release();
            }
            // formats
            ICaptureGraphBuilder2* cgb = nullptr;
            IAMStreamConfig* cfg = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_CaptureGraphBuilder2, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&cgb))))
            {
                IGraphBuilder* g = nullptr;
                CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g));
                if (g) { cgb->SetFiltergraph(g); g->AddFilter(f, L"cam"); }
                hr = cgb->FindInterface(&PIN_CATEGORY_CAPTURE, &MEDIATYPE_Video, f, IID_IAMStreamConfig, (void**)&cfg);
                if (FAILED(hr)) Out(L"      IAMStreamConfig on the capture pin: 0x%08lX (MISSING)", (unsigned long)hr);
                if (cfg)
                {
                    int count = 0, size = 0;
                    cfg->GetNumberOfCapabilities(&count, &size);
                    AM_MEDIA_TYPE* cur = nullptr;
                    wchar_t t[160];
                    if (SUCCEEDED(cfg->GetFormat(&cur)) && cur) { DescribeMt(cur, t, 160); Out(L"      current format: %ls", t); FreeMt(cur); }
                    Out(L"      formats offered: %d", count);
                    BYTE scc[4096];
                    for (int i = 0; i < count && i < 60 && size <= 4096; i++)
                    {
                        AM_MEDIA_TYPE* mt = nullptr;
                        if (SUCCEEDED(cfg->GetStreamCaps(i, &mt, scc)) && mt)
                        {
                            DescribeMt(mt, t, 160);
                            auto* caps = (VIDEO_STREAM_CONFIG_CAPS*)scc;
                            Out(L"        %2d: %ls (frame rate %.2f..%.2f)", i, t, caps->MaxFrameInterval ? 10000000.0 / caps->MaxFrameInterval : 0.0,
                                caps->MinFrameInterval ? 10000000.0 / caps->MinFrameInterval : 0.0);
                            FreeMt(mt);
                        }
                    }
                    if (run) TrySizes(cfg);
                    cfg->Release();
                }
                if (g) { g->RemoveFilter(f); g->Release(); }
                cgb->Release();
            }
            if (run) RunGraph(f, name, path);
            f->Release();
        }
        m->Release();
    }
    em->Release();
    de->Release();
}

// ---------------------------------------------------------------------------
// 4. Media Foundation

// What a program using OpenCV (default backend: Media Foundation) gets: the camera opened at 640x480 with the
// reader converting to RGB32, frames read for 3 s; the last one is measured (black share, mean brightness) and saved
// as a BMP next to the log.
static wchar_t g_logDir[MAX_PATH];

static void Snapshot(IMFActivate* dev, UINT32 index, const wchar_t* name)
{
    DWORD t0 = GetTickCount();
    IMFMediaSource* src = nullptr;
    IMFSourceReader* rd = nullptr;
    IMFAttributes* attr = nullptr;
    HRESULT hr = dev->ActivateObject(IID_PPV_ARGS(&src));
    if (SUCCEEDED(hr)) hr = MFCreateAttributes(&attr, 1);
    if (SUCCEEDED(hr)) hr = attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromMediaSource(src, attr, &rd);
    if (attr) attr->Release();
    if (FAILED(hr))
    {
        Out(L"      snapshot: open 0x%08lX", (unsigned long)hr);
        if (src) { src->Shutdown(); src->Release(); }
        return;
    }
    // the native 640x480 format (YUY2 preferred), else the current one
    wchar_t chosen[48] = L"current";
    for (DWORD t = 0; t < 80; t++)
    {
        IMFMediaType* mt = nullptr;
        if (FAILED(rd->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, t, &mt))) break;
        UINT32 w = 0, h = 0;
        GUID sub = {};
        MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &w, &h);
        mt->GetGUID(MF_MT_SUBTYPE, &sub);
        bool take = w == 640 && h == 480 && (wcscmp(chosen, L"current") == 0 || IsEqualGUID(sub, MFVideoFormat_YUY2));
        if (take && SUCCEEDED(rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, mt)))
        {
            Fourcc(sub, chosen);
            wcscat(chosen, L" 640x480");
        }
        mt->Release();
    }
    IMFMediaType* out = nullptr;
    hr = MFCreateMediaType(&out);
    if (SUCCEEDED(hr)) out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(hr)) hr = rd->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out);
    if (out) out->Release();
    UINT32 w = 0, h = 0;
    LONG stride = 0;
    IMFMediaType* cur = nullptr;
    if (SUCCEEDED(rd->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur)))
    {
        MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
        stride = (LONG)MFGetAttributeUINT32(cur, MF_MT_DEFAULT_STRIDE, w * 4);
        cur->Release();
    }
    if ((stride < 0 ? -stride : stride) < (LONG)(w * 4)) stride = stride < 0 ? -(LONG)(w * 4) : (LONG)(w * 4);
    if (FAILED(hr) || !w || !h)
    {
        Out(L"      snapshot: RGB32 output 0x%08lX", (unsigned long)hr);
        rd->Release();
        src->Shutdown();
        src->Release();
        return;
    }
    int frames = 0;
    DWORD first = 0;
    IMFSample* last = nullptr;
    HRESULT rhr = S_OK;
    DWORD start = GetTickCount();
    while (GetTickCount() - start < 3000 + (first ? 0 : 30000))
    {
        DWORD stream = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample* smp = nullptr;
        rhr = rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &ts, &smp);
        if (smp)
        {
            if (!first) { first = GetTickCount() - t0; start = GetTickCount(); }
            frames++;
            if (last) last->Release();
            last = smp;
        }
        if (FAILED(rhr) || (flags & MF_SOURCE_READERF_ERROR)) break;
    }
    if (!last)
    {
        Out(L"      snapshot %ls: NO FRAME (0x%08lX)", chosen, (unsigned long)rhr);
    }
    else
    {
        IMFMediaBuffer* buf = nullptr;
        IMF2DBuffer* b2 = nullptr;
        BYTE* top = nullptr;
        LONG pitch = 0;
        BYTE* p = nullptr;
        bool locked2d = false;
        if (SUCCEEDED(last->ConvertToContiguousBuffer(&buf)))
        {
            if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&top, &pitch))) locked2d = true;
            else if (SUCCEEDED(buf->Lock(&p, nullptr, nullptr)))
            {
                pitch = stride;
                top = stride < 0 ? p + (SIZE_T)(h - 1) * (SIZE_T)(-stride) : p;
            }
        }
        if (top)
        {
            ULONGLONG black = 0, sum = 0;
            for (UINT32 y = 0; y < h; y++)
            {
                const BYTE* row = top + (LONG_PTR)y * pitch;
                for (UINT32 x = 0; x < w; x++)
                {
                    int v = (row[x * 4] + row[x * 4 + 1] + row[x * 4 + 2]) / 3;
                    sum += (ULONGLONG)v;
                    if (v < 8) black++;
                }
            }
            // the picture as a BMP (bottom-up rows)
            wchar_t file[MAX_PATH], safe[64];
            int k = 0;
            for (const wchar_t* c = name; *c && k < 60; c++) safe[k++] = (iswalnum(*c) ? *c : L'_');
            safe[k] = 0;
            _snwprintf(file, MAX_PATH, L"%ls\\snap-%u-%ls.bmp", g_logDir, index, safe);
            file[MAX_PATH - 1] = 0;
            FILE* f = _wfopen(file, L"wb");
            if (f)
            {
                BITMAPFILEHEADER fh = {};
                BITMAPINFOHEADER ih = {};
                ih.biSize = sizeof(ih);
                ih.biWidth = (LONG)w;
                ih.biHeight = (LONG)h;
                ih.biPlanes = 1;
                ih.biBitCount = 32;
                ih.biSizeImage = w * h * 4;
                fh.bfType = 0x4D42;
                fh.bfOffBits = sizeof(fh) + sizeof(ih);
                fh.bfSize = fh.bfOffBits + ih.biSizeImage;
                fwrite(&fh, sizeof(fh), 1, f);
                fwrite(&ih, sizeof(ih), 1, f);
                for (UINT32 y = h; y-- > 0;) fwrite(top + (LONG_PTR)y * pitch, 1, w * 4, f);
                fclose(f);
            }
            Out(L"      snapshot %ls -> RGB32 %ux%u: first frame %lu ms after opening, %d frames in 3 s, black %.1f %%, mean %.0f, %ls",
                chosen, w, h, first, frames, 100.0 * black / ((double)w * h), (double)sum / ((double)w * h), f ? file : L"(not saved)");
            if (locked2d) b2->Unlock2D();
            else buf->Unlock();
        }
        if (b2) b2->Release();
        if (buf) buf->Release();
        last->Release();
    }
    rd->Release();
    src->Shutdown();
    src->Release();
}

static void MediaFoundation(bool run)
{
    Out(L"");
    Out(L"[4] Media Foundation cameras (%ls)", Bitness());
    IMFAttributes* attr = nullptr;
    IMFActivate** devs = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFCreateAttributes(&attr, 1);
    if (SUCCEEDED(hr)) hr = attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (SUCCEEDED(hr)) hr = MFEnumDeviceSources(attr, &devs, &count);
    if (attr) attr->Release();
    if (FAILED(hr)) { Out(L"  MFEnumDeviceSources: 0x%08lX", (unsigned long)hr); return; }
    if (!count) Out(L"  (none)");
    for (UINT32 i = 0; i < count; i++)
    {
        WCHAR* name = nullptr;
        WCHAR* link = nullptr;
        UINT32 len = 0;
        devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &name, &len);
        devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len);
        Out(L"  #%u \"%ls\"", i, name ? name : L"?");
        Out(L"      %ls", link ? link : L"(no symbolic link)");
        if (run)
        {
            IMFMediaSource* src = nullptr;
            IMFSourceReader* rd = nullptr;
            hr = devs[i]->ActivateObject(IID_PPV_ARGS(&src));
            if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromMediaSource(src, nullptr, &rd);
            Out(L"      open: 0x%08lX%ls", (unsigned long)hr, SUCCEEDED(hr) ? L" ok" : L" FAILED");
            if (SUCCEEDED(hr))
            {
                for (DWORD t = 0; t < 40; t++)
                {
                    IMFMediaType* mt = nullptr;
                    if (FAILED(rd->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, t, &mt))) break;
                    GUID sub = {};
                    UINT32 w = 0, h = 0, n = 0, d = 0;
                    mt->GetGUID(MF_MT_SUBTYPE, &sub);
                    MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &w, &h);
                    MFGetAttributeRatio(mt, MF_MT_FRAME_RATE, &n, &d);
                    wchar_t s[48];
                    Fourcc(sub, s);
                    Out(L"        %2lu: %ls %ux%u %.2f fps", t, s, w, h, d ? (double)n / d : 0.0);
                    mt->Release();
                }
                int frames = 0;
                HRESULT rhr = S_OK;
                DWORD start = GetTickCount();
                while (frames < 15 && GetTickCount() - start < 5000)
                {
                    DWORD stream = 0, flags = 0;
                    LONGLONG ts = 0;
                    IMFSample* smp = nullptr;
                    rhr = rd->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &ts, &smp);
                    if (smp) { frames++; smp->Release(); }
                    if (FAILED(rhr) || (flags & MF_SOURCE_READERF_ERROR)) break;
                }
                Out(L"      read: %d frames in %lu ms (last 0x%08lX)%ls", frames, GetTickCount() - start, (unsigned long)rhr,
                    frames ? L"" : L" NO FRAMES");
                rd->Release();
            }
            if (src) { src->Shutdown(); src->Release(); }
            Snapshot(devs[i], i, name ? name : L"camera");
        }
        CoTaskMemFree(name);
        CoTaskMemFree(link);
        devs[i]->Release();
    }
    CoTaskMemFree(devs);
}

// ---------------------------------------------------------------------------
// 5. Video for Windows

static void Vfw()
{
    Out(L"");
    Out(L"[5] Video for Windows drivers (old programs see only these)");
    int n = 0;
    for (WORD i = 0; i < 10; i++)
    {
        wchar_t name[128], ver[128];
        if (capGetDriverDescriptionW(i, name, 128, ver, 128))
        {
            Out(L"  %u: %ls (%ls)", i, name, ver);
            n++;
        }
    }
    if (!n) Out(L"  (none)");
}

// ---------------------------------------------------------------------------

int wmain(int argc, wchar_t** argv)
{
    bool run = true;
    for (int i = 1; i < argc; i++)
        if (!_wcsicmp(argv[i], L"--no-run")) run = false;
    wchar_t path[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, path)))
    {
        wcscat(path, L"\\Show2Cam");
        CreateDirectoryW(path, nullptr);
        wcscat(path, L"\\logs");
        CreateDirectoryW(path, nullptr);
        wcscpy(g_logDir, path);
        wcscat(path, Bitness()[1] == L'6' ? L"\\camdiag-x64.log" : L"\\camdiag-x86.log");
        g_log = _wfopen(path, L"w, ccs=UTF-8");
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    OSVERSIONINFOW os = { sizeof(os) };
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW*);
    auto rgv = (RtlGetVersionFn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (rgv) rgv(&os);
    Out(L"Show2Cam camera diagnostics %ls (%ls), %04u-%02u-%02u %02u:%02u, Windows %lu.%lu.%lu", L"" S2C_VER_STR, Bitness(), t.wYear,
        t.wMonth, t.wDay, t.wHour, t.wMinute, os.dwMajorVersion, os.dwMinorVersion, os.dwBuildNumber);
    if (g_log) Out(L"log: %ls", path);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);     // as most programs that use DirectShow
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    Registration();
    Privacy();
    DirectShow(run);
    MediaFoundation(run);
    Vfw();
    MFShutdown();
    CoUninitialize();
    Out(L"");
    Out(L"done");
    if (g_log) fclose(g_log);
    return 0;
}
