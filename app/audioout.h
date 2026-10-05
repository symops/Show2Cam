// Sound of the video files: the playback devices and a simple WASAPI output (shared mode, float samples in the
// file's own rate and channels; Windows converts them to the device format).
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

struct RenderDevice
{
    wchar_t id[256];
    wchar_t name[256];          // as in Sound settings, e.g. "Speak2Mic Speaker (Speak2Mic)"
    wchar_t adapter[128];       // the device's adapter (driver) name, e.g. "Speak2Mic"
};

// Active playback devices. COM must be initialised on the calling thread.
int ListRenderDevices(RenderDevice* out, int max);
// Speak2Mic's playback side ("Speak2Mic Speaker"), found by its adapter, so a renamed one is found too.
bool FindSpeak2MicSpeaker(RenderDevice* out);
bool FindRenderDevice(const wchar_t* id, RenderDevice* out);

struct IAudioClient;
struct IAudioRenderClient;

class AudioOut
{
public:
    ~AudioOut() { Close(); }
    bool Open(const wchar_t* deviceId, UINT32 rate, UINT32 channels, HRESULT* error);
    void Close();
    bool IsOpen() const { return m_client != nullptr; }
    UINT32 Channels() const { return m_channels; }
    UINT32 Rate() const { return m_rate; }
    UINT32 FreeFrames();                                    // room in the buffer now
    UINT32 QueuedFrames();                                  // not played yet
    bool   Write(const float* samples, UINT32 frames);      // interleaved; at most FreeFrames()
    void   Start();
    void   Stop();
    bool   Failed() const { return m_failed; }              // the device went away

private:
    IAudioClient*       m_client = nullptr;
    IAudioRenderClient* m_render = nullptr;
    UINT32              m_rate = 0, m_channels = 0, m_bufferFrames = 0;
    bool                m_started = false, m_failed = false;
};
