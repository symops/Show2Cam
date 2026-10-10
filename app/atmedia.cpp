// Test media of s2cautotest (see atmedia.h).
#include <winsock2.h>
#include <ws2tcpip.h>
#include "atmedia.h"
#include "applog.h"
#include <wincodec.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <shellapi.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

const ULONG kTestColors[8] = { 0xCC3333, 0x33CC33, 0x3333CC, 0xCCCC33, 0xCC33CC, 0x33CCCC, 0xFF9900, 0x996633 };

static void LogF(TestLog log, const wchar_t* fmt, ...)
{
    wchar_t buf[600];
    va_list a;
    va_start(a, fmt);
    _vsnwprintf(buf, 600, fmt, a);
    va_end(a);
    buf[599] = 0;
    if (log) log(buf);
    else AppLog(L"%ls", buf);
}

// ---------------------------------------------------------------------------
// Pictures

// A test picture: the colour, a checkered border (so it is not "even") and a white band at the top; the centre is
// plain colour (what the checks look at).
static void DrawPicture(ULONG* px, int w, int h, ULONG color)
{
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            ULONG c = color;
            bool border = x < w / 10 || x >= w - w / 10 || y < h / 10 || y >= h - h / 10;
            if (border && ((x / 16 + y / 16) & 1)) c = 0x000000;
            if (y >= h / 10 && y < h / 10 + 12) c = 0xFFFFFF;
            px[y * w + x] = 0xFF000000u | c;
        }
}

static HRESULT EncodePicture(IWICImagingFactory* wic, const ULONG* px, int w, int h, const GUID& container, IStream* out)
{
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* props = nullptr;
    IWICBitmap* bmp = nullptr;
    IWICFormatConverter* conv = nullptr;
    IWICPalette* pal = nullptr;
    HRESULT hr = wic->CreateEncoder(container, nullptr, &enc);
    if (SUCCEEDED(hr)) hr = enc->Initialize(out, WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, &props);
    if (SUCCEEDED(hr)) hr = frame->Initialize(props);
    if (SUCCEEDED(hr)) hr = frame->SetSize((UINT)w, (UINT)h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr))
        hr = wic->CreateBitmapFromMemory((UINT)w, (UINT)h, GUID_WICPixelFormat32bppBGRA, (UINT)w * 4, (UINT)w * h * 4, (BYTE*)px, &bmp);
    if (SUCCEEDED(hr) && !IsEqualGUID(fmt, GUID_WICPixelFormat32bppBGRA))
    {
        // the encoder wants another format (GIF: 8-bit indexed): converted, with the web palette for indexed ones
        bool indexed = IsEqualGUID(fmt, GUID_WICPixelFormat8bppIndexed) || IsEqualGUID(fmt, GUID_WICPixelFormat4bppIndexed) ||
                       IsEqualGUID(fmt, GUID_WICPixelFormat2bppIndexed) || IsEqualGUID(fmt, GUID_WICPixelFormat1bppIndexed);
        if (indexed)
        {
            hr = wic->CreatePalette(&pal);
            if (SUCCEEDED(hr)) hr = pal->InitializePredefined(WICBitmapPaletteTypeFixedWebPalette, FALSE);
            if (SUCCEEDED(hr)) hr = frame->SetPalette(pal);
        }
        if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&conv);
        if (SUCCEEDED(hr))
            hr = conv->Initialize(bmp, fmt, WICBitmapDitherTypeNone, pal, 0.0,
                                  indexed ? WICBitmapPaletteTypeFixedWebPalette : WICBitmapPaletteTypeCustom);
        if (SUCCEEDED(hr)) hr = frame->WriteSource(conv, nullptr);
    }
    else if (SUCCEEDED(hr))
        hr = frame->WriteSource(bmp, nullptr);
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    if (pal) pal->Release();
    if (conv) conv->Release();
    if (bmp) bmp->Release();
    if (props) props->Release();
    if (frame) frame->Release();
    if (enc) enc->Release();
    return hr;
}

static HRESULT WritePictureFile(IWICImagingFactory* wic, const wchar_t* path, ULONG color, const GUID& container)
{
    ULONG* px = (ULONG*)malloc((size_t)kTestW * kTestH * 4);
    if (!px) return E_OUTOFMEMORY;
    DrawPicture(px, kTestW, kTestH, color);
    IWICStream* s = nullptr;
    HRESULT hr = wic->CreateStream(&s);
    if (SUCCEEDED(hr)) hr = s->InitializeFromFilename(path, GENERIC_WRITE);
    if (SUCCEEDED(hr)) hr = EncodePicture(wic, px, kTestW, kTestH, container, s);
    if (s) s->Release();
    free(px);
    if (FAILED(hr)) DeleteFileW(path);
    return hr;
}

// A JPEG of a picture in memory (malloc'd).
static HRESULT JpegInMemory(IWICImagingFactory* wic, ULONG color, BYTE** data, DWORD* len)
{
    *data = nullptr;
    *len = 0;
    ULONG* px = (ULONG*)malloc((size_t)kTestW * kTestH * 4);
    if (!px) return E_OUTOFMEMORY;
    DrawPicture(px, kTestW, kTestH, color);
    IStream* s = nullptr;
    HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &s);
    if (SUCCEEDED(hr)) hr = EncodePicture(wic, px, kTestW, kTestH, GUID_ContainerFormatJpeg, s);
    free(px);
    HGLOBAL g = nullptr;
    if (SUCCEEDED(hr)) hr = GetHGlobalFromStream(s, &g);
    if (SUCCEEDED(hr))
    {
        STATSTG st = {};
        s->Stat(&st, STATFLAG_NONAME);
        *len = (DWORD)st.cbSize.QuadPart;
        *data = (BYTE*)malloc(*len);
        void* p = GlobalLock(g);
        if (*data && p) memcpy(*data, p, *len);
        GlobalUnlock(g);
        if (!*data) hr = E_OUTOFMEMORY;
    }
    if (s) s->Release();
    return hr;
}

