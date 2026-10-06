// One virtual camera: its AVStream filter (one video capture pin), the formats it offers, the picture it shows
// (from the Show2Cam program, otherwise a test pattern) and the conversion into the negotiated format.
#include "common.h"
#include "log.h"

// GUIDs (the MinGW headers only declare some of them; FOURCC subtypes: {XXXXXXXX-0000-0010-8000-00AA00389B71}).
static const GUID kTypeVideo        = { STATIC_KSDATAFORMAT_TYPE_VIDEO };
static const GUID kSpecVideoInfo    = { STATIC_KSDATAFORMAT_SPECIFIER_VIDEOINFO };
static const GUID kSubYUY2          = { 0x32595559, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID kSubNV12          = { 0x3231564e, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };
static const GUID kSubRGB32         = { 0xe436eb7e, 0x524f, 0x11ce, { 0x9f, 0x53, 0x00, 0x20, 0xaf, 0x0b, 0xa7, 0x70 } };
static const GUID kCategories[3]    = {
    { STATIC_KSCATEGORY_VIDEO },
    { STATIC_KSCATEGORY_CAPTURE },
    { 0xe5323777, 0xf976, 0x4f5b, { 0x9b, 0x55, 0xb9, 0x46, 0x99, 0xc4, 0x6e, 0x44 } },   // KSCATEGORY_VIDEO_CAMERA
};
static const GUID kPinNameCapture   = { STATIC_PINNAME_VIDEO_CAPTURE };
static const GUID kPropSetShow2Cam  = { STATIC_PROPSETID_Show2Cam };
static const GUID kMemoryNonPaged   = { STATIC_KSMEMORY_TYPE_KERNEL_NONPAGED };

// Statistics counters: atomic on x64; x86 has no 64-bit interlocked add, a lost increment there is harmless.
#if defined(_X86_)
#define S2C_ADD64(target, value) ((target) += (value))
#else
#define S2C_ADD64(target, value) InterlockedAdd64(&(target), (value))
#endif

#define FOURCC(a, b, c, d) ((ULONG)(a) | ((ULONG)(b) << 8) | ((ULONG)(c) << 16) | ((ULONG)(d) << 24))

// ---------------------------------------------------------------------------
// Pin: streaming state

struct S2C_PIN
{
    S2C_CAMERA*         Camera;
    KTIMER              Timer;          // periodic, twice per frame (classic timer + DPC: the high-resolution EX_TIMER
    KDPC                Dpc;            //  stopped firing at times, after the run's first frame - programs then hung)
    LARGE_INTEGER       RunStart;       // QPC when the pin went to run
    LARGE_INTEGER       QpcFrequency;
    ULONGLONG           FramesDone;     // frames delivered or dropped since RunStart
    ULONGLONG           Interval;       // frame interval the program chose (100-ns units)
    ULONG               Pid;            // the process that opened the pin
    BOOLEAN             FirstLogged;    // the run's first frame was logged
    BOOLEAN             SmallLogged;    // a too small buffer was logged
    ULONGLONG           LastBuffer;     // QPC time (100 ns) of the last buffer from the client
    BOOLEAN             StarveLogged;   // "no buffer for 5 s" was logged
    BOOLEAN             Running;
    // Stall diagnostics (a program waiting for frames that do not come): checked by the timer itself.
    volatile LONG       TimerFires, ProcessCalls;
    volatile LONGLONG   LastProcess, LastFrame;    // QPC time (100 ns since RunStart)
    BOOLEAN             StallLogged;
    BOOLEAN             KickLogged;     // the status watchdog restarted processing (logged once until frames come)
    LONG                FiresSeen;      // TimerFires when the watchdog looked last
};

static ULONGLONG QpcNow100ns(S2C_PIN* p)
{
    LARGE_INTEGER now = KeQueryPerformanceCounter(nullptr);
    ULONGLONG ticks = (ULONGLONG)(now.QuadPart - p->RunStart.QuadPart);
    ULONGLONG f = (ULONGLONG)p->QpcFrequency.QuadPart;
    return (ticks / f) * 10000000ULL + ((ticks % f) * 10000000ULL) / f;
}

static VOID NTAPI FrameTimer(_In_ PKDPC Dpc, _In_opt_ PVOID Context, _In_opt_ PVOID Arg1, _In_opt_ PVOID Arg2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(Arg1);
    UNREFERENCED_PARAMETER(Arg2);
    PKSPIN pin = (PKSPIN)Context;
    S2C_PIN* p = (S2C_PIN*)pin->Context;
    if (p && p->Running)
    {
        InterlockedIncrement(&p->TimerFires);
        // No frame for 3 s while running: what the pin is waiting for (logged once until frames come again).
        ULONGLONG now = QpcNow100ns(p);
        if (!p->StallLogged && now > 30000000ULL && now - (ULONGLONG)p->LastFrame > 30000000ULL)
        {
            p->StallLogged = TRUE;
            S2cLog("Camera %lu: no frame for 3 s to process %lu: timer fired %ld, Process called %ld (last %llu ms ago), "
                   "last buffer %llu ms ago, frames %llu", p->Camera->Index + 1, p->Pid, p->TimerFires, p->ProcessCalls,
                   (now - (ULONGLONG)p->LastProcess) / 10000, (now - p->LastBuffer) / 10000, p->FramesDone);
            S2cLogFlush();
        }
    }
    KsPinAttemptProcessing(pin, TRUE);     // Process runs at PASSIVE_LEVEL on a worker
}

static NTSTATUS S2C_CB PinCreate(_In_ PKSPIN Pin, _In_ PIRP Irp)
{
    S2C_CAMERA* camera = S2cCameraFromPin(Pin);
    if (!camera) return STATUS_INVALID_DEVICE_STATE;
    S2C_PIN* p = (S2C_PIN*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(S2C_PIN), S2C_POOLTAG);
    if (!p) return STATUS_INSUFFICIENT_RESOURCES;
    p->Camera = camera;
    KeInitializeTimerEx(&p->Timer, NotificationTimer);
    KeInitializeDpc(&p->Dpc, FrameTimer, Pin);
    // Video capture clients expect KS_FRAME_INFO after every stream header.
    Pin->StreamHeaderSize = sizeof(KSSTREAM_HEADER) + sizeof(KS_FRAME_INFO);
    Pin->Context = p;
    InterlockedIncrement(&camera->PinsOpen);
    p->Pid = IoGetRequestorProcessId(Irp);
    // The program's file name (the panel cannot read it for system / protected processes, e.g. a DLP agent).
    WCHAR exe[32] = {};
    PEPROCESS proc = nullptr;
    if (NT_SUCCESS(PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)p->Pid, &proc)))
    {
        PUNICODE_STRING image = nullptr;
        if (NT_SUCCESS(SeLocateProcessImageName(proc, &image)) && image)
        {
            USHORT chars = image->Length / sizeof(WCHAR), start = chars;
            while (start > 0 && image->Buffer[start - 1] != L'\\') start--;
            USHORT n = (USHORT)(chars - start) < 31 ? (USHORT)(chars - start) : 31;
            RtlCopyMemory(exe, image->Buffer + start, n * sizeof(WCHAR));
            exe[n] = 0;
            ExFreePool(image);
        }
        ObDereferenceObject(proc);
    }
    ExAcquireFastMutex(&camera->Lock);
    for (ULONG i = 0; i < S2C_MAX_USERS; i++)
        if (!camera->UserPids[i])
        {
            camera->UserPids[i] = p->Pid;
            RtlCopyMemory(camera->UserNames[i], exe, sizeof(exe));
            break;
        }
    for (ULONG i = 0; i < S2C_MAX_USERS; i++)
        if (!camera->Pins[i])
        {
            camera->Pins[i] = Pin;           // for the status watchdog (S2cCameraWatchdog)
            break;
        }
    ExReleaseFastMutex(&camera->Lock);
    S2cLog("Camera %lu: pin opened by process %lu (%ls)", camera->Index + 1, p->Pid, exe[0] ? exe : L"?");
    S2cLogFlush();
    return STATUS_SUCCESS;
}

