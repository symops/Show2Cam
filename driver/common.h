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

// ---------------------------------------------------------------------------
// The interface to the Show2Cam program: property set on every camera filter (open its device interface).
//   S2C_PROPERTY_FRAME  (SET)  S2C_FRAME_HEADER + Width*Height BGRA pixels, top-down: the picture to show
//                               (any size, scaled to the negotiated one); Width = 0: back to the test pattern
//   S2C_PROPERTY_STATUS (GET)  S2C_STATUS
// {6F1C2A9E-3B57-4E0C-9D1A-5C2E7B3F8A41}
#define STATIC_PROPSETID_Show2Cam 0x6f1c2a9e, 0x3b57, 0x4e0c, 0x9d, 0x1a, 0x5c, 0x2e, 0x7b, 0x3f, 0x8a, 0x41
enum { S2C_PROPERTY_FRAME = 0, S2C_PROPERTY_STATUS = 1 };
#define S2C_FRAME_MAGIC 0x4D415246      // 'FRAM'
#define S2C_MAX_FRAME_PIXELS (3840u * 2160u)

struct S2C_FRAME_HEADER
{
    ULONG Magic;
    ULONG Width;
    ULONG Height;
    ULONG Flags;                        // reserved, 0
};

struct S2C_STATUS
{
    ULONG Index;                        // 0-based camera number
    ULONG Width, Height, Fps;           // what the camera offers
    ULONG Streaming;                    // pins in the run state
    ULONG SourceWidth, SourceHeight;    // last picture from the program (0: test pattern)
    ULONG Reserved;
    ULONGLONG FramesDelivered;
    ULONGLONG FramesDropped;
    ULONGLONG PicturesReceived;
};

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
S2C_CAMERA* S2cCameraFromFilter(_In_ PKSFILTER Filter);
S2C_CAMERA* S2cCameraFromPin(_In_ PKSPIN Pin);
// Bytes of one frame of the negotiated format (YUY2, NV12 or RGB32).
ULONG S2cFrameSize(_In_ const KS_VIDEOINFOHEADER* Info);
// Renders one frame of the camera into Dst in the given format (picture or test pattern). FrameNumber animates
// the test pattern.
void S2cRenderFrame(_In_ S2C_CAMERA* Camera, _In_ const KS_VIDEOINFOHEADER* Info, _Out_ PUCHAR Dst, _In_ ULONG DstSize,
                    _In_ ULONGLONG FrameNumber);