// ---------------------------------------------------------------------------
// Video clips (Media Foundation sink writer)

ULONG TestClipColorAt(double seconds)
{
    int i = (int)(seconds / 0.5);
    if (i < 0) i = 0;
    return kTestColors[i % 4];
}

// The clip's frame `n` as NV12: the colour field and a white square moving along the top.
static void ClipFrameNV12(BYTE* nv12, int n, int fps)
{
    ULONG c = TestClipColorAt((double)n / fps);
    int R = (c >> 16) & 255, G = (c >> 8) & 255, B = c & 255;
    BYTE Y = (BYTE)(16 + ((66 * R + 129 * G + 25 * B + 128) >> 8));
    BYTE U = (BYTE)(128 + ((-38 * R - 74 * G + 112 * B + 128) >> 8));
    BYTE V = (BYTE)(128 + ((112 * R - 94 * G - 18 * B + 128) >> 8));
    memset(nv12, Y, (size_t)kTestW * kTestH);
    int sx = (n * 16) % (kTestW - 64), sy = 32;
    for (int y = sy; y < sy + 64; y++) memset(nv12 + (size_t)y * kTestW + sx, 235, 64);
    BYTE* uv = nv12 + (size_t)kTestW * kTestH;
    for (int y = 0; y < kTestH / 2; y++)
        for (int x = 0; x < kTestW / 2; x++)
        {
            bool sq = x * 2 >= sx && x * 2 < sx + 64 && y * 2 >= sy && y * 2 < sy + 64;
            uv[(size_t)y * kTestW + x * 2] = sq ? 128 : U;
            uv[(size_t)y * kTestW + x * 2 + 1] = sq ? 128 : V;
        }
}

static HRESULT AudioOutputType(const GUID& subtype, UINT32 channels, IMFMediaType** out, UINT32* rate)
{
    *out = nullptr;
    if (IsEqualGUID(subtype, MFAudioFormat_AAC))
    {
        IMFMediaType* t = nullptr;
        HRESULT hr = MFCreateMediaType(&t);
        if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
        if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);
        if (FAILED(hr) && t) t->Release();
        else *out = t;
        *rate = 48000;
        return hr;
    }
    // WMA: one of the encoder's own types
    IMFCollection* types = nullptr;
    HRESULT hr = MFTranscodeGetAudioOutputAvailableTypes(subtype, MFT_ENUM_FLAG_ALL & ~MFT_ENUM_FLAG_FIELDOFUSE, nullptr, &types);
    DWORD n = 0;
    if (SUCCEEDED(hr)) types->GetElementCount(&n);
    hr = MF_E_INVALIDMEDIATYPE;
    for (DWORD i = 0; i < n && !*out; i++)
    {
        IUnknown* u = nullptr;
        IMFMediaType* t = nullptr;
        if (SUCCEEDED(types->GetElement(i, &u)) && SUCCEEDED(u->QueryInterface(IID_PPV_ARGS(&t))))
        {
            UINT32 ch = MFGetAttributeUINT32(t, MF_MT_AUDIO_NUM_CHANNELS, 0);
            UINT32 r = MFGetAttributeUINT32(t, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
            if (ch == channels && (r == 44100 || r == 48000))
            {
                *out = t;
                *rate = r;
                hr = S_OK;
                t = nullptr;
            }
        }
        if (t) t->Release();
        if (u) u->Release();
    }
    if (types) types->Release();
    return hr;
}