static NTSTATUS S2C_CB PinClose(_In_ PKSPIN Pin, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(Irp);
    S2C_PIN* p = (S2C_PIN*)Pin->Context;
    if (p)
    {
        ExAcquireFastMutex(&p->Camera->Lock);
        for (ULONG i = 0; i < S2C_MAX_USERS; i++)
            if (p->Camera->Pins[i] == Pin) p->Camera->Pins[i] = nullptr;
        ExReleaseFastMutex(&p->Camera->Lock);
        p->Running = FALSE;
        KeCancelTimer(&p->Timer);
        KeFlushQueuedDpcs();                // a DPC already queued has run before the context goes
        if (p->Running) InterlockedDecrement(&p->Camera->Streaming);
        InterlockedDecrement(&p->Camera->PinsOpen);
        ExAcquireFastMutex(&p->Camera->Lock);
        for (ULONG i = 0; i < S2C_MAX_USERS; i++)
            if (p->Camera->UserPids[i] == p->Pid)
            {
                p->Camera->UserPids[i] = 0;
                p->Camera->UserNames[i][0] = 0;
                break;
            }
        ExReleaseFastMutex(&p->Camera->Lock);
        S2cLog("Camera %lu: pin closed", p->Camera->Index + 1);
        ExFreePoolWithTag(p, S2C_POOLTAG);
        Pin->Context = nullptr;
    }
    S2cLogFlush();
    return STATUS_SUCCESS;
}

static NTSTATUS S2C_CB PinSetDeviceState(_In_ PKSPIN Pin, _In_ KSSTATE ToState, _In_ KSSTATE FromState)
{
    S2C_PIN* p = (S2C_PIN*)Pin->Context;
    if (!p) return STATUS_SUCCESS;
    S2C_CAMERA* c = p->Camera;
    if (ToState == KSSTATE_RUN && !p->Running)
    {
        // The frame rate the program chose (within what the camera offers), else the camera's own.
        const KS_VIDEOINFOHEADER* vih = &((const KS_DATAFORMAT_VIDEOINFOHEADER*)Pin->ConnectionFormat)->VideoInfoHeader;
        LONGLONG period = vih->AvgTimePerFrame;
        if (period < S2C_MIN_INTERVAL || period > S2C_MAX_INTERVAL) period = 10000000LL / (c->Fps ? c->Fps : 30);
        p->Interval = (ULONGLONG)period;
        p->RunStart = KeQueryPerformanceCounter(&p->QpcFrequency);
        p->FramesDone = 0;
        p->TimerFires = p->ProcessCalls = 0;
        p->LastFrame = p->LastProcess = 0;
        p->StallLogged = FALSE;
        p->Running = TRUE;
        InterlockedIncrement(&c->Streaming);
        LARGE_INTEGER due;
        due.QuadPart = -period;
        LONG ms = (LONG)(period / 20000);     // twice per frame (Process delivers only frames that are due)
        KeSetTimerEx(&p->Timer, due, ms < 1 ? 1 : ms, &p->Dpc);
    }
    else if (ToState != KSSTATE_RUN && p->Running)
    {
        KeCancelTimer(&p->Timer);
        p->Running = FALSE;
        InterlockedDecrement(&c->Streaming);
    }
    S2cLog("Camera %lu: state %d -> %d (process %lu)", c->Index + 1, (int)FromState, (int)ToState, p->Pid);
    if (ToState == KSSTATE_RUN)
    {
        p->FirstLogged = p->SmallLogged = p->StarveLogged = p->StallLogged = FALSE;
        p->LastBuffer = 0;
        p->LastFrame = p->LastProcess = 0;
        p->TimerFires = p->ProcessCalls = 0;
    }
    S2cLogFlush();
    return STATUS_SUCCESS;
}

// Accepts only the formats of this camera (the framework already picked the matching range).
static NTSTATUS S2C_CB PinSetDataFormat(_In_ PKSPIN Pin, _In_opt_ PKSDATAFORMAT OldFormat, _In_opt_ PKSMULTIPLE_ITEM OldAttributeList,
                                        _In_ const KSDATARANGE* DataRange, _In_opt_ const KSATTRIBUTE_LIST* AttributeRange)
{
    UNREFERENCED_PARAMETER(OldFormat);
    UNREFERENCED_PARAMETER(OldAttributeList);
    UNREFERENCED_PARAMETER(AttributeRange);
    const KSDATAFORMAT* f = Pin->ConnectionFormat;
    if (!f || f->FormatSize < sizeof(KS_DATAFORMAT_VIDEOINFOHEADER) || !IsEqualGUID(f->Specifier, kSpecVideoInfo))
        return STATUS_NO_MATCH;
    UNREFERENCED_PARAMETER(DataRange);
    const KS_VIDEOINFOHEADER* vih = &((const KS_DATAFORMAT_VIDEOINFOHEADER*)f)->VideoInfoHeader;
    LONG h = vih->bmiHeader.biHeight < 0 ? -vih->bmiHeader.biHeight : vih->bmiHeader.biHeight;
    S2C_CAMERA* c = S2cCameraFromPin(Pin);
    if (!c) return STATUS_NO_MATCH;
    // One of our formats (any of the sizes: the framework hands over the first range of the subtype only);
    // biSizeImage may be 0 (uncompressed formats): the frame size is computed from width, height and format.
    BOOLEAN found = FALSE;
    for (ULONG i = 0; i < c->RangeCount && !found; i++)
    {
        const KS_DATARANGE_VIDEO* r = &c->Ranges[i];
        found = IsEqualGUID(f->SubFormat, r->DataRange.SubFormat) && vih->bmiHeader.biWidth == r->VideoInfoHeader.bmiHeader.biWidth &&
                h == r->VideoInfoHeader.bmiHeader.biHeight && vih->bmiHeader.biCompression == r->VideoInfoHeader.bmiHeader.biCompression;
    }
    if (!found || (vih->AvgTimePerFrame && (vih->AvgTimePerFrame < S2C_MIN_INTERVAL || vih->AvgTimePerFrame > S2C_MAX_INTERVAL)))
    {
        S2cLog("Camera %lu: format %ldx%ld refused (not offered)", c->Index + 1, vih->bmiHeader.biWidth, h);
        return STATUS_NO_MATCH;
    }
    S2cLog("Camera %lu: format %lux%lu %.4s %lu fps", c->Index + 1, (ULONG)vih->bmiHeader.biWidth, (ULONG)h,
           vih->bmiHeader.biCompression ? (const char*)&vih->bmiHeader.biCompression : "RGB",
           vih->AvgTimePerFrame ? (ULONG)(10000000LL / vih->AvgTimePerFrame) : c->Fps);
    return STATUS_SUCCESS;
}

