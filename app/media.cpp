// Pictures for the cameras (see media.h).
#include "media.h"
#include <wincodec.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

// ---------------------------------------------------------------------------
// Picture

bool Picture::Alloc(int width, int height)
{
    if (width <= 0 || height <= 0) return false;
    if (px && w == width && h == height) return true;
    Free();
    px = (ULONG*)VirtualAlloc(nullptr, (SIZE_T)width * height * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!px) return false;
    w = width;
    h = height;
    return true;
}

void Picture::Free()
{
    if (px) VirtualFree(px, 0, MEM_RELEASE);
    px = nullptr;
    w = h = 0;
}

// ---------------------------------------------------------------------------
// Helpers

static void FillBlack(ULONG* dst, int w, int h)
{
    for (SIZE_T i = 0, n = (SIZE_T)w * h; i < n; i++) dst[i] = 0xFF000000;
}

// The largest rectangle with the source's aspect ratio inside dw x dh, centred.
static void FitRect(int sw, int sh, int dw, int dh, int* x, int* y, int* w, int* h)
{
    if ((LONGLONG)sw * dh > (LONGLONG)sh * dw)
    {
        *w = dw;
        *h = (int)((LONGLONG)sh * dw / sw);
    }
    else
    {
        *h = dh;
        *w = (int)((LONGLONG)sw * dh / sh);
    }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
    *x = (dw - *w) / 2;
    *y = (dh - *h) / 2;
}

static IWICImagingFactory* Wic()
{
    // One per thread (the callers' threads each initialised COM).
    static thread_local IWICImagingFactory* factory;
    if (!factory)
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    return factory;
}

// The first frame of a decoder as 32bpp premultiplied BGRA (transparent parts end up black).
static IWICBitmapSource* FirstFrameBgra(IWICBitmapDecoder* decoder)
{
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* conv = nullptr;
    if (FAILED(decoder->GetFrame(0, &frame))) return nullptr;
    HRESULT hr = Wic()->CreateFormatConverter(&conv);
    if (SUCCEEDED(hr))
        hr = conv->Initialize(frame, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom);
    frame->Release();
    if (FAILED(hr))
    {
        if (conv) conv->Release();
        return nullptr;
    }
    return conv;
}

static void OpaqueAlpha(ULONG* px, SIZE_T n)
{
    for (SIZE_T i = 0; i < n; i++) px[i] |= 0xFF000000;
}

// ---------------------------------------------------------------------------
// Images

