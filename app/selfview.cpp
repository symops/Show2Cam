// Self-view (see selfview.h).
#include "selfview.h"
#include "applog.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wchar.h>

class SelfView
{
public:
    wchar_t          path[512];
    int              index;
    HANDLE           thread = nullptr, stopEvent = nullptr;
    volatile LONG    stop = 0;
    CRITICAL_SECTION cs;
    SelfViewStatus   status = {};
};

// The Media Foundation camera that is this device interface: the same device instance ("\\?\root#camera#0000#")
// and the same reference string ("\camera1"); the interface class GUID between them may differ.
static bool SameCamera(const wchar_t* a, const wchar_t* b)
{
    const wchar_t* ga = wcsstr(a, L"#{");
    const wchar_t* gb = wcsstr(b, L"#{");
    const wchar_t* ra = ga ? wcschr(ga, L'}') : nullptr;
    const wchar_t* rb = gb ? wcschr(gb, L'}') : nullptr;
    if (!ga || !gb || !ra || !rb) return _wcsicmp(a, b) == 0;
    return ga - a == gb - b && _wcsnicmp(a, b, ga - a) == 0 && _wcsicmp(ra + 1, rb + 1) == 0;
}

static HRESULT OpenCamera(SelfView* v, IMFMediaSource** source, IMFSourceReader** reader)
{
    *source = nullptr;
    *reader = nullptr;
    IMFAttributes* attr = nullptr;
    IMFActivate** devs = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFCreateAttributes(&attr, 1);
    if (SUCCEEDED(hr)) hr = attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    if (SUCCEEDED(hr)) hr = MFEnumDeviceSources(attr, &devs, &count);
    if (attr) attr->Release();
    if (FAILED(hr)) return hr;
    hr = MF_E_NOT_FOUND;
    for (UINT32 i = 0; i < count; i++)
    {
        WCHAR* link = nullptr;
        UINT32 len = 0;
        if (hr == (HRESULT)MF_E_NOT_FOUND &&
            SUCCEEDED(devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &link, &len)))
        {
            if (SameCamera(link, v->path)) hr = devs[i]->ActivateObject(IID_PPV_ARGS(source));
            CoTaskMemFree(link);
        }
        devs[i]->Release();
    }
    CoTaskMemFree(devs);
    if (FAILED(hr)) return hr;
    hr = MFCreateSourceReaderFromMediaSource(*source, nullptr, reader);
    IMFMediaType* type = nullptr;
    if (SUCCEEDED(hr) && SUCCEEDED((*reader)->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &type)))
    {
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h);
        EnterCriticalSection(&v->cs);
        v->status.width = w;
        v->status.height = h;
        LeaveCriticalSection(&v->cs);
        type->Release();
    }
    if (FAILED(hr))
    {
        (*source)->Shutdown();
        (*source)->Release();
        *source = nullptr;
    }
    return hr;
}

static DWORD WINAPI SelfViewThread(LPVOID p)
{
    SelfView* v = (SelfView*)p;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    HRESULT lastLogged = S_OK;
    while (!v->stop)
    {
        IMFMediaSource* source = nullptr;
        IMFSourceReader* reader = nullptr;
        HRESULT hr = OpenCamera(v, &source, &reader);
        if (SUCCEEDED(hr))
        {
            AppLog(L"camera %d: self-view opened the camera (%lux%lu)", v->index + 1, v->status.width, v->status.height);
            ULONG frames = 0;
            DWORD second = GetTickCount();
            while (!v->stop)
            {
                DWORD stream = 0, flags = 0;
                LONGLONG ts = 0;
                IMFSample* sample = nullptr;
                hr = reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &ts, &sample);
                if (sample)
                {
                    frames++;
                    sample->Release();
                }
                if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)))
                {
                    if (SUCCEEDED(hr)) hr = MF_E_END_OF_STREAM;
                    break;
                }
                DWORD now = GetTickCount();
                if (now - second >= 1000)
                {
                    EnterCriticalSection(&v->cs);
                    v->status.open = true;
                    v->status.error = S_OK;
                    v->status.fps = (ULONG)(frames * 1000 / (now - second));
                    LeaveCriticalSection(&v->cs);
                    frames = 0;
                    second = now;
                }
            }
            reader->Release();
            source->Shutdown();
            source->Release();
            if (!v->stop) AppLog(L"camera %d: self-view lost the camera (0x%08lX)", v->index + 1, (unsigned long)hr);
            lastLogged = hr;
        }
        else if (hr != lastLogged)
        {
            AppLog(L"camera %d: self-view cannot open the camera (0x%08lX)", v->index + 1, (unsigned long)hr);
            lastLogged = hr;
        }
        EnterCriticalSection(&v->cs);
        v->status.open = false;
        v->status.fps = 0;
        v->status.error = v->stop ? S_OK : hr;
        LeaveCriticalSection(&v->cs);
        if (!v->stop) WaitForSingleObject(v->stopEvent, 3000);
    }
    MFShutdown();
    CoUninitialize();
    return 0;
}

SelfView* SelfViewStart(const wchar_t* cameraPath, int index)
{
    SelfView* v = new SelfView;
    wcsncpy(v->path, cameraPath, 511);
    v->path[511] = 0;
    v->index = index;
    InitializeCriticalSection(&v->cs);
    v->stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    v->thread = CreateThread(nullptr, 0, SelfViewThread, v, 0, nullptr);
    AppLog(L"camera %d: self-view on", index + 1);
    return v;
}

void SelfViewStop(SelfView* v)
{
    if (!v) return;
    InterlockedExchange(&v->stop, 1);
    SetEvent(v->stopEvent);
    if (v->thread)
    {
        WaitForSingleObject(v->thread, 10000);      // a frame read ends within one frame time
        CloseHandle(v->thread);
    }
    CloseHandle(v->stopEvent);
    DeleteCriticalSection(&v->cs);
    AppLog(L"camera %d: self-view off", v->index + 1);
    delete v;
}

void SelfViewGetStatus(SelfView* v, SelfViewStatus* s)
{
    EnterCriticalSection(&v->cs);
    *s = v->status;
    LeaveCriticalSection(&v->cs);
}