// Delivers the frames that became due since the run started (one per call; the ones skipped count as dropped).
static NTSTATUS S2C_CB PinProcess(_In_ PKSPIN Pin)
{
    S2C_PIN* p = (S2C_PIN*)Pin->Context;
    if (!p || !p->Running) return STATUS_SUCCESS;
    S2C_CAMERA* c = p->Camera;
    ULONGLONG interval = p->Interval ? p->Interval : 10000000ULL / (c->Fps ? c->Fps : 30);
    ULONGLONG now = QpcNow100ns(p);
    InterlockedIncrement(&p->ProcessCalls);
    p->LastProcess = (LONGLONG)now;
    ULONGLONG due = now / interval + 1;                             // frames that should exist by now
    if (p->FramesDone >= due) return STATUS_SUCCESS;

    PKSSTREAM_POINTER leading = KsPinGetLeadingEdgeStreamPointer(Pin, KSSTREAM_POINTER_STATE_LOCKED);
    if (!leading)
    {
        // no buffer from the client right now; a client that gives none for 5 s is logged once (it gets no frames)
        if (!p->StarveLogged && now - p->LastBuffer > 50000000ULL)
        {
            p->StarveLogged = TRUE;
            S2cLog("Camera %lu: no buffer from process %lu for 5 s (frames due: %llu)", c->Index + 1, p->Pid, due);
            S2cLogFlush();
        }
        return STATUS_SUCCESS;
    }
    p->LastBuffer = now;

    if (due - p->FramesDone > 1)
    {
        S2C_ADD64(c->FramesDropped, (LONGLONG)(due - p->FramesDone - 1));
        p->FramesDone = due - 1;
    }

    const KS_VIDEOINFOHEADER* vih = &((const KS_DATAFORMAT_VIDEOINFOHEADER*)Pin->ConnectionFormat)->VideoInfoHeader;
    ULONG size = S2cFrameSize(vih);
    PKSSTREAM_HEADER header = leading->StreamHeader;
    ULONG used = 0;
    if (leading->OffsetOut.Remaining >= size)
    {
        S2cRenderFrame(c, vih, leading->OffsetOut.Data, size, p->FramesDone);
        used = size;
    }
    else if (!p->SmallLogged)
    {
        p->SmallLogged = TRUE;
        S2cLog("Camera %lu: buffer of process %lu too small (%lu < %lu bytes): empty frames", c->Index + 1, p->Pid,
               leading->OffsetOut.Remaining, size);
        S2cLogFlush();
    }
    if (!p->FirstLogged)
    {
        p->FirstLogged = TRUE;
        S2cLog("Camera %lu: first frame to process %lu: %lu of %lu bytes, %llu ms after run (timer fired %ld, Process called %ld)",
               c->Index + 1, p->Pid, used, leading->OffsetOut.Remaining, now / 10000, p->TimerFires, p->ProcessCalls);
        S2cLogFlush();
    }
    header->PresentationTime.Time = (LONGLONG)(p->FramesDone * interval);
    header->PresentationTime.Numerator = 1;
    header->PresentationTime.Denominator = 1;
    header->Duration = (LONGLONG)interval;
    header->OptionsFlags = KSSTREAM_HEADER_OPTIONSF_TIMEVALID | KSSTREAM_HEADER_OPTIONSF_DURATIONVALID |
                           KSSTREAM_HEADER_OPTIONSF_SPLICEPOINT;
    if (header->Size >= sizeof(KSSTREAM_HEADER) + sizeof(KS_FRAME_INFO))
    {
        PKS_FRAME_INFO fi = (PKS_FRAME_INFO)(header + 1);
        fi->ExtendedHeaderSize = sizeof(KS_FRAME_INFO);
        fi->dwFrameFlags = KS_VIDEO_FLAG_FRAME;
        fi->PictureNumber = (LONGLONG)p->FramesDone;
        fi->DropCount = c->FramesDropped;
    }
    p->FramesDone++;
    p->LastFrame = (LONGLONG)now;
    if (p->StallLogged || p->KickLogged)
    {
        p->StallLogged = p->KickLogged = FALSE;
        S2cLog("Camera %lu: frames to process %lu again", c->Index + 1, p->Pid);
        S2cLogFlush();
    }
    S2C_ADD64(c->FramesDelivered, 1);
    KsStreamPointerAdvanceOffsetsAndUnlock(leading, 0, used, TRUE);  // TRUE: hand the buffer back with this frame
    return STATUS_SUCCESS;
}