// A clip: kClipSeconds of kTestW x kTestH at 15 fps, the video codec `vsub`, sound `asub` (GUID_NULL: none).
static HRESULT WriteClip(const wchar_t* path, const GUID& vsub, const GUID& asub, UINT32 channels)
{
    const UINT32 fps = 15;
    IMFAttributes* wa = nullptr;
    IMFSinkWriter* w = nullptr;
    HRESULT hr = MFCreateAttributes(&wa, 1);
    if (SUCCEEDED(hr)) wa->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
    if (SUCCEEDED(hr)) hr = MFCreateSinkWriterFromURL(path, nullptr, wa, &w);
    if (wa) wa->Release();
    if (FAILED(hr)) return hr;

    DWORD vs = 0, as = (DWORD)-1;
    IMFMediaType* t = nullptr;
    auto videoType = [&](const GUID& sub, IMFMediaType** out) {
        IMFMediaType* m = nullptr;
        HRESULT r = MFCreateMediaType(&m);
        if (SUCCEEDED(r)) r = m->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(r)) r = m->SetGUID(MF_MT_SUBTYPE, sub);
        if (SUCCEEDED(r)) r = m->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (SUCCEEDED(r)) r = MFSetAttributeSize(m, MF_MT_FRAME_SIZE, kTestW, kTestH);
        if (SUCCEEDED(r)) r = MFSetAttributeRatio(m, MF_MT_FRAME_RATE, fps, 1);
        if (SUCCEEDED(r)) r = MFSetAttributeRatio(m, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (SUCCEEDED(r) && !IsEqualGUID(sub, MFVideoFormat_NV12)) r = m->SetUINT32(MF_MT_AVG_BITRATE, 2000000);
        if (FAILED(r) && m) m->Release();
        else *out = m;
        return r;
    };
    hr = videoType(vsub, &t);
    if (SUCCEEDED(hr)) hr = w->AddStream(t, &vs);
    if (t) t->Release();
    t = nullptr;
    if (SUCCEEDED(hr)) hr = videoType(MFVideoFormat_NV12, &t);
    if (SUCCEEDED(hr)) hr = w->SetInputMediaType(vs, t, nullptr);
    if (t) t->Release();
    t = nullptr;

    UINT32 rate = 48000;
    if (SUCCEEDED(hr) && !IsEqualGUID(asub, GUID_NULL))
    {
        hr = AudioOutputType(asub, channels, &t, &rate);
        if (SUCCEEDED(hr)) hr = w->AddStream(t, &as);
        if (t) t->Release();
        t = nullptr;
        if (SUCCEEDED(hr)) hr = MFCreateMediaType(&t);
        if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(hr)) hr = t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, rate);
        if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, channels);
        if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, channels * 2);
        if (SUCCEEDED(hr)) hr = t->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, rate * channels * 2);
        if (SUCCEEDED(hr)) hr = w->SetInputMediaType(as, t, nullptr);
        if (t) t->Release();
        t = nullptr;
    }
    if (SUCCEEDED(hr)) hr = w->BeginWriting();

    const UINT32 frames = (UINT32)(kClipSeconds * fps);
    const LONGLONG frameDur = 10000000 / fps;
    const UINT32 chunk = rate / 10;                       // 100 ms of sound per sample
    UINT32 audioPos = 0, audioTotal = (UINT32)(kClipSeconds * rate);
    DWORD nvSize = (DWORD)(kTestW * kTestH * 3 / 2);
    for (UINT32 n = 0; n < frames && SUCCEEDED(hr); n++)
    {
        LONGLONG vt = (LONGLONG)n * frameDur;
        // the sound up to this frame's end, in time order with the picture
        while (as != (DWORD)-1 && audioPos < audioTotal && (LONGLONG)audioPos * 10000000 / rate <= vt + frameDur && SUCCEEDED(hr))
        {
            UINT32 count = audioTotal - audioPos < chunk ? audioTotal - audioPos : chunk;
            IMFMediaBuffer* b = nullptr;
            IMFSample* s = nullptr;
            hr = MFCreateMemoryBuffer(count * channels * 2, &b);
            BYTE* p = nullptr;
            if (SUCCEEDED(hr)) hr = b->Lock(&p, nullptr, nullptr);
            if (SUCCEEDED(hr))
            {
                SHORT* q = (SHORT*)p;
                for (UINT32 i = 0; i < count; i++)
                {
                    SHORT v = (SHORT)(8000.0 * sin(2.0 * 3.14159265358979 * kToneHz * (audioPos + i) / rate));
                    for (UINT32 c = 0; c < channels; c++) *q++ = v;
                }
                b->Unlock();
                b->SetCurrentLength(count * channels * 2);
            }
            if (SUCCEEDED(hr)) hr = MFCreateSample(&s);
            if (SUCCEEDED(hr)) hr = s->AddBuffer(b);
            if (SUCCEEDED(hr)) hr = s->SetSampleTime((LONGLONG)audioPos * 10000000 / rate);
            if (SUCCEEDED(hr)) hr = s->SetSampleDuration((LONGLONG)count * 10000000 / rate);
            if (SUCCEEDED(hr)) hr = w->WriteSample(as, s);
            if (s) s->Release();
            if (b) b->Release();
            audioPos += count;
        }
        IMFMediaBuffer* b = nullptr;
        IMFSample* s = nullptr;
        hr = MFCreateMemoryBuffer(nvSize, &b);
        BYTE* p = nullptr;
        if (SUCCEEDED(hr)) hr = b->Lock(&p, nullptr, nullptr);
        if (SUCCEEDED(hr))
        {
            ClipFrameNV12(p, (int)n, (int)fps);
            b->Unlock();
            b->SetCurrentLength(nvSize);
        }
        if (SUCCEEDED(hr)) hr = MFCreateSample(&s);
        if (SUCCEEDED(hr)) hr = s->AddBuffer(b);
        if (SUCCEEDED(hr)) hr = s->SetSampleTime(vt);
        if (SUCCEEDED(hr)) hr = s->SetSampleDuration(frameDur);
        if (SUCCEEDED(hr)) hr = w->WriteSample(vs, s);
        if (s) s->Release();
        if (b) b->Release();
    }
    if (SUCCEEDED(hr)) hr = w->Finalize();
    w->Release();
    if (FAILED(hr)) DeleteFileW(path);
    return hr;
}

// ---------------------------------------------------------------------------
// The media folder

static void Join(wchar_t* out, const wchar_t* a, const wchar_t* b)
{
    _snwprintf(out, MAX_PATH, L"%ls\\%ls", a, b);
    out[MAX_PATH - 1] = 0;
}

// ---------------------------------------------------------------------------
// Samples built in as resources (RCDATA 500 = manifest, 501.. = the files in its order)

static bool ResourceToFile(int id, const wchar_t* path)
{
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const void* p = g ? LockResource(g) : nullptr;
    DWORD size = r ? SizeofResource(nullptr, r) : 0;
    if (!p || !size) return false;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    bool ok = WriteFile(f, p, size, &n, nullptr) && n == size;
    CloseHandle(f);
    return ok;
}