bool ImageFileSize(const wchar_t* path, UINT* w, UINT* h)
{
    *w = *h = 0;
    IWICImagingFactory* f = Wic();
    IWICBitmapDecoder* decoder = nullptr;
    if (!f || FAILED(f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
        return false;
    IWICBitmapFrameDecode* frame = nullptr;
    bool ok = SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(w, h)) && *w && *h;
    if (frame) frame->Release();
    decoder->Release();
    return ok;
}

bool RenderImageFile(const wchar_t* path, ULONG* dst, int dw, int dh, UINT* sw, UINT* sh)
{
    *sw = *sh = 0;
    IWICImagingFactory* f = Wic();
    IWICBitmapDecoder* decoder = nullptr;
    if (!f || FAILED(f->CreateDecoderFromFilename(path, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
        return false;
    IWICBitmapSource* src = FirstFrameBgra(decoder);
    decoder->Release();
    if (!src) return false;
    bool ok = false;
    if (SUCCEEDED(src->GetSize(sw, sh)) && *sw && *sh)
    {
        int x, y, w, h;
        FitRect((int)*sw, (int)*sh, dw, dh, &x, &y, &w, &h);
        IWICBitmapSource* scaled = src;
        IWICBitmapScaler* scaler = nullptr;
        if ((UINT)w != *sw || (UINT)h != *sh)
        {
            if (SUCCEEDED(f->CreateBitmapScaler(&scaler)) &&
                SUCCEEDED(scaler->Initialize(src, (UINT)w, (UINT)h, WICBitmapInterpolationModeFant)))
                scaled = scaler;
            else
                w = 0;
        }
        if (w)
        {
            FillBlack(dst, dw, dh);
            WICRect rc = { 0, 0, w, h };
            ok = SUCCEEDED(scaled->CopyPixels(&rc, (UINT)dw * 4, (UINT)((SIZE_T)dw * h * 4),
                                              (BYTE*)(dst + (SIZE_T)y * dw + x)));
            OpaqueAlpha(dst, (SIZE_T)dw * dh);
        }
        if (scaler) scaler->Release();
    }
    src->Release();
    return ok;
}

bool DecodeImageMemory(const BYTE* data, size_t size, Picture* out)
{
    IWICImagingFactory* f = Wic();
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    if (!f || FAILED(f->CreateStream(&stream))) return false;
    bool ok = false;
    if (SUCCEEDED(stream->InitializeFromMemory((BYTE*)data, (DWORD)size)) &&
        SUCCEEDED(f->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnDemand, &decoder)))
    {
        IWICBitmapSource* src = FirstFrameBgra(decoder);
        UINT w = 0, h = 0;
        if (src && SUCCEEDED(src->GetSize(&w, &h)) && w && h && w <= 8192 && h <= 8192 && out->Alloc((int)w, (int)h))
        {
            ok = SUCCEEDED(src->CopyPixels(nullptr, w * 4, w * h * 4, (BYTE*)out->px));
            if (ok) OpaqueAlpha(out->px, (SIZE_T)w * h);
        }
        if (src) src->Release();
        decoder->Release();
    }
    stream->Release();
    return ok;
}

// ---------------------------------------------------------------------------
// Scaling (video frames, stream pictures): bilinear, 16.16 fixed point

void FitPixels(const BYTE* src, int sw, int sh, LONG stride, ULONG* dst, int dw, int dh)
{
    if (sw <= 0 || sh <= 0) { FillBlack(dst, dw, dh); return; }
    if (sw == dw && sh == dh)
    {
        for (int y = 0; y < dh; y++)
        {
            const ULONG* s = (const ULONG*)(src + (LONG_PTR)y * stride);
            ULONG* d = dst + (SIZE_T)y * dw;
            for (int x = 0; x < dw; x++) d[x] = s[x] | 0xFF000000;
        }
        return;
    }
    int rx, ry, rw, rh;
    FitRect(sw, sh, dw, dh, &rx, &ry, &rw, &rh);
    FillBlack(dst, dw, dh);

    // Source position of every target column: index and weight of the right neighbour (0..255).
    static thread_local int* xs;
    static thread_local int xsCap;
    if (xsCap < rw)
    {
        free(xs);
        xs = (int*)malloc(sizeof(int) * rw * 2);
        xsCap = xs ? rw : 0;
        if (!xs) return;
    }
    for (int x = 0; x < rw; x++)
    {
        LONGLONG fx = ((LONGLONG)(2 * x + 1) * sw * 65536 / (2 * rw)) - 32768;   // pixel centres
        if (fx < 0) fx = 0;
        int ix = (int)(fx >> 16);
        int wx = (int)((fx >> 8) & 0xFF);
        if (ix >= sw - 1) { ix = sw - 1; wx = 0; }
        xs[2 * x] = ix;
        xs[2 * x + 1] = wx;
    }
    for (int y = 0; y < rh; y++)
    {
        LONGLONG fy = ((LONGLONG)(2 * y + 1) * sh * 65536 / (2 * rh)) - 32768;
        if (fy < 0) fy = 0;
        int iy = (int)(fy >> 16);
        int wy = (int)((fy >> 8) & 0xFF);
        if (iy >= sh - 1) { iy = sh - 1; wy = 0; }
        const ULONG* r0 = (const ULONG*)(src + (LONG_PTR)iy * stride);
        const ULONG* r1 = (const ULONG*)(src + (LONG_PTR)(iy + (wy ? 1 : 0)) * stride);
        ULONG* d = dst + (SIZE_T)(ry + y) * dw + rx;
        for (int x = 0; x < rw; x++)
        {
            int ix = xs[2 * x], wx = xs[2 * x + 1];
            int ix1 = ix + (wx ? 1 : 0);
            ULONG a = r0[ix], b = r0[ix1], c = r1[ix], e = r1[ix1];
            ULONG out = 0xFF000000;
            for (int sh8 = 0; sh8 < 24; sh8 += 8)
            {
                int top = (int)((a >> sh8) & 0xFF) * (256 - wx) + (int)((b >> sh8) & 0xFF) * wx;
                int bot = (int)((c >> sh8) & 0xFF) * (256 - wx) + (int)((e >> sh8) & 0xFF) * wx;
                int v = (top * (256 - wy) + bot * wy) >> 16;
                out |= (ULONG)v << sh8;
            }
            d[x] = out;
        }
    }
}

// ---------------------------------------------------------------------------
// Text and notice screens (GDI on a top-down DIB section)

struct Dib
{
    HDC     dc = nullptr;
    HBITMAP bmp = nullptr;
    HGDIOBJ old = nullptr;
    ULONG*  bits = nullptr;
    bool Create(int w, int h)
    {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(nullptr);
        bmp = dc ? CreateDIBSection(dc, &bi, DIB_RGB_COLORS, (void**)&bits, nullptr, 0) : nullptr;
        if (!bmp) return false;
        old = SelectObject(dc, bmp);
        return true;
    }
    ~Dib()
    {
        if (dc && old) SelectObject(dc, old);
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
    }
};

static HFONT MakeFont(int px, int weight)
{
    return CreateFontW(-px, 0, 0, 0, weight, 0, 0, 0, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                       ANTIALIASED_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

// Draws `text` centred horizontally in [left, right], top at `top`, as large as fits (start size, at most maxH high).
// Returns the bottom of the text.
static int DrawFitted(HDC dc, const wchar_t* text, int left, int right, int top, int maxH, int startPx, int weight,
                      COLORREF color, bool vcenter)
{
    if (!text || !*text) return top;
    int px = startPx;
    RECT rc;
    HFONT font = nullptr;
    for (;;)
    {
        font = MakeFont(px, weight);
        HGDIOBJ prev = SelectObject(dc, font);
        rc = { left, 0, right, 0 };
        DrawTextW(dc, text, -1, &rc, DT_CALCRECT | DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
        SelectObject(dc, prev);
        if ((rc.right - rc.left <= right - left && rc.bottom - rc.top <= maxH) || px <= 10) break;
        DeleteObject(font);
        px = px * 9 / 10;
    }
    int height = rc.bottom - rc.top;
    int y = vcenter ? top + (maxH - height) / 2 : top;
    RECT out = { left, y, right, y + height };
    HGDIOBJ prev = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0, 0, 0));
    int shadow = px / 24 > 1 ? px / 24 : 1;
    RECT sh = { out.left + shadow, out.top + shadow, out.right + shadow, out.bottom + shadow };
    DrawTextW(dc, text, -1, &sh, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    SetTextColor(dc, color);
    DrawTextW(dc, text, -1, &out, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    SelectObject(dc, prev);
    DeleteObject(font);
    return out.bottom;
}

void RenderTextScreen(ULONG* dst, int w, int h, const wchar_t* text)
{
    Dib dib;
    if (!dib.Create(w, h))
    {
        FillBlack(dst, w, h);
        return;
    }
    // The Show2Cam colours (violet -> cyan, as the icon), darkened so white text reads well.
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
        {
            int t = (int)(((LONGLONG)y * 650 / (h > 1 ? h - 1 : 1)) + ((LONGLONG)x * 350 / (w > 1 ? w - 1 : 1)));   // 0..1000
            int r = (108 * (1000 - t) + 0 * t) / 1000, g = (59 * (1000 - t) + 194 * t) / 1000, b = (255 * (1000 - t) + 255 * t) / 1000;
            dib.bits[(SIZE_T)y * w + x] = (ULONG)((r * 45 / 100) << 16 | (g * 45 / 100) << 8 | (b * 45 / 100));
        }
    DrawFitted(dib.dc, text, w / 20, w - w / 20, h / 10, h - h / 5, h * 28 / 100, FW_SEMIBOLD, RGB(255, 255, 255), true);
    GdiFlush();
    for (SIZE_T i = 0, n = (SIZE_T)w * h; i < n; i++) dst[i] = dib.bits[i] | 0xFF000000;
}

void RenderNoticeScreen(ULONG* dst, int w, int h, NoticeIcon icon, const wchar_t* title, const wchar_t* detail)
{
    Dib dib;
    if (!dib.Create(w, h))
    {
        FillBlack(dst, w, h);
        return;
    }
    for (SIZE_T i = 0, n = (SIZE_T)w * h; i < n; i++) dib.bits[i] = 0x181820;
    HDC dc = dib.dc;

    // Icon: a box of s x s centred, in the upper part.
    int s = (w < h ? w : h) * 26 / 100;
    int cx = w / 2, top = h * 16 / 100;
    int pen = s / 14 > 2 ? s / 14 : 2;
    LOGBRUSH lb = { BS_SOLID, RGB(200, 200, 210), 0 };
    HPEN light = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND | PS_JOIN_ROUND, (DWORD)pen, &lb, 0, nullptr);
    HGDIOBJ oldPen = SelectObject(dc, light);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    if (icon == NoticeNoSignal)
    {
        // A video camera (body + lens wedge), struck through in red.
        int bl = cx - s / 2, bt = top + s / 4, br = cx + s / 6, bb = top + s * 3 / 4;
        RoundRect(dc, bl, bt, br, bb, s / 6, s / 6);
        POINT wedge[3] = { { br + s / 16, (bt + bb) / 2 }, { cx + s / 2, bt + s / 12 }, { cx + s / 2, bb - s / 12 } };
        Polygon(dc, wedge, 3);
        LOGBRUSH red = { BS_SOLID, RGB(235, 70, 60), 0 };
        HPEN slash = ExtCreatePen(PS_GEOMETRIC | PS_SOLID | PS_ENDCAP_ROUND, (DWORD)(pen * 3 / 2), &red, 0, nullptr);
        SelectObject(dc, slash);
        MoveToEx(dc, cx - s / 2, top + s - s / 12, nullptr);
        LineTo(dc, cx + s / 2, top + s / 12);
        SelectObject(dc, light);
        DeleteObject(slash);
    }
    else if (icon == NoticeEmptyFolder)
    {
        // A folder outline.
        int l = cx - s / 2, r = cx + s / 2, t = top + s / 4, b = top + s * 4 / 5;
        POINT f[6] = { { l, b }, { l, t }, { l + s / 3, t }, { l + s * 2 / 5, t + s / 10 }, { r, t + s / 10 }, { r, b } };
        Polygon(dc, f, 6);
        MoveToEx(dc, l, t + s / 5, nullptr);
        LineTo(dc, r, t + s / 5);
    }
    else
    {
        // A circle with "!".
        Ellipse(dc, cx - s / 2 + pen, top + pen, cx + s / 2 - pen, top + s - pen);
        MoveToEx(dc, cx, top + s / 4, nullptr);
        LineTo(dc, cx, top + s * 3 / 5);
        MoveToEx(dc, cx, top + s * 3 / 4, nullptr);
        LineTo(dc, cx, top + s * 3 / 4);
    }
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(light);

    int y = top + s + h / 20;
    y = DrawFitted(dc, title, w / 16, w - w / 16, y, h / 6, h * 9 / 100, FW_SEMIBOLD, RGB(255, 255, 255), false);
    DrawFitted(dc, detail, w / 12, w - w / 12, y + h / 40, h - y - h / 16, h * 4 / 100, FW_NORMAL, RGB(170, 170, 180), false);
    GdiFlush();
    for (SIZE_T i = 0, n = (SIZE_T)w * h; i < n; i++) dst[i] = dib.bits[i] | 0xFF000000;
}

// ---------------------------------------------------------------------------
// Media files

bool IsMediaFile(const wchar_t* name, MediaKind kind)
{
    const wchar_t* dot = wcsrchr(name, L'.');
    if (!dot) return false;
    static const wchar_t* kImages[] = { L".png", L".jpg", L".jpeg", L".jpe", L".jfif", L".bmp", L".dib", L".gif", L".tif",
                                        L".tiff", L".ico", L".webp", L".heic", L".heif", L".avif", L".jxr", L".wdp", L".hdp" };
    static const wchar_t* kVideo[] = { L".mp4", L".m4v", L".mov", L".3gp", L".wmv", L".asf", L".avi", L".mkv", L".webm",
                                       L".ts", L".mts", L".m2ts" };
    if (kind == MediaImages)
    {
        for (const wchar_t* e : kImages)
            if (_wcsicmp(dot, e) == 0) return true;
    }
    else
    {
        for (const wchar_t* e : kVideo)
            if (_wcsicmp(dot, e) == 0) return true;
    }
    return false;
}

int ListMediaFiles(const wchar_t* folder, MediaKind kind, wchar_t (*names)[MAX_PATH], int max)
{
    wchar_t pattern[MAX_PATH];
    _snwprintf(pattern, MAX_PATH, L"%ls\\*", folder);
    pattern[MAX_PATH - 1] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int n = 0;
    do
    {
        if (!(fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN)) && IsMediaFile(fd.cFileName, kind) && n < max)
        {
            wcsncpy(names[n], fd.cFileName, MAX_PATH - 1);
            names[n][MAX_PATH - 1] = 0;
            n++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    qsort(names, n, sizeof(names[0]), [](const void* a, const void* b) { return _wcsicmp((const wchar_t*)a, (const wchar_t*)b); });
    return n;
}

static int RandomIndex(int n)
{
    ULONG r = 0;
    if (BCryptGenRandom(nullptr, (PUCHAR)&r, sizeof(r), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        r = GetTickCount() ^ (GetCurrentThreadId() << 16);
    return (int)(r % (ULONG)n);
}

bool PickRandomFile(const wchar_t* folder, MediaKind kind, const wchar_t* last, wchar_t* out)
{
    static thread_local wchar_t (*names)[MAX_PATH];
    if (!names) names = (wchar_t (*)[MAX_PATH])malloc(sizeof(wchar_t) * MAX_PATH * 1024);
    if (!names) return false;
    int n = ListMediaFiles(folder, kind, names, 1024);
    if (n == 0) return false;
    // Random, but never the file just shown while there is another one (as Speak2Mic's music).
    for (;;)
    {
        int pick = RandomIndex(n);
        _snwprintf(out, MAX_PATH, L"%ls\\%ls", folder, names[pick]);
        out[MAX_PATH - 1] = 0;
        if (n == 1 || !last || _wcsicmp(out, last) != 0) return true;
    }
}

void DefaultMediaFolder(MediaKind kind, wchar_t* folder)
{
    const wchar_t* sub = kind == MediaImages ? L"images" : L"mp4";
    wchar_t exeDir[MAX_PATH];
    GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
    wchar_t* slash = wcsrchr(exeDir, L'\\');
    if (slash) *slash = 0;
    _snwprintf(folder, MAX_PATH, L"%ls\\%ls", exeDir, sub);
    folder[MAX_PATH - 1] = 0;
    if (GetFileAttributesW(folder) != INVALID_FILE_ATTRIBUTES) return;
    // The package layout (x64\Show2Cam.exe next to ..\images)
    wchar_t up[MAX_PATH];
    _snwprintf(up, MAX_PATH, L"%ls\\..\\%ls", exeDir, sub);
    up[MAX_PATH - 1] = 0;
    if (GetFileAttributesW(up) != INVALID_FILE_ATTRIBUTES) GetFullPathNameW(up, MAX_PATH, folder, nullptr);
}