// Format negotiation: the caller's range against one of ours -> our exact KS_DATAFORMAT_VIDEOINFOHEADER.
static NTSTATUS S2C_CB PinIntersect(_In_ PVOID Context, _In_ PIRP Irp, _In_ PKSP_PIN PinInstance, _In_ PKSDATARANGE CallerRange,
                                    _In_ PKSDATARANGE OurRange, _In_ ULONG BufferSize, _Out_opt_ PVOID Data, _Out_ PULONG DataSize)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(PinInstance);
    // The caller's range may be a KS_DATARANGE_VIDEO, a KS_DATAFORMAT_VIDEOINFOHEADER (a program asking for one
    // format, e.g. DirectShow SetFormat) or a plain KSDATARANGE (GUIDs only, the framework already matched them).
    // A size that is not this range's is refused (a camera that "accepts" a size it cannot deliver gives programs no
    // picture); a frame rate within 1..60 fps is taken.
    if (!IsEqualGUID(CallerRange->Specifier, kSpecVideoInfo)) return STATUS_NO_MATCH;
    const KS_DATARANGE_VIDEO* ours = (const KS_DATARANGE_VIDEO*)OurRange;
    const KS_VIDEOINFOHEADER* want = nullptr;
    if (CallerRange->FormatSize >= sizeof(KS_DATARANGE_VIDEO))
        want = &((const KS_DATARANGE_VIDEO*)CallerRange)->VideoInfoHeader;
    else if (CallerRange->FormatSize >= sizeof(KS_DATAFORMAT_VIDEOINFOHEADER))
        want = &((const KS_DATAFORMAT_VIDEOINFOHEADER*)CallerRange)->VideoInfoHeader;
    LONGLONG interval = ours->VideoInfoHeader.AvgTimePerFrame;
    if (want)
    {
        LONG wh = want->bmiHeader.biHeight < 0 ? -want->bmiHeader.biHeight : want->bmiHeader.biHeight;
        if (want->bmiHeader.biWidth && (want->bmiHeader.biWidth != ours->VideoInfoHeader.bmiHeader.biWidth ||
                                         wh != ours->VideoInfoHeader.bmiHeader.biHeight))
            return STATUS_NO_MATCH;
        if (want->AvgTimePerFrame >= S2C_MIN_INTERVAL && want->AvgTimePerFrame <= S2C_MAX_INTERVAL) interval = want->AvgTimePerFrame;
    }
    ULONG size = sizeof(KS_DATAFORMAT_VIDEOINFOHEADER);
    if (BufferSize == 0)
    {
        *DataSize = size;
        return STATUS_BUFFER_OVERFLOW;
    }
    if (BufferSize < size || !Data) return STATUS_BUFFER_TOO_SMALL;
    KS_DATAFORMAT_VIDEOINFOHEADER* f = (KS_DATAFORMAT_VIDEOINFOHEADER*)Data;
    RtlCopyMemory(&f->DataFormat, &ours->DataRange, sizeof(KSDATAFORMAT));
    f->DataFormat.FormatSize = size;
    f->DataFormat.SampleSize = ours->VideoInfoHeader.bmiHeader.biSizeImage;
    RtlCopyMemory(&f->VideoInfoHeader, &ours->VideoInfoHeader, sizeof(KS_VIDEOINFOHEADER));
    f->VideoInfoHeader.AvgTimePerFrame = interval;
    *DataSize = size;
    return STATUS_SUCCESS;
}

static const KSPIN_DISPATCH kPinDispatch = {
    (PFNKSPINIRP)PinCreate, (PFNKSPINIRP)PinClose, (PFNKSPIN)PinProcess, nullptr,
    (PFNKSPINSETDATAFORMAT)PinSetDataFormat, (PFNKSPINSETDEVICESTATE)PinSetDeviceState, nullptr, nullptr, nullptr, nullptr
};

// ---------------------------------------------------------------------------
// Filter: the Show2Cam property set

static S2C_CAMERA* CameraFromIrp(PIRP Irp)
{
    PKSFILTER filter = KsGetFilterFromIrp(Irp);
    return filter ? S2cCameraFromFilter(filter) : nullptr;
}

static ULONG PropertyDataSize(PIRP Irp)
{
    return IoGetCurrentIrpStackLocation(Irp)->Parameters.DeviceIoControl.OutputBufferLength;
}

static NTSTATUS S2C_CB SetFrame(_In_ PIRP Irp, _In_ PKSIDENTIFIER Request, _Inout_ PVOID Data)
{
    UNREFERENCED_PARAMETER(Request);
    S2C_CAMERA* c = CameraFromIrp(Irp);
    ULONG size = PropertyDataSize(Irp);
    const S2C_FRAME_HEADER* h = (const S2C_FRAME_HEADER*)Data;
    if (!c || size < sizeof(S2C_FRAME_HEADER) || h->Magic != S2C_FRAME_MAGIC) return STATUS_INVALID_PARAMETER;
    ULONG* copy = nullptr;
    if (h->Width && h->Height)
    {
        if (h->Width > 3840 || h->Height > 2160 || (ULONGLONG)h->Width * h->Height * 4 > size - sizeof(S2C_FRAME_HEADER))
            return STATUS_INVALID_PARAMETER;
        SIZE_T bytes = (SIZE_T)h->Width * h->Height * 4;
        copy = (ULONG*)ExAllocatePool2(POOL_FLAG_NON_PAGED, bytes, S2C_POOLTAG);
        if (!copy) return STATUS_INSUFFICIENT_RESOURCES;
        RtlCopyMemory(copy, h + 1, bytes);
    }
    ExAcquireFastMutex(&c->Lock);
    ULONG* old = c->Picture;
    BOOLEAN sizeChanged = !copy || c->PictureWidth != h->Width || c->PictureHeight != h->Height;
    c->Picture = copy;
    c->PictureWidth = copy ? h->Width : 0;
    c->PictureHeight = copy ? h->Height : 0;
    ExReleaseFastMutex(&c->Lock);
    if (old) ExFreePoolWithTag(old, S2C_POOLTAG);
    S2C_ADD64(c->PicturesReceived, 1);
    if (sizeChanged)
    {
        S2cLog("Camera %lu: picture %lux%lu%s", c->Index + 1, h->Width, h->Height, copy ? "" : " (test pattern)");
        S2cLogFlush();
    }
    return STATUS_SUCCESS;
}

// Called with every status request (the panel asks twice a second): a running pin that got no frame for a while is
// processed again (and its timer re-armed when it stopped firing), logged once with what it was waiting for.
static void S2cCameraWatchdog(S2C_CAMERA* c)
{
    ExAcquireFastMutex(&c->Lock);
    for (ULONG i = 0; i < S2C_MAX_USERS; i++)
    {
        PKSPIN pin = c->Pins[i];
        S2C_PIN* p = pin ? (S2C_PIN*)pin->Context : nullptr;
        if (!p || !p->Running) continue;
        ULONGLONG interval = p->Interval ? p->Interval : 10000000ULL / (c->Fps ? c->Fps : 30);
        ULONGLONG now = QpcNow100ns(p);
        ULONGLONG quiet = 4 * interval > 10000000ULL ? 4 * interval : 10000000ULL;        // 4 frames, at least 1 s
        if (now < quiet || now - (ULONGLONG)p->LastFrame < quiet) continue;
        LONG fires = p->TimerFires;
        bool timerDead = fires == p->FiresSeen;
        p->FiresSeen = fires;
        if (!p->KickLogged)
        {
            p->KickLogged = TRUE;
            S2cLog("Camera %lu: no frame for %llu ms to process %lu (timer fired %ld%s, Process called %ld, last buffer %llu ms "
                   "ago): processing restarted", c->Index + 1, (now - (ULONGLONG)p->LastFrame) / 10000, p->Pid, fires,
                   timerDead ? " - stopped" : "", p->ProcessCalls, (now - p->LastBuffer) / 10000);
            S2cLogFlush();
        }
        if (timerDead)
        {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)interval;
            LONG ms = (LONG)(interval / 20000);
            KeSetTimerEx(&p->Timer, due, ms < 1 ? 1 : ms, &p->Dpc);
        }
        KsPinAttemptProcessing(pin, TRUE);
    }
    ExReleaseFastMutex(&c->Lock);
}