static HRESULT ProbePicture(const wchar_t* path)
{
    IWICImagingFactory* wic = nullptr;
    IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr)) hr = wic->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &frame);
    // the pixels themselves (a HEIF decoder may describe the frame but not decode it without the HEVC extension)
    IWICFormatConverter* conv = nullptr;
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr)) hr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    UINT fw = 0, fh = 0;
    if (SUCCEEDED(hr)) hr = conv->GetSize(&fw, &fh);
    if (SUCCEEDED(hr) && fw && fh)
    {
        ULONG* row = (ULONG*)malloc((size_t)fw * 4);
        WICRect rc = { 0, (INT)(fh / 2), (INT)fw, 1 };
        hr = row ? conv->CopyPixels(&rc, fw * 4, fw * 4, (BYTE*)row) : E_OUTOFMEMORY;
        free(row);
    }
    if (conv) conv->Release();
    if (frame) frame->Release();
    if (dec) dec->Release();
    if (wic) wic->Release();
    return hr;
}

// One picture decoded by Media Foundation (as the camera's video source does).
static HRESULT ProbeVideo(const wchar_t* path)
{
    IMFAttributes* a = nullptr;
    IMFSourceReader* r = nullptr;
    HRESULT hr = MFCreateAttributes(&a, 1);
    if (SUCCEEDED(hr)) a->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromURL(path, a, &r);
    if (a) a->Release();
    IMFMediaType* t = nullptr;
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&t);
    if (SUCCEEDED(hr)) t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) t->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(hr)) hr = r->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, t);
    if (t) t->Release();
    for (int i = 0; SUCCEEDED(hr) && i < 20; i++)
    {
        DWORD s = 0, flags = 0;
        LONGLONG ts = 0;
        IMFSample* sample = nullptr;
        hr = r->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &s, &flags, &ts, &sample);
        if (SUCCEEDED(hr) && (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM))) hr = MF_E_INVALID_STREAM_DATA;
        if (sample)
        {
            sample->Release();
            break;
        }
    }
    if (r) r->Release();
    return hr;
}

// Whether Windows decodes the clip's sound track (Windows 11 24H2 dropped the AC-3 decoder: 0xC00D36B4).
static HRESULT ProbeSound(const wchar_t* path)
{
    IMFSourceReader* r = nullptr;
    HRESULT hr = MFCreateSourceReaderFromURL(path, nullptr, &r);
    IMFMediaType* t = nullptr;
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&t);
    if (SUCCEEDED(hr)) t->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    if (SUCCEEDED(hr)) t->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
    if (SUCCEEDED(hr)) hr = r->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, t);
    if (t) t->Release();
    if (r) r->Release();
    return hr;
}

static void AddSamples(TestMedia* m, TestLog log, TestLog warn)
{
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(500), (LPCWSTR)RT_RCDATA);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const char* text = g ? (const char*)LockResource(g) : nullptr;
    DWORD size = r ? SizeofResource(nullptr, r) : 0;
    if (!text || !size)
    {
        LogF(log, L"  no built-in samples");
        return;
    }
    int images = 0, videos = 0, missing = 0, brokenPictures = 0, brokenClips = 0;
    wchar_t goodClip[MAX_PATH] = L"", goodName[128] = L"", brokenFolder[MAX_PATH] = L"";
    const char* p = text;
    const char* end = text + size;
    for (int id = 501; p < end; id++)
    {
        const char* eol = p;
        while (eol < end && *eol != '\n') eol++;
        char line[300];
        size_t len = (size_t)(eol - p) < sizeof(line) - 1 ? (size_t)(eol - p) : sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = 0;
        p = eol + 1;
        if (len && line[len - 1] == '\r') line[--len] = 0;
        if (!len)
        {
            id--;
            continue;
        }
        char kind[16] = "", file[128] = "";
        unsigned a1 = 0, a2 = 0, a3 = 0;
        char* f1 = strchr(line, '|');
        char* f2 = f1 ? strchr(f1 + 1, '|') : nullptr;
        if (!f1 || !f2) continue;
        *f1 = *f2 = 0;
        strncpy(kind, line, 15);
        strncpy(file, f1 + 1, 127);
        wchar_t wfile[128], folder[MAX_PATH], path[MAX_PATH], sub[160];
        MultiByteToWideChar(CP_UTF8, 0, file, -1, wfile, 128);
        _snwprintf(sub, 160, L"sample-%ls", wfile);
        for (wchar_t* q = sub; *q; q++)
            if (*q == L'.') *q = L'-';
        bool brokenImage = !strcmp(kind, "broken-image"), brokenVideo = !strcmp(kind, "broken-video");
        if (brokenImage) wcscpy(folder, m->allImages);          // among the good pictures
        else if (brokenVideo)
        {
            Join(folder, m->root, L"videos-broken");          // broken clips + one good one
            if (!brokenClips) CreateDirectoryW(folder, nullptr);
        }
        else
        {
            Join(folder, m->root, sub);
            CreateDirectoryW(folder, nullptr);
        }
        Join(path, folder, wfile);
        if (!ResourceToFile(id, path))
        {
            LogF(log, L"  sample %ls not written", wfile);
            continue;
        }
        // A "broken" file Windows can still decode (a cut PNG is drawn as far as it goes) is not broken for this
        // system: left out, the sources have to skip only what Windows refuses.
        if ((brokenImage && SUCCEEDED(ProbePicture(path))) || (brokenVideo && SUCCEEDED(ProbeVideo(path))))
        {
            LogF(log, L"  broken sample %ls: Windows decodes it anyway - left out", wfile);
            DeleteFileW(path);
            continue;
        }
        if (brokenImage)
        {
            brokenPictures++;
            continue;
        }
        if (brokenVideo)
        {
            if (!brokenClips) wcscpy(brokenFolder, folder);
            brokenClips++;
            continue;
        }
        if (!strcmp(kind, "image") && m->imageCount < 16)
        {
            sscanf(f2 + 1, "%x", &a1);
            HRESULT hr = ProbePicture(path);
            if (FAILED(hr))
            {
                wchar_t t[300];
                _snwprintf(t, 300, L"sample picture %ls: Windows cannot decode it (0x%08lX; its codec comes with a Store "
                           L"extension): skipped", wfile, (unsigned long)hr);
                t[299] = 0;
                if (warn) warn(t);
                missing++;
                continue;
            }
            TestImage& img = m->images[m->imageCount++];
            wcscpy(img.folder, folder);
            _snwprintf(img.format, 40, L"%ls (sample)", wcsrchr(wfile, L'.') ? wcsrchr(wfile, L'.') + 1 : wfile);
            img.color = a1;
            images++;
        }
        else if (!strcmp(kind, "video") && m->videoCount < 48)
        {
            sscanf(f2 + 1, "%u|%u|%u", &a1, &a2, &a3);
            if (a3)
            {
                HRESULT hr = ProbeVideo(path);
                if (FAILED(hr))
                {
                    wchar_t t[300];
                    _snwprintf(t, 300, L"sample clip %ls: Windows cannot decode it (0x%08lX; its codec comes with a Store "
                               L"extension): skipped", wfile, (unsigned long)hr);
                    t[299] = 0;
                    if (warn) warn(t);
                    missing++;
                    continue;
                }
            }
            if ((!a3 && !goodClip[0]) || !_wcsicmp(wfile, L"mp4-h264-mp3.mp4"))
            {
                wcscpy(goodClip, path);
                wcsncpy(goodName, wfile, 127);
            }
            TestVideo& v = m->videos[m->videoCount++];
            wcscpy(v.folder, folder);
            wcsncpy(v.name, wfile, 63);
            v.name[63] = 0;
            v.minColors = (int)a1;
            v.sound = a2 != 0;
            HRESULT sh = v.sound ? ProbeSound(path) : S_OK;
            if (FAILED(sh))
            {
                wchar_t t[300];
                _snwprintf(t, 300, L"sample clip %ls: Windows cannot decode its sound (0x%08lX; Windows 11 24H2 has no AC-3 "
                           L"decoder): only its picture is checked", wfile, (unsigned long)sh);
                t[299] = 0;
                if (warn) warn(t);
                v.sound = false;
                v.soundless = true;
            }
            v.channels = 2;
            v.optional = a3 != 0;
            videos++;
        }
    }
    // The broken clips' folder gets one good clip: the source must skip the others and play it.
    if (brokenClips && goodClip[0] && m->videoCount < 48)
    {
        wchar_t copy[MAX_PATH];
        Join(copy, brokenFolder, goodName);
        if (CopyFileW(goodClip, copy, FALSE))
        {
            TestVideo& v = m->videos[m->videoCount++];
            wcscpy(v.folder, brokenFolder);
            _snwprintf(v.name, 64, L"%d broken clips + %ls", brokenClips, goodName);
            v.name[63] = 0;
            v.sound = true;
            v.channels = 2;
        }
    }
    LogF(log, L"  built-in samples: %d picture(s), %d clip(s), broken: %d picture(s) among the good ones, %d clip(s)%ls", images,
         videos, brokenPictures, brokenClips, missing ? L" (some skipped, see WARN)" : L"");
}

