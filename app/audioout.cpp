// Sound of the video files (see audioout.h).
#include "audioout.h"
#include "applog.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmreg.h>
#include <ks.h>
#include <ksmedia.h>
#include <wchar.h>

static const GUID kSubtypeFloat = { 0x00000003, 0x0000, 0x0010, { 0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71 } };

static void ReadString(IPropertyStore* props, const PROPERTYKEY& key, wchar_t* out, size_t len)
{
    out[0] = 0;
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(props->GetValue(key, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
    {
        wcsncpy(out, v.pwszVal, len - 1);
        out[len - 1] = 0;
    }
    PropVariantClear(&v);
}

int ListRenderDevices(RenderDevice* out, int max)
{
    IMMDeviceEnumerator* en = nullptr;
    IMMDeviceCollection* list = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return 0;
    int n = 0;
    UINT count = 0;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &list)) && SUCCEEDED(list->GetCount(&count)))
    {
        for (UINT i = 0; i < count && n < max; i++)
        {
            IMMDevice* dev = nullptr;
            IPropertyStore* props = nullptr;
            LPWSTR id = nullptr;
            if (SUCCEEDED(list->Item(i, &dev)) && SUCCEEDED(dev->GetId(&id)) && SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)))
            {
                RenderDevice& d = out[n++];
                wcsncpy(d.id, id, 255);
                d.id[255] = 0;
                ReadString(props, PKEY_Device_FriendlyName, d.name, 256);
                ReadString(props, PKEY_DeviceInterface_FriendlyName, d.adapter, 128);
            }
            if (id) CoTaskMemFree(id);
            if (props) props->Release();
            if (dev) dev->Release();
        }
    }
    if (list) list->Release();
    en->Release();
    return n;
}

bool FindSpeak2MicSpeaker(RenderDevice* out)
{
    RenderDevice list[64];
    int n = ListRenderDevices(list, 64);
    for (int i = 0; i < n; i++)
        if (_wcsicmp(list[i].adapter, L"Speak2Mic") == 0)
        {
            *out = list[i];
            return true;
        }
#ifdef S2C_UI_TEST
    if (n > 0) { *out = list[0]; return true; }     // sound tests under Wine (no Speak2Mic there); never in a release
#endif
    return false;
}

bool FindRenderDevice(const wchar_t* id, RenderDevice* out)
{
    RenderDevice list[64];
    int n = ListRenderDevices(list, 64);
    for (int i = 0; i < n; i++)
        if (_wcsicmp(list[i].id, id) == 0)
        {
            *out = list[i];
            return true;
        }
    return false;
}

bool AudioOut::Open(const wchar_t* deviceId, UINT32 rate, UINT32 channels, HRESULT* error)
{
    Close();
    IMMDeviceEnumerator* en = nullptr;
    IMMDevice* dev = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (SUCCEEDED(hr)) hr = en->GetDevice(deviceId, &dev);
    if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&m_client);
    if (SUCCEEDED(hr))
    {
        WAVEFORMATEXTENSIBLE wf = {};
        wf.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        wf.Format.nChannels = (WORD)channels;
        wf.Format.nSamplesPerSec = rate;
        wf.Format.wBitsPerSample = 32;
        wf.Format.nBlockAlign = (WORD)(channels * 4);
        wf.Format.nAvgBytesPerSec = rate * channels * 4;
        wf.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        wf.Samples.wValidBitsPerSample = 32;
        // The usual layouts (Windows mixes them down to the device); without one some systems refuse 5.1 / 7.1 tracks.
        static const DWORD kMasks[9] = { 0, 0x4 /* C */, 0x3 /* L R */, 0x7 /* L R C */, 0x33 /* quad */, 0x37 /* 5.0 */,
                                         0x3F /* 5.1 */, 0x13F /* 6.1 */, 0x63F /* 7.1 */ };
        wf.dwChannelMask = channels <= 8 ? kMasks[channels] : 0;
        wf.SubFormat = kSubtypeFloat;
        // 1 s buffer; Windows resamples / remaps the channels to the device format.
        hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                  10000000, 0, &wf.Format, nullptr);
    }
    if (SUCCEEDED(hr)) hr = m_client->GetBufferSize(&m_bufferFrames);
    if (SUCCEEDED(hr)) hr = m_client->GetService(IID_PPV_ARGS(&m_render));
    if (dev) dev->Release();
    if (en) en->Release();
    if (error) *error = hr;
    if (FAILED(hr))
    {
        Close();
        return false;
    }
    m_rate = rate;
    m_channels = channels;
    m_failed = false;
    return true;
}

void AudioOut::Close()
{
    if (m_client && m_started) m_client->Stop();
    if (m_render) m_render->Release();
    if (m_client) m_client->Release();
    m_render = nullptr;
    m_client = nullptr;
    m_started = false;
}

UINT32 AudioOut::QueuedFrames()
{
    UINT32 padding = 0;
    if (!m_client) return 0;
    HRESULT hr = m_client->GetCurrentPadding(&padding);
    if (FAILED(hr))
    {
        m_failed = true;
        return 0;
    }
    return padding;
}

UINT32 AudioOut::FreeFrames()
{
    if (!m_client) return 0;
    UINT32 queued = QueuedFrames();
    return m_failed ? 0 : m_bufferFrames - queued;
}

bool AudioOut::Write(const float* samples, UINT32 frames)
{
    if (!m_render || !frames) return false;
    BYTE* buf = nullptr;
    HRESULT hr = m_render->GetBuffer(frames, &buf);
    if (FAILED(hr))
    {
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) m_failed = true;
        return false;
    }
    memcpy(buf, samples, (SIZE_T)frames * m_channels * sizeof(float));
    m_render->ReleaseBuffer(frames, 0);
    return true;
}

void AudioOut::Start()
{
    if (!m_client || m_started) return;
    HRESULT hr = m_client->Start();
    if (SUCCEEDED(hr)) m_started = true;
    else
    {
        AppLog(L"sound output: start failed (0x%08lX)", (unsigned long)hr);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) m_failed = true;
    }
}

void AudioOut::Stop()
{
    if (m_client && m_started) m_client->Stop();
    m_started = false;
}
