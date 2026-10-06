// "Self-view": the panel itself uses a camera like any webcam program (Media Foundation capture through the
// Windows camera stack / Frame Server) and reads its frames all the time, so for Windows the camera is really in
// use (privacy "camera in use" indicator, Settings > Privacy > Camera), while no other program watches it.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct SelfViewStatus
{
    bool    open;           // the camera is open and delivers frames
    HRESULT error;          // last error opening / reading (S_OK if none)
    ULONG   fps;            // frames per second received (last second)
    ULONG   width, height;  // the format Windows opened the camera with
};

class SelfView;
// Starts using the camera whose device interface is `cameraPath` (CamInfo::path); retries every 3 s when it fails.
SelfView* SelfViewStart(const wchar_t* cameraPath, int index);
void SelfViewStop(SelfView* v);                 // closes the camera; returns when it is closed
void SelfViewGetStatus(SelfView* v, SelfViewStatus* s);