static NTSTATUS S2C_CB GetLog(_In_ PIRP Irp, _In_ PKSIDENTIFIER Request, _Inout_ PVOID Data)
{
    UNREFERENCED_PARAMETER(Request);
    if (PropertyDataSize(Irp) < sizeof(S2C_LOG)) return STATUS_BUFFER_TOO_SMALL;
    S2C_LOG* l = (S2C_LOG*)Data;
    l->Length = S2cLogCopy(l->Text, S2C_LOG_MAX, &l->Generation);
    Irp->IoStatus.Information = sizeof(S2C_LOG);
    return STATUS_SUCCESS;
}

static NTSTATUS S2C_CB GetStatus(_In_ PIRP Irp, _In_ PKSIDENTIFIER Request, _Inout_ PVOID Data)
{
    UNREFERENCED_PARAMETER(Request);
    S2C_CAMERA* c = CameraFromIrp(Irp);
    if (!c || PropertyDataSize(Irp) < sizeof(S2C_STATUS)) return STATUS_INVALID_PARAMETER;
    S2cCameraWatchdog(c);
    S2C_STATUS* s = (S2C_STATUS*)Data;
    RtlZeroMemory(s, sizeof(*s));
    s->Index = c->Index;
    s->Width = c->Width;
    s->Height = c->Height;
    s->Fps = c->Fps;
    s->Streaming = (ULONG)c->Streaming;
    s->SourceWidth = c->PictureWidth;
    s->SourceHeight = c->PictureHeight;
    s->PinsOpen = (ULONG)c->PinsOpen;
    ExAcquireFastMutex(&c->Lock);
    RtlCopyMemory(s->UserPids, c->UserPids, sizeof(s->UserPids));
    RtlCopyMemory(s->UserNames, c->UserNames, sizeof(s->UserNames));
    ExReleaseFastMutex(&c->Lock);
    s->FramesDelivered = (ULONGLONG)c->FramesDelivered;
    s->FramesDropped = (ULONGLONG)c->FramesDropped;
    s->PicturesReceived = (ULONGLONG)c->PicturesReceived;
    Irp->IoStatus.Information = sizeof(S2C_STATUS);
    return STATUS_SUCCESS;
}

static void SetRanges(S2C_CAMERA* c, ULONG Width, ULONG Height, ULONG Fps);
static void ApplyName(S2C_CAMERA* c, PCWSTR Name);

static void SaveDword(S2C_CAMERA* c, PCWSTR What, ULONG Value)
{
    WCHAR name[32];
    RtlStringCchPrintfW(name, 32, L"Camera%lu%ls", c->Index + 1, What);
    RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, L"Show2Cam\\Parameters", name, REG_DWORD, &Value, sizeof(Value));
}

static NTSTATUS S2C_CB SetFormat(_In_ PIRP Irp, _In_ PKSIDENTIFIER Request, _Inout_ PVOID Data)
{
    UNREFERENCED_PARAMETER(Request);
    S2C_CAMERA* c = CameraFromIrp(Irp);
    if (!c || PropertyDataSize(Irp) < sizeof(S2C_FORMAT)) return STATUS_INVALID_PARAMETER;
    const S2C_FORMAT* f = (const S2C_FORMAT*)Data;
    if (f->Width < S2C_MIN_WIDTH || f->Width > S2C_MAX_WIDTH || f->Height < S2C_MIN_HEIGHT || f->Height > S2C_MAX_HEIGHT ||
        (f->Width & 1) || (f->Height & 1) || f->Fps < 1 || f->Fps > S2C_MAX_FPS)
        return STATUS_INVALID_PARAMETER;
    if (f->Width == c->Width && f->Height == c->Height && f->Fps == c->Fps) return STATUS_SUCCESS;
    // The formats are read by every program that opens the camera: they change only while nobody has it open.
    if (c->PinsOpen > 0)
    {
        S2cLog("Camera %lu: format %lux%lu %lu fps refused, the camera is in use", c->Index + 1, f->Width, f->Height, f->Fps);
        S2cLogFlush();
        return STATUS_DEVICE_BUSY;
    }
    SetRanges(c, f->Width, f->Height, f->Fps);
    NTSTATUS cache = KsFilterFactoryUpdateCacheData(c->Factory, nullptr);
    SaveDword(c, L"Width", f->Width);
    SaveDword(c, L"Height", f->Height);
    SaveDword(c, L"Fps", f->Fps);
    S2cLog("Camera %lu: format set to %lux%lu %lu fps (cache 0x%08lX)", c->Index + 1, f->Width, f->Height, f->Fps, (ULONG)cache);
    S2cLogFlush();
    return STATUS_SUCCESS;
}

static NTSTATUS S2C_CB SetName(_In_ PIRP Irp, _In_ PKSIDENTIFIER Request, _Inout_ PVOID Data)
{
    UNREFERENCED_PARAMETER(Request);
    S2C_CAMERA* c = CameraFromIrp(Irp);
    if (!c || PropertyDataSize(Irp) < sizeof(S2C_NAME)) return STATUS_INVALID_PARAMETER;
    WCHAR name[S2C_NAME_CHARS];
    RtlCopyMemory(name, ((const S2C_NAME*)Data)->Name, sizeof(name));
    name[S2C_NAME_CHARS - 1] = 0;
    WCHAR value[32];
    RtlStringCchPrintfW(value, 32, L"Camera%luName", c->Index + 1);
    if (name[0])
        RtlWriteRegistryValue(RTL_REGISTRY_SERVICES, L"Show2Cam\\Parameters", value, REG_SZ, name,
                              (ULONG)((wcslen(name) + 1) * sizeof(WCHAR)));
    else
        RtlDeleteRegistryValue(RTL_REGISTRY_SERVICES, L"Show2Cam\\Parameters", value);
    ApplyName(c, name);
    S2cLogFlush();
    return STATUS_SUCCESS;
}

