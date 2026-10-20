// Pictures for the cameras: decoding (WIC), fitting into the camera's frame, text and notice screens, and the
// media files of a folder. Pixels are BGRA, top-down, alpha 0xFF. Callers initialise COM on their thread.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Before CoUninitialize on a thread that decoded pictures: releases that thread's WIC factory.
void MediaThreadEnd();
// Files decoded on the calling thread (pictures, clips). Windows' codecs leave a few event handles behind on the thread
// that opened a decoder - freed only when that thread ends (the autotest's handle probe: AVIF / HEIC / WebP, VP9, also
// H.264 grew by ~2-10 per file on one thread, by ~0 on a new thread per file). The camera's worker moves to a new thread
// after a few files.
void MediaCountDecode();
int  MediaThreadDecodes();
// Files Windows could not decode / play, for the whole process (every source, also a new one after a settings change).
// Each attempt to open a broken clip left 5..30 handles behind inside Media Foundation (the autotest's handle probe),
// and the sources tried their broken files again every minute: thousands of handles a day with one broken file in the
// folder. A file is tried again only once it has changed (size or time), been replaced or removed.
void MediaMarkBroken(const wchar_t* path);
// The known broken files directly in `folder` that are unchanged (up to `max`); their count.
int  MediaBrokenIn(const wchar_t* folder, wchar_t (*bad)[MAX_PATH], int max);

// A decoded picture (owns its pixels).
struct Picture
{
    ULONG* px = nullptr;
    int    w = 0, h = 0;
    ~Picture() { Free(); }
    bool Alloc(int width, int height);
    void Free();
};

// Image files (WIC: PNG, JPEG, BMP, GIF, TIFF, ICO, JPEG XR; WebP / HEIF / AVIF when Windows has their codecs).
bool ImageFileSize(const wchar_t* path, UINT* w, UINT* h);
// Decodes into dst (dw x dh) scaled to fit, centred on black (high-quality scaler). The image's own size in sw/sh.
bool RenderImageFile(const wchar_t* path, ULONG* dst, int dw, int dh, UINT* sw, UINT* sh);
// A JPEG (or any WIC image) in memory, decoded at its own size.
bool DecodeImageMemory(const BYTE* data, size_t size, Picture* out);

// Scales a BGRA/RGB32 picture (rows `stride` bytes apart, negative = bottom-up) into dst to fit, centred on black.
void FitPixels(const BYTE* src, int sw, int sh, LONG stride, ULONG* dst, int dw, int dh);

// The "Text" source: the text centred on the Show2Cam background, as large as fits.
void RenderTextScreen(ULONG* dst, int w, int h, const wchar_t* text);

// A notice screen (no signal, empty folder, ...): an icon, a title and a smaller detail line.
enum NoticeIcon { NoticeNoSignal, NoticeEmptyFolder, NoticeError, NoticeConnecting };
void RenderNoticeScreen(ULONG* dst, int w, int h, NoticeIcon icon, const wchar_t* title, const wchar_t* detail);

// Media files of a folder.
enum MediaKind { MediaImages, MediaVideo };
bool IsMediaFile(const wchar_t* name, MediaKind kind);
int  ListMediaFiles(const wchar_t* folder, MediaKind kind, wchar_t (*names)[MAX_PATH], int max);   // sorted by name
// A random file of the folder, never `last` again while there is another one. Full path into `out`.
// A random file of the folder, not one of `bad` (files that cannot be shown) and not `last` while there is another one.
// 1: picked; 0: no files; -1: every file is a bad one.
int PickRandomFile(const wchar_t* folder, MediaKind kind, const wchar_t* last, const wchar_t (*bad)[MAX_PATH], int badCount,
                   wchar_t* out);
// The default folders next to the program: "images" and "mp4".
void DefaultMediaFolder(MediaKind kind, wchar_t* folder);