// Folders of earlier runs whose program no longer runs (closed with the window's close box: no time to clean up).
static void RemoveStaleFolders(const wchar_t* tmp, const wchar_t* prefix)
{
    wchar_t pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%ls\\%ls*", tmp, prefix);
    pattern[MAX_PATH - 1] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        DWORD pid = (DWORD)wcstoul(fd.cFileName + wcslen(prefix), nullptr, 10);
        if (!pid || pid == GetCurrentProcessId()) continue;
        HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, pid);
        bool alive = p && WaitForSingleObject(p, 0) == WAIT_TIMEOUT;
        if (p) CloseHandle(p);
        if (alive) continue;
        wchar_t from[MAX_PATH + 2] = {};
        _snwprintf(from, MAX_PATH, L"%ls\\%ls", tmp, fd.cFileName);
        SHFILEOPSTRUCTW op = {};
        op.wFunc = FO_DELETE;
        op.pFrom = from;
        op.fFlags = FOF_NO_UI;
        int rc = SHFileOperationW(&op);
        AppLog(L"left over from an earlier run: %ls removed (%d)", from, rc);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool TestMediaCreate(TestMedia* m, TestLog log, TestLog warn)
{
    ZeroMemory(m, sizeof(*m));
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH - 40, tmp);
    size_t tl = wcslen(tmp);
    if (tl && tmp[tl - 1] == L'\\') tmp[tl - 1] = 0;
    RemoveStaleFolders(tmp, L"s2cautotest-");
    _snwprintf(m->root, MAX_PATH, L"%ls\\s2cautotest-%lu", tmp, GetCurrentProcessId());
    m->root[MAX_PATH - 1] = 0;
    for (wchar_t* p = m->root; *p; p++)
        if (p[0] == L'\\' && p[1] == L'\\') memmove(p, p + 1, (wcslen(p + 1) + 1) * sizeof(wchar_t));
    if (!CreateDirectoryW(m->root, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
    {
        LogF(log, L"  test media: cannot create %ls (%lu)", m->root, GetLastError());
        return false;
    }
    LogF(log, L"Test media in %ls", m->root);

    // Pictures: one folder per format, and one folder with all of them, a broken picture and a text file.
    IWICImagingFactory* wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    struct { const wchar_t* name; const wchar_t* ext; GUID container; } kFormats[] = {
        { L"PNG", L"png", GUID_ContainerFormatPng }, { L"JPEG", L"jpg", GUID_ContainerFormatJpeg },
        { L"BMP", L"bmp", GUID_ContainerFormatBmp }, { L"GIF", L"gif", GUID_ContainerFormatGif },
        { L"TIFF", L"tif", GUID_ContainerFormatTiff }, { L"JPEG XR", L"jxr", GUID_ContainerFormatWmp },
    };
    Join(m->allImages, m->root, L"images-all");
    CreateDirectoryW(m->allImages, nullptr);
    int made = 0;
    for (int i = 0; wic && i < (int)(sizeof(kFormats) / sizeof(kFormats[0])); i++)
    {
        TestImage& img = m->images[m->imageCount];
        wchar_t sub[40], file[MAX_PATH], name[40];
        _snwprintf(sub, 40, L"image-%ls", kFormats[i].ext);
        Join(img.folder, m->root, sub);
        CreateDirectoryW(img.folder, nullptr);
        _snwprintf(name, 40, L"picture.%ls", kFormats[i].ext);
        Join(file, img.folder, name);
        img.color = kTestColors[i % 8];
        wcsncpy(img.format, kFormats[i].name, 39);
        HRESULT hr = WritePictureFile(wic, file, img.color, kFormats[i].container);
        if (FAILED(hr))
        {
            LogF(log, L"  test media: %ls picture not made (0x%08lX): skipped", kFormats[i].name, (unsigned long)hr);
            RemoveDirectoryW(img.folder);
            continue;
        }
        m->imageCount++;
        made++;
        _snwprintf(name, 40, L"all-%d.%ls", i, kFormats[i].ext);
        Join(file, m->allImages, name);
        WritePictureFile(wic, file, img.color, kFormats[i].container);
    }
    if (wic) wic->Release();
    {
        wchar_t file[MAX_PATH];
        Join(file, m->allImages, L"broken.png");
        HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE)
        {
            BYTE junk[4096];
            for (int i = 0; i < 4096; i++) junk[i] = (BYTE)(i * 37 + 11);
            DWORD n = 0;
            WriteFile(f, junk, sizeof(junk), &n, nullptr);
            CloseHandle(f);
        }
        Join(file, m->allImages, L"readme.txt");
        f = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE)
        {
            DWORD n = 0;
            WriteFile(f, "not a picture", 13, &n, nullptr);
            CloseHandle(f);
        }
    }
    LogF(log, L"  %d picture format(s)", m->imageCount);

    // Videos.
    struct { const wchar_t* file; GUID v; GUID a; UINT32 ch; const wchar_t* copyAs; } kClips[] = {
        { L"h264-aac.mp4", MFVideoFormat_H264, MFAudioFormat_AAC, 2, nullptr },
        { L"h264-aac51.mp4", MFVideoFormat_H264, MFAudioFormat_AAC, 6, nullptr },
        { L"h264-silent.mp4", MFVideoFormat_H264, GUID_NULL, 0, nullptr },
        { L"h264-aac.3gp", MFVideoFormat_H264, MFAudioFormat_AAC, 2, nullptr },
        { L"wmv-wma.wmv", MFVideoFormat_WMV3, MFAudioFormat_WMAudioV8, 2, nullptr },
        { L"h264-aac.mov", MFVideoFormat_H264, MFAudioFormat_AAC, 2, L"h264-aac.mp4" },
        { L"h264-aac.m4v", MFVideoFormat_H264, MFAudioFormat_AAC, 2, L"h264-aac.mp4" },
        { L"h264-aac.divx", MFVideoFormat_H264, MFAudioFormat_AAC, 2, L"h264-aac.mp4" },    // unknown extension: by content
    };
    wchar_t firstMp4[MAX_PATH] = L"";
    for (int i = 0; i < (int)(sizeof(kClips) / sizeof(kClips[0])); i++)
    {
        TestVideo& v = m->videos[m->videoCount];
        wchar_t sub[64], file[MAX_PATH];
        _snwprintf(sub, 64, L"video-%d", i);
        Join(v.folder, m->root, sub);
        CreateDirectoryW(v.folder, nullptr);
        Join(file, v.folder, kClips[i].file);
        HRESULT hr;
        if (kClips[i].copyAs)
            hr = firstMp4[0] && CopyFileW(firstMp4, file, FALSE) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
        else
        {
            // the sink writer picks the container by the extension
            hr = WriteClip(file, kClips[i].v, kClips[i].a, kClips[i].ch);
        }
        if (FAILED(hr))
        {
            LogF(log, L"  test media: clip %ls not made (0x%08lX): skipped", kClips[i].file, (unsigned long)hr);
            RemoveDirectoryW(v.folder);
            continue;
        }
        if (i == 0) wcscpy(firstMp4, file);
        wcscpy(v.name, kClips[i].file);
        v.sound = !IsEqualGUID(kClips[i].a, GUID_NULL);
        v.channels = kClips[i].ch;
        m->videoCount++;
        made++;
    }
    // a broken file next to a good one: the good one must still play
    if (firstMp4[0])
    {
        TestVideo& v = m->videos[m->videoCount];
        wchar_t file[MAX_PATH];
        Join(v.folder, m->root, L"video-broken");
        CreateDirectoryW(v.folder, nullptr);
        Join(file, v.folder, L"broken.mp4");
        HANDLE f = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE)
        {
            BYTE junk[65536];
            for (int i = 0; i < 65536; i++) junk[i] = (BYTE)(i * 131 + 7);
            DWORD n = 0;
            WriteFile(f, junk, sizeof(junk), &n, nullptr);
            CloseHandle(f);
        }
        Join(file, v.folder, L"good.mp4");
        if (CopyFileW(firstMp4, file, FALSE))
        {
            wcscpy(v.name, L"broken.mp4 + good.mp4");
            v.sound = true;
            v.channels = 2;
            m->videoCount++;
        }
    }
    LogF(log, L"  %d video clip(s)", m->videoCount);
    AddSamples(m, log, warn);
    return made > 0 || m->imageCount || m->videoCount;
}