static const KSPROPERTY_ITEM kProperties[] = {
    { S2C_PROPERTY_FRAME, { nullptr }, sizeof(KSPROPERTY), sizeof(S2C_FRAME_HEADER), { (PFNKSHANDLER)SetFrame },
      nullptr, 0, nullptr, nullptr, 0 },
    { S2C_PROPERTY_STATUS, { (PFNKSHANDLER)GetStatus }, sizeof(KSPROPERTY), sizeof(S2C_STATUS), { nullptr },
      nullptr, 0, nullptr, nullptr, 0 },
    { S2C_PROPERTY_FORMAT, { nullptr }, sizeof(KSPROPERTY), sizeof(S2C_FORMAT), { (PFNKSHANDLER)SetFormat },
      nullptr, 0, nullptr, nullptr, 0 },
    { S2C_PROPERTY_NAME, { nullptr }, sizeof(KSPROPERTY), sizeof(S2C_NAME), { (PFNKSHANDLER)SetName },
      nullptr, 0, nullptr, nullptr, 0 },
    { S2C_PROPERTY_LOG, { (PFNKSHANDLER)GetLog }, sizeof(KSPROPERTY), sizeof(S2C_LOG), { nullptr },
      nullptr, 0, nullptr, nullptr, 0 },
};
static const KSPROPERTY_SET kPropertySets[] = {
    { &kPropSetShow2Cam, sizeof(kProperties) / sizeof(kProperties[0]), kProperties, 0, nullptr },
};
static const KSAUTOMATION_TABLE kFilterAutomation = {
    1, sizeof(KSPROPERTY_ITEM), kPropertySets, 0, 0, nullptr, 0, 0, nullptr        // (x86: Alignment = 0)
};

// ---------------------------------------------------------------------------
// Camera

S2C_CAMERA* S2cCameraFromFilter(_In_ PKSFILTER Filter)
{
    S2C_CAMERA* c = CONTAINING_RECORD(Filter->Descriptor, S2C_CAMERA, FilterDescriptor);
    return c->Signature == S2C_CAMERA_SIGNATURE ? c : nullptr;
}

S2C_CAMERA* S2cCameraFromPin(_In_ PKSPIN Pin)
{
    S2C_CAMERA* c = CONTAINING_RECORD(Pin->Descriptor, S2C_CAMERA, PinDescriptor);
    return c->Signature == S2C_CAMERA_SIGNATURE ? c : nullptr;
}

static void InitRange(KS_DATARANGE_VIDEO* r, const GUID& sub, ULONG fourcc, USHORT bits, ULONG w, ULONG h, ULONG fps)
{
    RtlZeroMemory(r, sizeof(*r));
    ULONG size = (ULONG)((ULONGLONG)w * h * bits / 8);
    LONGLONG interval = 10000000LL / fps;
    r->DataRange.FormatSize = sizeof(KS_DATARANGE_VIDEO);
    r->DataRange.SampleSize = size;
    r->DataRange.MajorFormat = kTypeVideo;
    r->DataRange.SubFormat = sub;
    r->DataRange.Specifier = kSpecVideoInfo;
    r->bFixedSizeSamples = TRUE;
    r->bTemporalCompression = FALSE;
    r->StreamDescriptionFlags = KS_VIDEOSTREAM_CAPTURE;
    r->MemoryAllocationFlags = 0;

    KS_VIDEO_STREAM_CONFIG_CAPS* cc = &r->ConfigCaps;
    cc->guid = kSpecVideoInfo;
    cc->VideoStandard = KS_AnalogVideo_None;
    cc->InputSize.cx = (LONG)w; cc->InputSize.cy = (LONG)h;
    cc->MinCroppingSize = cc->InputSize;
    cc->MaxCroppingSize = cc->InputSize;
    cc->CropGranularityX = cc->CropGranularityY = 1;
    cc->CropAlignX = cc->CropAlignY = 1;
    cc->MinOutputSize = cc->InputSize;
    cc->MaxOutputSize = cc->InputSize;
    cc->OutputGranularityX = cc->OutputGranularityY = 1;
    cc->MinFrameInterval = S2C_MIN_INTERVAL;       // programs may choose 1..60 fps
    cc->MaxFrameInterval = S2C_MAX_INTERVAL;
    cc->MinBitsPerSecond = (LONG)((ULONGLONG)size * 8 * fps > 0x7fffffff ? 0x7fffffff : (ULONGLONG)size * 8 * fps);
    cc->MaxBitsPerSecond = cc->MinBitsPerSecond;

    KS_VIDEOINFOHEADER* v = &r->VideoInfoHeader;
    v->rcSource.right = v->rcTarget.right = (LONG)w;
    v->rcSource.bottom = v->rcTarget.bottom = (LONG)h;
    v->dwBitRate = (DWORD)cc->MaxBitsPerSecond;
    v->AvgTimePerFrame = interval;
    v->bmiHeader.biSize = sizeof(KS_BITMAPINFOHEADER);
    v->bmiHeader.biWidth = (LONG)w;
    v->bmiHeader.biHeight = (LONG)h;         // RGB: bottom-up; YUV: top-down by definition
    v->bmiHeader.biPlanes = 1;
    v->bmiHeader.biBitCount = bits;
    v->bmiHeader.biCompression = fourcc;      // 0 = BI_RGB
    v->bmiHeader.biSizeImage = size;
}

// The formats (YUY2, NV12, RGB32 at one size and rate) and the buffer framing for them.
static void SetRanges(S2C_CAMERA* c, ULONG Width, ULONG Height, ULONG Fps)
{
    c->Width = Width;
    c->Height = Height;
    c->Fps = Fps;
    // The camera's own size first (the default of programs), then the usual webcam sizes: programs often ask for one
    // of those (640x480 above all) and skip a camera without it. The picture is scaled to the size chosen.
    static const ULONG kUsual[][2] = { { 1920, 1080 }, { 1280, 720 }, { 960, 540 }, { 800, 600 }, { 640, 480 }, { 640, 360 }, { 320, 240 } };
    ULONG sizes[S2C_MAX_SIZES][2];
    ULONG n = 0;
    sizes[n][0] = Width;
    sizes[n][1] = Height;
    n++;
    for (ULONG i = 0; i < sizeof(kUsual) / sizeof(kUsual[0]) && n < S2C_MAX_SIZES; i++)
    {
        if (kUsual[i][0] == Width && kUsual[i][1] == Height) continue;
        sizes[n][0] = kUsual[i][0];
        sizes[n][1] = kUsual[i][1];
        n++;
    }
    ULONG maxPixels = 0, minPixels = 0xFFFFFFFF, k = 0;
    for (ULONG i = 0; i < n; i++)
    {
        ULONG w = sizes[i][0], h = sizes[i][1];
        InitRange(&c->Ranges[k++], kSubYUY2, FOURCC('Y', 'U', 'Y', '2'), 16, w, h, Fps);
        InitRange(&c->Ranges[k++], kSubNV12, FOURCC('N', 'V', '1', '2'), 12, w, h, Fps);
        InitRange(&c->Ranges[k++], kSubRGB32, 0, 32, w, h, Fps);
        if (w * h > maxPixels) maxPixels = w * h;
        if (w * h < minPixels) minPixels = w * h;
    }
    c->RangeCount = k;
    for (ULONG i = 0; i < k; i++) c->RangePointers[i] = &c->Ranges[i].DataRange;
    c->PinDescriptor.PinDescriptor.DataRangesCount = k;

    // One frame buffer per sample, system memory, up to the largest format (RGB32 of the largest size).
    KSALLOCATOR_FRAMING_EX* f = &c->Framing;
    RtlZeroMemory(f, sizeof(*f));
    f->CountItems = 1;
    f->OutputCompression.RatioNumerator = 1;
    f->OutputCompression.RatioDenominator = 1;
    f->FramingItem[0].MemoryType = kMemoryNonPaged;
    f->FramingItem[0].Flags = KSALLOCATOR_REQUIREMENTF_SYSTEM_MEMORY | KSALLOCATOR_REQUIREMENTF_PREFERENCES_ONLY;
    f->FramingItem[0].Frames = 3;
    f->FramingItem[0].FileAlignment = FILE_LONG_ALIGNMENT;
    f->FramingItem[0].PhysicalRange.MaxFrameSize = (ULONG)-1;
    f->FramingItem[0].PhysicalRange.Stepping = 1;
    f->FramingItem[0].FramingRange.Range.MinFrameSize = minPixels * 3 / 2;
    f->FramingItem[0].FramingRange.Range.MaxFrameSize = maxPixels * 4;
    f->FramingItem[0].FramingRange.Range.Stepping = 1;
}

