// The interface between the Show2Cam driver and the Show2Cam programs (included by both): a property set on every
// camera filter (open the camera's device interface and send IOCTL_KS_PROPERTY).
#pragma once

//   S2C_PROPERTY_FRAME  (SET)  S2C_FRAME_HEADER + Width*Height BGRA pixels, top-down: the picture to show
//                               (any size, scaled to the negotiated one); Width = 0: back to the test pattern
//   S2C_PROPERTY_STATUS (GET)  S2C_STATUS
//   S2C_PROPERTY_FORMAT (SET)  S2C_FORMAT: the size and frame rate the camera offers; only while no program has
//                               the camera open (STATUS_DEVICE_BUSY otherwise). Kept in the driver settings.
//   S2C_PROPERTY_NAME   (SET)  S2C_NAME: the camera's name in Windows (FriendlyName of its device interfaces);
//                               empty = "Show2Cam Camera N". Kept in the driver settings.
// {6F1C2A9E-3B57-4E0C-9D1A-5C2E7B3F8A41}
#define STATIC_PROPSETID_Show2Cam 0x6f1c2a9e, 0x3b57, 0x4e0c, 0x9d, 0x1a, 0x5c, 0x2e, 0x7b, 0x3f, 0x8a, 0x41
enum { S2C_PROPERTY_FRAME = 0, S2C_PROPERTY_STATUS = 1, S2C_PROPERTY_FORMAT = 2, S2C_PROPERTY_NAME = 3 };
#define S2C_NAME_CHARS 64
#define S2C_MIN_WIDTH  160
#define S2C_MIN_HEIGHT 120
#define S2C_MAX_WIDTH  3840
#define S2C_MAX_HEIGHT 2160
#define S2C_MAX_FPS    60
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
    ULONG PinsOpen;                     // programs that have the camera open (streaming or not)
    ULONGLONG FramesDelivered;
    ULONGLONG FramesDropped;
    ULONGLONG PicturesReceived;
};

struct S2C_FORMAT
{
    ULONG Width, Height, Fps;           // even, S2C_MIN..S2C_MAX; 1..S2C_MAX_FPS
    ULONG Reserved;                     // 0
};

struct S2C_NAME
{
    WCHAR Name[S2C_NAME_CHARS];         // zero-terminated
};