void TestMediaDelete(TestMedia* m)
{
    if (!m->root[0]) return;
    // the whole folder (double zero-terminated for SHFileOperation)
    wchar_t from[MAX_PATH + 2] = {};
    wcsncpy(from, m->root, MAX_PATH);
    SHFILEOPSTRUCTW op = {};
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_NO_UI;
    int rc = SHFileOperationW(&op);
    AppLog(L"test media %ls removed: %ls (%d)", m->root, rc == 0 && GetFileAttributesW(m->root) == INVALID_FILE_ATTRIBUTES ? L"yes" : L"NO", rc);
    m->root[0] = 0;
}

// ---------------------------------------------------------------------------
// MJPEG server

static SOCKET           g_listen = INVALID_SOCKET;
static HANDLE           g_acceptThread;
static volatile LONG    g_stop, g_clients, g_color = (LONG)0xCC3333;
static volatile LONGLONG g_framesSent;
static bool             g_auth, g_wsa;
static BYTE*            g_jpeg[8];
static DWORD            g_jpegLen[8];

static int ColorIndex(ULONG c)
{
    for (int i = 0; i < 8; i++)
        if (kTestColors[i] == c) return i;
    return 0;
}

static bool SendAll(SOCKET s, const char* p, int n)
{
    while (n > 0)
    {
        int k = send(s, p, n, 0);
        if (k <= 0) return false;
        p += k;
        n -= k;
    }
    return true;
}

