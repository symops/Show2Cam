// What the cameras show: one worker thread per camera renders its source (text, images, video, MJPEG stream)
// into frames of the camera's size and sends them to the driver; it also sets the camera's size and frame rate
// ("as the source" or the user's choice) whenever no program has the camera open.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "camdev.h"
#include "media.h"
#include "selfview.h"

enum SourceKind { SourceText = 0, SourceImages, SourceVideo, SourceStream, SourceGenerator, SourceKindCount };
enum AudioMode { AudioSpeak2Mic = 0, AudioOff, AudioDevice };

struct CamConfig
{
    int     kind = SourceText;
    wchar_t text[256] = L"";
    wchar_t imageFolder[MAX_PATH] = L"";     // empty: the "images" folder next to the program
    wchar_t videoFolder[MAX_PATH] = L"";     // empty: the "mp4" folder next to the program
    wchar_t url[512] = L"http://127.0.0.1:8080";    // MJPEG stream address
    int     audioMode = AudioSpeak2Mic;
    wchar_t audioDevice[256] = L"";          // endpoint id for AudioDevice
    ULONG   width = 0, height = 0;           // 0: as the source
    ULONG   fps = 0;                         // 0: as the source
    bool    paused = false;
    bool    selfView = false;                // the panel itself uses the camera (Windows sees it in use)
};

enum SourceState { StateStarting = 0, StateOk, StateNoSignal, StateNoFiles, StateError };

struct CamRunStatus
{
    bool       deviceOpen;                   // the camera's device interface is open
    S2C_STATUS driver;                       // what the driver reports (valid with deviceOpen)
    ULONG      sourceW, sourceH, sourceFps;  // the source's own format (0: not known)
    int        state;                        // SourceState
    wchar_t    current[MAX_PATH];            // file or address shown
    wchar_t    detail[256];                  // why not (error text)
    wchar_t    audio[256];                   // where the video's sound goes ("" = nowhere)
    bool       formatWaiting;                // another size is wanted but the camera is in use
    ULONG      wantW, wantH, wantFps;
    bool       selfView;                     // self-view is on
    SelfViewStatus self;                     // its state
};

// Events posted to the notify window: message `msg`, wParam = camera index, lParam = event.
enum CamEvent
{
    EvNoSignal = 1, EvSignal, EvNoFiles, EvVideoFile, EvVideoError, EvFormatChanged, EvFormatWaiting, EvAudioMissing,
    EvSelfViewOn, EvSelfViewError,
    EvPreviewFrame,     // a new frame for the preview window
};

class CameraRunner;
CameraRunner* RunnerStart(int index, const wchar_t* path, const CamConfig& config, HWND notify, UINT msg);
void RunnerConfigure(CameraRunner* r, const CamConfig& config);
void RunnerStop(CameraRunner* r);            // the camera goes back to the driver's test pattern
void RunnerGetStatus(CameraRunner* r, CamRunStatus* status);
void RunnerSetPreview(CameraRunner* r, bool on);
bool RunnerGetPreviewFrame(CameraRunner* r, Picture* out);   // the last frame sent (false: none yet)

// The size and rate a camera gets for the wanted (or the source's) format, within what the driver accepts.
void NormalizeFormat(ULONG* w, ULONG* h, ULONG* fps);
