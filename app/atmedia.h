// Test media of s2cautotest, made at its start in a temporary folder and removed at its end: pictures in every format
// Windows encodes, video clips in several containers / codecs (moving colour fields and a 1 kHz tone), a local MJPEG
// server, and a level meter on Speak2Mic Microphone (the clips' sound end to end).
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// The colours of the generated media (0xRRGGBB, web-safe so GIF keeps them exactly).
extern const ULONG kTestColors[8];
const int    kTestW = 640, kTestH = 480;     // size of the pictures and clips
const double kClipSeconds = 3.0;             // clip length; its colour field changes every 0.5 s (kTestColors[0..3])
const double kToneHz = 1000.0;               // the clips' sound

struct TestImage { wchar_t folder[MAX_PATH]; const wchar_t* format; ULONG color; };
struct TestVideo { wchar_t folder[MAX_PATH]; wchar_t name[64]; bool sound; UINT32 channels; };

struct TestMedia
{
    wchar_t   root[MAX_PATH];
    TestImage images[8];                     // one folder per format, one picture each
    int       imageCount;
    wchar_t   allImages[MAX_PATH];           // every picture + a broken one + a text file
    TestVideo videos[16];                    // one folder per clip
    int       videoCount;
};

typedef void (*TestLog)(const wchar_t* line);
// Creates the media (each failure is logged and skipped); false: nothing at all could be made.
bool TestMediaCreate(TestMedia* m, TestLog log);
void TestMediaDelete(TestMedia* m);
ULONG TestClipColorAt(double seconds);       // the colour field of a clip at that time

// Local MJPEG server on 127.0.0.1 (port 0: any free one). auth: requires user "s2c", password "test".
bool  MjpegStart(USHORT* port, bool auth);
void  MjpegSetColor(ULONG color);
void  MjpegStop();
LONG  MjpegClients();
ULONGLONG MjpegFramesSent();

// Speak2Mic Microphone for `ms`: level (RMS of channel 1, 0..1) and the share of it that is the 1 kHz tone.
struct MicResult { bool found; HRESULT hr; double rms, tone; };
void MicMeasure(DWORD ms, MicResult* out);
