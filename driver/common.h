// Common declarations for the Show2Cam AVStream driver: N virtual cameras ("Show2Cam Camera 1".."N") whose frames
// come from the Show2Cam program (a custom property on each camera filter) or, without it, from a test pattern.
#pragma once

#include <ntddk.h>
#include <windef.h>
#define NOBITMAP
#include <mmreg.h>
#undef NOBITMAP
#include <ks.h>
#include <ksmedia.h>

#include <ntstrsafe.h>
#include "compat.h"

#define S2C_POOLTAG      'C2hS'
#define S2C_MAX_CAMERAS  10
#define S2C_FORMATS      3              // YUY2, NV12, RGB32 for every camera

// AVStream calls its dispatch routines with the WDK's default calling convention: __stdcall on x86 (the MinGW
// typedefs carry none, so the routines are declared with it explicitly and cast in the tables), plain on x64.
#if defined(_X86_)
#define S2C_CB __stdcall
#else
#define S2C_CB
#endif

#include "s2cproto.h"

// ---------------------------------------------------------------------------

struct S2C_CAMERA
{
    ULONG                   Signature;  // 'CAM2'
    ULONG                   Index;
    ULONG                   Width, Height, Fps;
    WCHAR                   RefString[16];          // "Camera1".. (device interface reference string)

    // AVStream descriptors of this camera (its resolution is in the data ranges)
    KSFILTER_DESCRIPTOR     FilterDescriptor;
    KSPIN_DESCRIPTOR_EX     PinDescriptor;
    KS_DATARANGE_VIDEO      Ranges[S2C_FORMATS];
    PKSDATARANGE            RangePointers[S2C_FORMATS];
    KSALLOCATOR_FRAMING_EX  Framing;
    PKSFILTERFACTORY        Factory;

    // the picture from the program (BGRA, top-down), guarded by Lock
    FAST_MUTEX              Lock;
    ULONG*                  Picture;
    ULONG                   PictureWidth, PictureHeight;

    volatile LONG           Streaming;
    volatile LONG           PinsOpen;
    volatile LONGLONG       FramesDelivered, FramesDropped, PicturesReceived;
};
#define S2C_CAMERA_SIGNATURE 0x324D4143

struct S2C_DEVICE
{
    ULONG       CameraCount;
    BOOLEAN     FactoriesCreated;
    S2C_CAMERA* Cameras[S2C_MAX_CAMERAS];
};

// camera.cpp
NTSTATUS S2cCameraCreate(_In_ ULONG Index, _In_ ULONG Width, _In_ ULONG Height, _In_ ULONG Fps, _Out_ S2C_CAMERA** Camera);
void     S2cCameraFree(_In_ S2C_CAMERA* Camera);
// Puts the camera's saved name (Camera<N>Name) on its device interfaces; once its filter factory exists.
void     S2cCameraApplySavedName(_In_ S2C_CAMERA* Camera);
S2C_CAMERA* S2cCameraFromFilter(_In_ PKSFILTER Filter);
S2C_CAMERA* S2cCameraFromPin(_In_ PKSPIN Pin);
// Bytes of one frame of the negotiated format (YUY2, NV12 or RGB32).
ULONG S2cFrameSize(_In_ const KS_VIDEOINFOHEADER* Info);
// Renders one frame of the camera into Dst in the given format (picture or test pattern). FrameNumber animates
// the test pattern.
void S2cRenderFrame(_In_ S2C_CAMERA* Camera, _In_ const KS_VIDEOINFOHEADER* Info, _Out_ PUCHAR Dst, _In_ ULONG DstSize,
                    _In_ ULONGLONG FrameNumber);