static DWORD WINAPI ClientThread(LPVOID p)
{
    SOCKET s = (SOCKET)(ULONG_PTR)p;
    DWORD timeout = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
    char req[4096];
    int got = 0;
    while (got < (int)sizeof(req) - 1)
    {
        int k = recv(s, req + got, (int)sizeof(req) - 1 - got, 0);
        if (k <= 0) break;
        got += k;
        req[got] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    req[got] = 0;
    // "s2c:test" in Basic authentication
    bool authorized = !g_auth || strstr(req, "Authorization: Basic czJjOnRlc3Q=") != nullptr;
    if (!authorized)
    {
        const char* deny = "HTTP/1.0 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"s2c\"\r\nContent-Length: 0\r\n\r\n";
        SendAll(s, deny, (int)strlen(deny));
    }
    else
    {
        const char* head = "HTTP/1.0 200 OK\r\nCache-Control: no-cache\r\n"
                           "Content-Type: multipart/x-mixed-replace; boundary=s2cframe\r\n\r\n";
        bool ok = SendAll(s, head, (int)strlen(head));
        while (ok && !g_stop)
        {
            int i = ColorIndex((ULONG)g_color);
            char part[160];
            int n = _snprintf(part, sizeof(part), "--s2cframe\r\nContent-Type: image/jpeg\r\nContent-Length: %lu\r\n\r\n", g_jpegLen[i]);
            ok = SendAll(s, part, n) && SendAll(s, (const char*)g_jpeg[i], (int)g_jpegLen[i]) && SendAll(s, "\r\n", 2);
            if (ok) InterlockedIncrement64(&g_framesSent);
            Sleep(100);                                     // 10 pictures per second
        }
    }
    shutdown(s, SD_BOTH);
    closesocket(s);
    InterlockedDecrement(&g_clients);
    return 0;
}

static DWORD WINAPI AcceptThread(LPVOID)
{
    while (!g_stop)
    {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(g_listen, &set);
        timeval tv = { 0, 200000 };
        if (select(0, &set, nullptr, nullptr, &tv) <= 0) continue;
        SOCKET c = accept(g_listen, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        InterlockedIncrement(&g_clients);
        HANDLE t = CreateThread(nullptr, 0, ClientThread, (LPVOID)(ULONG_PTR)c, 0, nullptr);
        if (t) CloseHandle(t);
        else
        {
            closesocket(c);
            InterlockedDecrement(&g_clients);
        }
    }
    return 0;
}

bool MjpegStart(USHORT* port, bool auth)
{
    if (!g_wsa)
    {
        WSADATA wd;
        if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) return false;
        g_wsa = true;
    }
    if (!g_jpeg[0])
    {
        IWICImagingFactory* wic = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
        for (int i = 0; i < 8; i++) JpegInMemory(wic, kTestColors[i], &g_jpeg[i], &g_jpegLen[i]);
        wic->Release();
        if (!g_jpeg[0]) return false;
    }
    MjpegStop();
    g_auth = auth;
    g_stop = 0;
    g_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_listen == INVALID_SOCKET) return false;
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(*port);
    if (bind(g_listen, (sockaddr*)&a, sizeof(a)) != 0 || listen(g_listen, 8) != 0)
    {
        AppLog(L"mjpeg test server: cannot listen on port %u (%d)", *port, WSAGetLastError());
        closesocket(g_listen);
        g_listen = INVALID_SOCKET;
        return false;
    }
    int len = sizeof(a);
    getsockname(g_listen, (sockaddr*)&a, &len);
    *port = ntohs(a.sin_port);
    g_acceptThread = CreateThread(nullptr, 0, AcceptThread, nullptr, 0, nullptr);
    AppLog(L"mjpeg test server on 127.0.0.1:%u%ls", *port, auth ? L" (authentication)" : L"");
    return g_acceptThread != nullptr;
}

void MjpegSetColor(ULONG color) { InterlockedExchange(&g_color, (LONG)color); }
LONG MjpegClients() { return g_clients; }
ULONGLONG MjpegFramesSent() { return (ULONGLONG)g_framesSent; }