// The name on the camera's device interfaces (one per category: video, capture, camera): the "FriendlyName" value
// of each interface's registry key (what the INF set, read by programs listing cameras) and the matching property.
static const DEVPROPKEY kDeviceFriendlyName = { { 0xa45c254e, 0xdf1c, 0x4efd, { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 } }, 14 };
static const DEVPROPKEY kInterfaceFriendlyName = { { 0x026e516e, 0xb814, 0x414b, { 0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22 } }, 2 };

static void ApplyName(S2C_CAMERA* c, PCWSTR Name)
{
    WCHAR fallback[S2C_NAME_CHARS];
    if (!Name || !Name[0])
    {
        RtlStringCchPrintfW(fallback, S2C_NAME_CHARS, L"Show2Cam Camera %lu", c->Index + 1);
        Name = fallback;
    }
    PUNICODE_STRING link = c->Factory ? KsFilterFactoryGetSymbolicLink(c->Factory) : nullptr;
    if (!link || !link->Buffer) return;
    ULONG bytes = (ULONG)((wcslen(Name) + 1) * sizeof(WCHAR));
    UNICODE_STRING valueName;
    RtlInitUnicodeString(&valueName, L"FriendlyName");
    ULONG done = 0;
    for (ULONG i = 0; i < 3; i++)
    {
        UNICODE_STRING alias = {};
        if (!NT_SUCCESS(IoGetDeviceInterfaceAlias(link, &kCategories[i], &alias))) continue;
        HANDLE key;
        if (NT_SUCCESS(IoOpenDeviceInterfaceRegistryKey(&alias, KEY_SET_VALUE, &key)))
        {
            if (NT_SUCCESS(ZwSetValueKey(key, &valueName, 0, REG_SZ, (PVOID)Name, bytes))) done++;
            ZwClose(key);
        }
        IoSetDeviceInterfacePropertyData(&alias, &kInterfaceFriendlyName, 0, 0, DEVPROP_TYPE_STRING, bytes, (PVOID)Name);
        RtlFreeUnicodeString(&alias);
    }
    // The device (one per camera) has the camera's name: some programs (e.g. SearchInform's camera module) take a
    // DirectShow camera only when its name is also the name of a device (as with one-camera drivers, e2eSoft VCam).
    if (c->Pdo)
    {
        NTSTATUS ds = IoSetDevicePropertyData(c->Pdo, &kDeviceFriendlyName, 0, PLUGPLAY_PROPERTY_PERSISTENT, DEVPROP_TYPE_STRING,
                                              bytes, (PVOID)Name);
        S2cLog("Device name \"%ls\" -> 0x%08lX", Name, (ULONG)ds);
    }
    S2cLog("Camera %lu: name \"%ls\" (%lu of 3 interfaces)", c->Index + 1, Name, done);
}

