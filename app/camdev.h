// The Show2Cam cameras as the programs see them: their device interfaces (KSCATEGORY_VIDEO, reference string
// "Camera<N>") and the driver's property set on them (driver/s2cproto.h): pictures, status, size, name.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../driver/s2cproto.h"

#define S2C_MAX_CAMERAS_UI 10        // what the driver supports (CameraCount 1..10)

struct CamInfo
{
    int     index;              // 0-based camera number (from the interface's "\Camera<N>")
    wchar_t path[512];          // device interface path, for CamOpen
    wchar_t name[S2C_NAME_CHARS];   // FriendlyName (what programs show)
};

// The cameras the driver offers right now, sorted by number. Returns how many (up to max).
int CamList(CamInfo* out, int max);

// Opens a camera for its properties (overlapped handle); INVALID_HANDLE_VALUE on error (GetLastError()).
HANDLE CamOpen(const wchar_t* path);

// One property request; false on error (GetLastError()).
bool CamProperty(HANDLE cam, ULONG id, ULONG flags, void* data, DWORD size, DWORD* returned);
bool CamGetStatus(HANDLE cam, S2C_STATUS* status);
// ERROR_SUCCESS, ERROR_BUSY (the camera is in use: the size can't change now) or another error.
DWORD CamSetFormat(HANDLE cam, ULONG width, ULONG height, ULONG fps);
bool CamSetName(HANDLE cam, const wchar_t* name);     // empty = default name
bool CamSetCount(HANDLE cam, ULONG count);           // number of cameras 1..10 (any camera's handle)

// A picture for S2C_PROPERTY_FRAME: the header followed by width*height BGRA pixels (top-down).
struct CamFrame
{
    BYTE*  buffer = nullptr;
    SIZE_T capacity = 0;
    ULONG  width = 0, height = 0;

    ~CamFrame();
    bool   Resize(ULONG w, ULONG h);         // keeps the buffer if it is large enough
    ULONG* Pixels() { return (ULONG*)(buffer + sizeof(S2C_FRAME_HEADER)); }
};
bool CamSendFrame(HANDLE cam, CamFrame& frame);
bool CamSendTestPattern(HANDLE cam);         // the driver's own test pattern again