void MjpegStop()
{
    if (g_listen == INVALID_SOCKET && !g_acceptThread) return;
    InterlockedExchange(&g_stop, 1);
    if (g_acceptThread)
    {
        WaitForSingleObject(g_acceptThread, 3000);
        CloseHandle(g_acceptThread);
        g_acceptThread = nullptr;
    }
    if (g_listen != INVALID_SOCKET) closesocket(g_listen);
    g_listen = INVALID_SOCKET;
    for (int i = 0; i < 50 && g_clients > 0; i++) Sleep(100);    // the client threads end within a picture
}

// ---------------------------------------------------------------------------
// Speak2Mic Microphone level

static const PROPERTYKEY kAdapterName = { { 0x026e516e, 0xb814, 0x414b, { 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22 } }, 2 };

void MicMeasure(DWORD ms, MicResult* out)
{
    ZeroMemory(out, sizeof(*out));
    IMMDeviceEnumerator* en = nullptr;
    IMMDeviceCollection* list = nullptr;
    IMMDevice* dev = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (SUCCEEDED(hr)) hr = en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &list);
    UINT n = 0;
    if (SUCCEEDED(hr)) list->GetCount(&n);
    for (UINT i = 0; i < n && !dev; i++)
    {
        IMMDevice* d = nullptr;
        IPropertyStore* ps = nullptr;
        if (SUCCEEDED(list->Item(i, &d)) && SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)))
        {
            PROPVARIANT v;
            PropVariantInit(&v);
            if (SUCCEEDED(ps->GetValue(kAdapterName, &v)) && v.vt == VT_LPWSTR && !_wcsicmp(v.pwszVal, L"Speak2Mic"))
            {
                dev = d;
                d = nullptr;
            }
            PropVariantClear(&v);
        }
        if (ps) ps->Release();
        if (d) d->Release();
    }
    if (list) list->Release();
    if (en) en->Release();
    if (!dev) return;
    out->found = true;

    IAudioClient* client = nullptr;
    IAudioCaptureClient* cap = nullptr;
    WAVEFORMATEX* mix = nullptr;
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&client);
    if (SUCCEEDED(hr)) hr = client->GetMixFormat(&mix);
    if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, mix, nullptr);
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&cap));
    if (SUCCEEDED(hr)) hr = client->Start();
    float* samples = nullptr;
    UINT32 count = 0, cap_ = 0;
    if (SUCCEEDED(hr))
    {
        bool isFloat = mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT ||
                       (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                        ((WAVEFORMATEXTENSIBLE*)mix)->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT);
        UINT32 bytes = mix->wBitsPerSample / 8, block = mix->nBlockAlign;
        cap_ = mix->nSamplesPerSec * (ms / 1000 + 2);
        samples = (float*)malloc((size_t)cap_ * sizeof(float));
        DWORD start = GetTickCount();
        while (samples && GetTickCount() - start < ms)
        {
            Sleep(10);
            UINT32 packet = 0;
            while (SUCCEEDED(cap->GetNextPacketSize(&packet)) && packet)
            {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                for (UINT32 f = 0; f < frames && count < cap_; f++)
                {
                    const BYTE* s = data + (size_t)f * block;
                    float v = 0;
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                    {
                        if (isFloat) v = *(const float*)s;
                        else if (bytes == 2) v = *(const SHORT*)s / 32768.0f;
                        else if (bytes == 3) v = (float)(((int)s[2] << 24 | (int)s[1] << 16 | (int)s[0] << 8) >> 8) / 8388608.0f;
                        else if (bytes == 4) v = (float)(*(const LONG*)s / 2147483648.0);
                    }
                    samples[count++] = v;
                }
                cap->ReleaseBuffer(frames);
            }
        }
        client->Stop();
        // the strongest frequency 100..4000 Hz over the first 0.5 s (what is playing, when the tone is not)
        {
            UINT32 n = count < mix->nSamplesPerSec / 2 ? count : mix->nSamplesPerSec / 2;
            double best = 0;
            for (int f = 100; f <= 4000 && n; f += 20)
            {
                double c = 2.0 * cos(2.0 * 3.14159265358979 * f / mix->nSamplesPerSec), q1 = 0, q2 = 0;
                for (UINT32 i = 0; i < n; i++)
                {
                    double q0 = samples[i] + c * q1 - q2;
                    q2 = q1;
                    q1 = q0;
                }
                double pw = q1 * q1 + q2 * q2 - c * q1 * q2;
                if (pw > best)
                {
                    best = pw;
                    out->peakHz = f;
                }
            }
            out->rate = mix->nSamplesPerSec;
            out->channels = mix->nChannels;
        }
        // level and the 1 kHz share (Goertzel)
        double sum = 0, s1 = 0, s2 = 0, coeff = 2.0 * cos(2.0 * 3.14159265358979 * kToneHz / mix->nSamplesPerSec);
        for (UINT32 i = 0; i < count; i++)
        {
            sum += (double)samples[i] * samples[i];
            double s0 = samples[i] + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        if (count)
        {
            out->rms = sqrt(sum / count);
            double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
            double amp = 2.0 * sqrt(power > 0 ? power : 0) / count;     // the tone's amplitude
            out->tone = out->rms > 1e-6 ? (amp * amp / 2.0) / (out->rms * out->rms) : 0;
            if (out->tone > 1) out->tone = 1;
        }
    }
    out->hr = hr;
    free(samples);
    if (mix) CoTaskMemFree(mix);
    if (cap) cap->Release();
    if (client) client->Release();
    dev->Release();
}