void S2cCameraApplySavedName(_In_ S2C_CAMERA* Camera)
{
    WCHAR value[32], buffer[S2C_NAME_CHARS] = {};
    RtlStringCchPrintfW(value, 32, L"Camera%luName", Camera->Index + 1);
    UNICODE_STRING name = { 0, (USHORT)(sizeof(buffer) - sizeof(WCHAR)), buffer };
    RTL_QUERY_REGISTRY_TABLE q[2];
    RtlZeroMemory(q, sizeof(q));
    q[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_REQUIRED | RTL_QUERY_REGISTRY_TYPECHECK;
    q[0].Name = value;
    q[0].EntryContext = &name;
    q[0].DefaultType = REG_SZ << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT;
    if (NT_SUCCESS(RtlQueryRegistryValues(RTL_REGISTRY_SERVICES, L"Show2Cam\\Parameters", q, nullptr, nullptr)) && buffer[0])
        ApplyName(Camera, buffer);
    else
        ApplyName(Camera, nullptr);         // the default name, also as the device's name
}

NTSTATUS S2cCameraCreate(_In_ ULONG Index, _In_ ULONG Width, _In_ ULONG Height, _In_ ULONG Fps, _Out_ S2C_CAMERA** Camera)
{
    *Camera = nullptr;
    S2C_CAMERA* c = (S2C_CAMERA*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(S2C_CAMERA), S2C_POOLTAG);
    if (!c) return STATUS_INSUFFICIENT_RESOURCES;
    c->Signature = S2C_CAMERA_SIGNATURE;
    c->Index = Index;
    RtlStringCchPrintfW(c->RefString, 16, L"Camera%lu", Index + 1);
    ExInitializeFastMutex(&c->Lock);

    SetRanges(c, Width, Height, Fps);

    KSPIN_DESCRIPTOR_EX* pd = &c->PinDescriptor;
    pd->Dispatch = &kPinDispatch;
    pd->AutomationTable = nullptr;
    pd->PinDescriptor.DataRangesCount = c->RangeCount;
    pd->PinDescriptor.DataRanges = c->RangePointers;
    pd->PinDescriptor.DataFlow = KSPIN_DATAFLOW_OUT;
    pd->PinDescriptor.Communication = KSPIN_COMMUNICATION_BOTH;
    pd->PinDescriptor.Category = &kPinNameCapture;
    pd->PinDescriptor.Name = &kPinNameCapture;
    pd->Flags = KSPIN_FLAG_PROCESS_IN_RUN_STATE_ONLY | KSPIN_FLAG_DO_NOT_INITIATE_PROCESSING;
    pd->InstancesPossible = 1;
    pd->InstancesNecessary = 1;
    pd->AllocatorFraming = &c->Framing;
    pd->IntersectHandler = (PFNKSINTERSECTHANDLEREX)PinIntersect;

    KSFILTER_DESCRIPTOR* fd = &c->FilterDescriptor;
    fd->Dispatch = nullptr;
    fd->AutomationTable = &kFilterAutomation;
    fd->Version = KSFILTER_DESCRIPTOR_VERSION;
    fd->Flags = 0;
    fd->ReferenceGuid = nullptr;
    fd->PinDescriptorsCount = 1;
    fd->PinDescriptorSize = sizeof(KSPIN_DESCRIPTOR_EX);
    fd->PinDescriptors = pd;
    fd->CategoriesCount = 3;
    fd->Categories = kCategories;

    *Camera = c;
    return STATUS_SUCCESS;
}

void S2cCameraFree(_In_ S2C_CAMERA* Camera)
{
    if (Camera->Picture) ExFreePoolWithTag(Camera->Picture, S2C_POOLTAG);
    ExFreePoolWithTag(Camera, S2C_POOLTAG);
}

// ---------------------------------------------------------------------------
// Rendering

// Test pattern: seven colour bars, the camera number as white blocks in the top left corner and a white bar
// moving across the bottom (shows the picture is live).
static ULONG PatternPixel(ULONG x, ULONG y, ULONG w, ULONG h, ULONG index, ULONGLONG frame)
{
    static const ULONG bars[7] = { 0xFFC0C0C0, 0xFFC0C000, 0xFF00C0C0, 0xFF00C000, 0xFFC000C0, 0xFFC00000, 0xFF0000C0 };
    ULONG block = h / 12 ? h / 12 : 1;
    if (y >= block / 2 && y < block / 2 + block)
    {
        ULONG k = (x - block / 2) / (block + block / 2);
        ULONG inK = (x - block / 2) % (block + block / 2);
        if (x >= block / 2 && k <= index && inK < block) return 0xFFFFFFFF;
    }
    if (y >= h - h / 8)
    {
        ULONG pos = (ULONG)((frame * (w / 60 ? w / 60 : 1)) % w);
        return (x >= pos && x < pos + w / 16) ? 0xFFFFFFFF : 0xFF101010;
    }
    return bars[(x * 7) / w];
}

static inline void Yuv(ULONG bgra, int* Y, int* U, int* V)
{
    int b = bgra & 0xFF, g = (bgra >> 8) & 0xFF, r = (bgra >> 16) & 0xFF;
    *Y = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;           // BT.601, limited range
    *U = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
    *V = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
}

ULONG S2cFrameSize(_In_ const KS_VIDEOINFOHEADER* Info)
{
    ULONG w = (ULONG)Info->bmiHeader.biWidth;
    ULONG h = (ULONG)(Info->bmiHeader.biHeight < 0 ? -Info->bmiHeader.biHeight : Info->bmiHeader.biHeight);
    switch (Info->bmiHeader.biCompression)
    {
    case FOURCC('Y', 'U', 'Y', '2'): return w * h * 2;
    case FOURCC('N', 'V', '1', '2'): return w * h * 3 / 2;
    default:                         return w * h * 4;        // RGB32
    }
}

void S2cRenderFrame(_In_ S2C_CAMERA* c, _In_ const KS_VIDEOINFOHEADER* Info, _Out_ PUCHAR Dst, _In_ ULONG DstSize,
                    _In_ ULONGLONG FrameNumber)
{
    ULONG w = (ULONG)Info->bmiHeader.biWidth;
    ULONG h = (ULONG)(Info->bmiHeader.biHeight < 0 ? -Info->bmiHeader.biHeight : Info->bmiHeader.biHeight);
    ULONG fourcc = Info->bmiHeader.biCompression;
    if (!w || !h) return;

    ExAcquireFastMutex(&c->Lock);
    const ULONG* pic = c->Picture;
    ULONG pw = c->PictureWidth, ph = c->PictureHeight;
    // Nearest-neighbour scaling of the picture to the output size (the program sends the camera's own size; a
    // program may have chosen another one). Another aspect ratio fills the frame, cropped at the sides or at top and
    // bottom as real webcams do: black bars would make the picture look dark to programs that check frames for a
    // covered lens (e.g. SearchInform takes a frame with too few bright points as empty).
    ULONG sx = 0, sy = 0, sw = pw, sh = ph;
    if (pic && pw && ph)
    {
        if ((ULONGLONG)pw * h > (ULONGLONG)ph * w) sw = (ULONG)((ULONGLONG)ph * w / h);      // wider: crop the sides
        else sh = (ULONG)((ULONGLONG)pw * h / w);                                           // taller: crop top/bottom
        if (!sw) sw = 1;
        if (!sh) sh = 1;
        sx = (pw - sw) / 2;
        sy = (ph - sh) / 2;
    }
    #define SRC(x, y) (pic ? pic[(SIZE_T)(sy + (y) * sh / h) * pw + sx + (x) * sw / w] \
                           : PatternPixel((x), (y), w, h, c->Index, FrameNumber))

    if (fourcc == FOURCC('Y', 'U', 'Y', '2') && DstSize >= w * h * 2)
    {
        for (ULONG y = 0; y < h; y++)
        {
            PUCHAR d = Dst + (SIZE_T)y * w * 2;
            for (ULONG x = 0; x + 1 < w; x += 2)
            {
                int y0, u0, v0, y1, u1, v1;
                Yuv(SRC(x, y), &y0, &u0, &v0);
                Yuv(SRC(x + 1, y), &y1, &u1, &v1);
                d[0] = (UCHAR)y0; d[1] = (UCHAR)((u0 + u1) / 2); d[2] = (UCHAR)y1; d[3] = (UCHAR)((v0 + v1) / 2);
                d += 4;
            }
        }
    }
    else if (fourcc == FOURCC('N', 'V', '1', '2') && DstSize >= w * h * 3 / 2)
    {
        PUCHAR uv = Dst + (SIZE_T)w * h;
        for (ULONG y = 0; y < h; y++)
        {
            PUCHAR d = Dst + (SIZE_T)y * w;
            for (ULONG x = 0; x < w; x++)
            {
                int Y, U, V;
                Yuv(SRC(x, y), &Y, &U, &V);
                d[x] = (UCHAR)Y;
                if (!(y & 1) && !(x & 1))
                {
                    PUCHAR q = uv + (SIZE_T)(y / 2) * w + x;
                    q[0] = (UCHAR)U;
                    q[1] = (UCHAR)V;
                }
            }
        }
    }
    else if (fourcc == 0 && DstSize >= w * h * 4)
    {
        BOOLEAN bottomUp = Info->bmiHeader.biHeight > 0;
        for (ULONG y = 0; y < h; y++)
        {
            ULONG* d = (ULONG*)(Dst + (SIZE_T)(bottomUp ? h - 1 - y : y) * w * 4);
            for (ULONG x = 0; x < w; x++) d[x] = SRC(x, y);
        }
    }
    #undef SRC
    ExReleaseFastMutex(&c->Lock);
}
