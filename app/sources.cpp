// What the cameras show (see sources.h).
#include "sources.h"
#include "audioout.h"
#include "applog.h"
#include "lang.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

// ---------------------------------------------------------------------------
// Common

struct EventSink
{
    HWND wnd;
    UINT msg;
    int  index;
    void Post(int event) const { if (wnd) PostMessageW(wnd, msg, (WPARAM)index, (LPARAM)event); }
};

void NormalizeFormat(ULONG* w, ULONG* h, ULONG* fps)
{
    double W = *w ? *w : 1280, H = *h ? *h : 720;
    // Keep the aspect ratio while bringing it into the driver's limits.
    if (W > S2C_MAX_WIDTH) { H = H * S2C_MAX_WIDTH / W; W = S2C_MAX_WIDTH; }
    if (H > S2C_MAX_HEIGHT) { W = W * S2C_MAX_HEIGHT / H; H = S2C_MAX_HEIGHT; }
    if (W < S2C_MIN_WIDTH) { H = H * S2C_MIN_WIDTH / W; W = S2C_MIN_WIDTH; }
    if (H < S2C_MIN_HEIGHT) { W = W * S2C_MIN_HEIGHT / H; H = S2C_MIN_HEIGHT; }
    ULONG a = (ULONG)(W + 0.5), b = (ULONG)(H + 0.5);
    a &= ~1u;
    b &= ~1u;
    *w = a < S2C_MIN_WIDTH ? S2C_MIN_WIDTH : (a > S2C_MAX_WIDTH ? S2C_MAX_WIDTH : a);
    *h = b < S2C_MIN_HEIGHT ? S2C_MIN_HEIGHT : (b > S2C_MAX_HEIGHT ? S2C_MAX_HEIGHT : b);
    if (*fps < 1) *fps = 30;
    if (*fps > S2C_MAX_FPS) *fps = S2C_MAX_FPS;
}

static const wchar_t* FileName(const wchar_t* path)
{
    const wchar_t* s = wcsrchr(path, L'\\');
    return s ? s + 1 : path;
}

class Source
{
public:
    explicit Source(const EventSink& sink) : m_sink(sink) {}
    virtual ~Source() {}
    // Renders into px (w x h) when a new picture is due or `force`; *changed says so. Returns the ms until the next call.
    virtual DWORD Tick(ULONG* px, int w, int h, bool force, bool paused, bool* changed) = 0;
    // The source's own size and rate, if it has one.
    virtual bool Native(ULONG* w, ULONG* h, ULONG* fps) = 0;
    virtual void Describe(CamRunStatus* s) = 0;

protected:
    EventSink m_sink;
};

// ---------------------------------------------------------------------------
// Text

class TextSource : public Source
{
public:
    TextSource(const EventSink& sink, const wchar_t* text) : Source(sink)
    {
        wcsncpy(m_text, text, 255);
        m_text[255] = 0;
    }
    DWORD Tick(ULONG* px, int w, int h, bool force, bool, bool* changed) override
    {
        if (force)
        {
            RenderTextScreen(px, w, h, m_text);
            *changed = true;
        }
        return 1000;
    }
    bool Native(ULONG* w, ULONG* h, ULONG* fps) override
    {
        *w = 1280;
        *h = 720;
        *fps = 30;
        return true;
    }
    void Describe(CamRunStatus* s) override
    {
        s->state = StateOk;
        s->current[0] = 0;
    }

private:
    wchar_t m_text[256];
};

// ---------------------------------------------------------------------------
// Images: a random picture of the folder every 5 seconds, never the same twice in a row while there is another

class ImageSource : public Source
{
public:
    ImageSource(const EventSink& sink, const wchar_t* folder) : Source(sink)
    {
        wcscpy(m_folder, folder);
        FindNative();
    }
    DWORD Tick(ULONG* px, int w, int h, bool force, bool paused, bool* changed) override
    {
        DWORD now = GetTickCount();
        bool due = !paused && (int)(now - m_next) >= 0;
        if (!due && !force) return Remaining(now);
        if (due || !m_current[0])
        {
            wchar_t next[MAX_PATH];
            if (PickRandomFile(m_folder, MediaImages, m_current, next))
            {
                UINT sw, sh;
                if (RenderImageFile(next, px, w, h, &sw, &sh))
                {
                    wcscpy(m_current, next);
                    m_state = StateOk;
                    m_detail[0] = 0;
                    *changed = true;
                    if (!m_nativeW) FindNative();
                    m_next = now + 5000;
                    return Remaining(now);
                }
                AppLog(L"camera %d: cannot decode %ls", m_sink.index + 1, next);
                _snwprintf(m_detail, 256, TR(L"Не удалось открыть «%ls» (формат не поддерживается Windows?)."), FileName(next));
                m_detail[255] = 0;
                if (!m_current[0])
                {
                    RenderNoticeScreen(px, w, h, NoticeError, TR(L"Не удалось открыть изображение"), FileName(next));
                    m_state = StateError;
                    *changed = true;
                }
                m_next = now + 1000;
                return Remaining(now);
            }
            // No pictures (any more).
            m_current[0] = 0;
            if (m_state != StateNoFiles || force)
            {
                RenderNoticeScreen(px, w, h, NoticeEmptyFolder, TR(L"В папке нет изображений"), m_folder);
                *changed = true;
                if (m_state != StateNoFiles) m_sink.Post(EvNoFiles);
                m_state = StateNoFiles;
            }
            m_next = now + 2000;
            m_nativeW = m_nativeH = 0;
            return Remaining(now);
        }
        // force (new camera size): the same picture again
        UINT sw, sh;
        if (RenderImageFile(m_current, px, w, h, &sw, &sh)) *changed = true;
        return Remaining(now);
    }
    bool Native(ULONG* w, ULONG* h, ULONG* fps) override
    {
        if (!m_nativeW) return false;
        *w = m_nativeW;
        *h = m_nativeH;
        *fps = 30;
        return true;
    }
    void Describe(CamRunStatus* s) override
    {
        s->state = m_state;
        wcscpy(s->current, m_current);
        wcscpy(s->detail, m_detail);
    }

private:
    // "As the source": the size of the folder's first picture (by name), so it does not change every 5 seconds.
    void FindNative()
    {
        static thread_local wchar_t names[64][MAX_PATH];
        int n = ListMediaFiles(m_folder, MediaImages, names, 64);
        for (int i = 0; i < n; i++)
        {
            wchar_t path[MAX_PATH];
            _snwprintf(path, MAX_PATH, L"%ls\\%ls", m_folder, names[i]);
            path[MAX_PATH - 1] = 0;
            UINT w, h;
            if (ImageFileSize(path, &w, &h))
            {
                m_nativeW = w;
                m_nativeH = h;
                return;
            }
        }
    }
    DWORD Remaining(DWORD now) const
    {
        int left = (int)(m_next - now);
        return left < 10 ? 10 : (left > 1000 ? 1000 : (DWORD)left);
    }

    wchar_t m_folder[MAX_PATH];
    wchar_t m_current[MAX_PATH] = L"";
    wchar_t m_detail[256] = L"";
    int     m_state = StateStarting;
    DWORD   m_next = GetTickCount();
    UINT    m_nativeW = 0, m_nativeH = 0;
};

// ---------------------------------------------------------------------------
// Video: random files of the folder (Media Foundation), the sound in step on the chosen playback device

static const LONGLONG kAudioLead = 3000000;        // write sound up to 300 ms ahead of the clock (100-ns units)

class VideoSource : public Source
{
public:
    VideoSource(const EventSink& sink, const CamConfig& c) : Source(sink)
    {
        wcscpy(m_folder, c.videoFolder);
        m_audioMode = c.audioMode;
        wcscpy(m_audioDevice, c.audioDevice);
        QueryPerformanceFrequency(&m_freq);
    }
    ~VideoSource() override { CloseFile(); }

    DWORD Tick(ULONG* px, int w, int h, bool force, bool paused, bool* changed) override
    {
        DWORD now = GetTickCount();
        if (!m_reader)
        {
            if ((int)(now - m_retryAt) < 0)
            {
                if (force) { DrawNotice(px, w, h); *changed = true; }
                return 200;
            }
            if (!OpenNext())
            {
                DrawNotice(px, w, h);
                *changed = true;
                return 200;
            }
        }
        if (paused != m_clockPaused) SetPaused(paused);
        if (force && m_shown)
        {
            Present(m_shown, px, w, h);
            *changed = true;
        }
        if (paused) return 100;

        LONGLONG t = Clock();
        bool due = false;
        for (int guard = 0; !m_videoEnded && guard < 120; guard++)
        {
            if (!m_nextVideo && !ReadVideo()) break;
            if (!m_nextVideo || m_nextVideoTs > t) break;
            // Due: the newest due frame is the one to show (late ones are skipped).
            if (m_shown) m_shown->Release();
            m_shown = m_nextVideo;
            m_nextVideo = nullptr;
            due = true;
        }
        if (due)
        {
            Present(m_shown, px, w, h);
            *changed = true;
        }
        FeedAudio(t);
        if (m_audio.Failed())
        {
            AppLog(L"camera %d: the sound device went away", m_sink.index + 1);
            m_audio.Close();
            m_audioName[0] = 0;
        }

        bool audioDone = !m_audio.IsOpen() || (m_audioEnded && !m_pendingFrames && m_audio.QueuedFrames() == 0);
        if (m_videoEnded && audioDone)
        {
            AppLog(L"camera %d: video %ls ended", m_sink.index + 1, m_current);
            CloseFile();
            m_retryAt = now;
            return 1;
        }
        LONGLONG wait = m_nextVideo ? (m_nextVideoTs - t) / 10000 : 10;
        return wait < 1 ? 1 : (wait > 20 ? 20 : (DWORD)wait);
    }

    bool Native(ULONG* w, ULONG* h, ULONG* fps) override
    {
        if (!m_cropW) return false;
        *w = m_cropW;
        *h = m_cropH;
        *fps = m_fps ? m_fps : 30;
        return true;
    }

    void Describe(CamRunStatus* s) override
    {
        s->state = m_state;
        wcscpy(s->current, m_current);
        wcscpy(s->detail, m_detail);
        wcscpy(s->audio, m_audioName);
    }

private:
    void DrawNotice(ULONG* px, int w, int h)
    {
        if (m_state == StateNoFiles)
            RenderNoticeScreen(px, w, h, NoticeEmptyFolder, TR(L"В папке нет видео"), m_folder);
        else
            RenderNoticeScreen(px, w, h, NoticeError, TR(L"Не удалось воспроизвести видео"), m_detail);
    }

    LONGLONG Clock()
    {
        if (m_clockPaused) return m_pausedAt;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        LONGLONG ticks = now.QuadPart - m_start.QuadPart;
        return m_base + ticks / m_freq.QuadPart * 10000000 + (ticks % m_freq.QuadPart) * 10000000 / m_freq.QuadPart;
    }

    void SetPaused(bool paused)
    {
        if (paused)
        {
            m_pausedAt = Clock();
            m_clockPaused = true;
            m_audio.Stop();
        }
        else
        {
            QueryPerformanceCounter(&m_start);
            m_base = m_pausedAt;
            m_clockPaused = false;
            if (m_audioStarted) m_audio.Start();
        }
    }

    bool OpenNext()
    {
        for (int attempt = 0; attempt < 5; attempt++)
        {
            wchar_t path[MAX_PATH];
            if (!PickRandomFile(m_folder, MediaVideo, m_last, path))
            {
                if (m_state != StateNoFiles) m_sink.Post(EvNoFiles);
                m_state = StateNoFiles;
                m_current[0] = 0;
                m_cropW = m_cropH = 0;
                m_retryAt = GetTickCount() + 2000;
                return false;
            }
            wcscpy(m_last, path);
            HRESULT hr = OpenFile(path);
            if (SUCCEEDED(hr))
            {
                m_state = StateOk;
                m_detail[0] = 0;
                m_sink.Post(EvVideoFile);
                return true;
            }
            AppLog(L"camera %d: cannot play %ls (0x%08lX)", m_sink.index + 1, path, (unsigned long)hr);
            _snwprintf(m_detail, 256, L"%ls (0x%08lX)", FileName(path), (unsigned long)hr);
            m_detail[255] = 0;
            CloseFile();
        }
        m_state = StateError;
        m_sink.Post(EvVideoError);
        m_retryAt = GetTickCount() + 5000;
        return false;
    }

    HRESULT OpenFile(const wchar_t* path)
    {
        IMFAttributes* attr = nullptr;
        HRESULT hr = MFCreateAttributes(&attr, 2);
        if (SUCCEEDED(hr)) hr = attr->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
        if (SUCCEEDED(hr)) hr = MFCreateSourceReaderFromURL(path, attr, &m_reader);
        if (attr) attr->Release();
        if (FAILED(hr)) return hr;

        // Video: RGB32 (converted by the reader).
        IMFMediaType* type = nullptr;
        hr = MFCreateMediaType(&type);
        if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(hr)) hr = type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (SUCCEEDED(hr)) hr = m_reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, type);
        if (type) type->Release();
        if (FAILED(hr)) return hr;
        IMFMediaType* cur = nullptr;
        hr = m_reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
        if (FAILED(hr)) return hr;
        UINT32 w = 0, h = 0, num = 0, den = 0;
        MFGetAttributeSize(cur, MF_MT_FRAME_SIZE, &w, &h);
        m_frameW = w;
        m_frameH = h;
        m_stride = (LONG)MFGetAttributeUINT32(cur, MF_MT_DEFAULT_STRIDE, w * 4);
        // RGB32 rows are at least width * 4 bytes apart (some decoders report the stride in pixels).
        if ((m_stride < 0 ? -m_stride : m_stride) < (LONG)(w * 4)) m_stride = m_stride < 0 ? -(LONG)(w * 4) : (LONG)(w * 4);
        m_cropX = m_cropY = 0;
        m_cropW = w;
        m_cropH = h;
        MFVideoArea area;
        UINT32 got = 0;
        if (SUCCEEDED(cur->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, (UINT8*)&area, sizeof(area), &got)) && got == sizeof(area) &&
            area.Area.cx > 0 && area.Area.cy > 0 && area.OffsetX.value >= 0 && area.OffsetY.value >= 0 &&
            (UINT32)(area.OffsetX.value + area.Area.cx) <= w && (UINT32)(area.OffsetY.value + area.Area.cy) <= h)
        {
            m_cropX = area.OffsetX.value;
            m_cropY = area.OffsetY.value;
            m_cropW = (UINT32)area.Area.cx;
            m_cropH = (UINT32)area.Area.cy;
        }
        m_fps = 30;
        if (SUCCEEDED(MFGetAttributeRatio(cur, MF_MT_FRAME_RATE, &num, &den)) && num && den)
            m_fps = (num + den / 2) / den;
        GUID sub = {};
        cur->GetGUID(MF_MT_SUBTYPE, &sub);
        AppLog(L"camera %d: video output type %08lX, %ux%u, default stride %ld", m_sink.index + 1, (unsigned long)sub.Data1, w, h,
               (long)m_stride);
        m_logBuffer = true;
        cur->Release();
        if (!m_frameW || !m_frameH) return MF_E_INVALIDMEDIATYPE;

        // Sound: float samples, played on the chosen device; without one the audio stream is not decoded at all.
        m_audioName[0] = 0;
        bool wantAudio = false;
        RenderDevice dev = {};
        if (m_audioMode == AudioSpeak2Mic) wantAudio = FindSpeak2MicSpeaker(&dev);
        else if (m_audioMode == AudioDevice) wantAudio = m_audioDevice[0] && FindRenderDevice(m_audioDevice, &dev);
        if (m_audioMode != AudioOff && !wantAudio) m_sink.Post(EvAudioMissing);
        bool audioOk = false;
        if (wantAudio)
        {
            IMFMediaType* at = nullptr;
            HRESULT ahr = MFCreateMediaType(&at);
            if (SUCCEEDED(ahr)) ahr = at->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            if (SUCCEEDED(ahr)) ahr = at->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Float);
            if (SUCCEEDED(ahr)) ahr = m_reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, at);
            if (at) at->Release();
            IMFMediaType* ac = nullptr;
            if (SUCCEEDED(ahr)) ahr = m_reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, &ac);
            if (SUCCEEDED(ahr))
            {
                UINT32 ch = MFGetAttributeUINT32(ac, MF_MT_AUDIO_NUM_CHANNELS, 0);
                UINT32 rate = MFGetAttributeUINT32(ac, MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
                ac->Release();
                HRESULT ohr = E_FAIL;
                if (ch && rate && m_audio.Open(dev.id, rate, ch, &ohr))
                {
                    audioOk = true;
                    wcscpy(m_audioName, dev.name);
                }
                else
                    AppLog(L"camera %d: cannot open the sound device \"%ls\" (0x%08lX)", m_sink.index + 1, dev.name, (unsigned long)ohr);
            }
            else if (ahr != (HRESULT)MF_E_INVALIDSTREAMNUMBER)
                AppLog(L"camera %d: the video's sound cannot be decoded (0x%08lX)", m_sink.index + 1, (unsigned long)ahr);
        }
        if (!audioOk) m_reader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, FALSE);
        m_audioEnded = !audioOk;
        m_audioStarted = false;
        m_videoEnded = false;
        wcscpy(m_current, path);

        // The clock starts at the first frame's time.
        if (!ReadVideo() || !m_nextVideo) return MF_E_INVALID_STREAM_DATA;
        m_base = m_nextVideoTs;
        QueryPerformanceCounter(&m_start);
        m_clockPaused = false;
        AppLog(L"camera %d: video %ls, %ux%u (shown %ux%u) %lu fps, sound %ls", m_sink.index + 1, path, m_frameW, m_frameH, m_cropW,
               m_cropH, m_fps, audioOk ? m_audioName : L"off");
        return S_OK;
    }

    void CloseFile()
    {
        if (m_nextVideo) m_nextVideo->Release();
        if (m_shown) m_shown->Release();
        m_nextVideo = m_shown = nullptr;
        if (m_pending) free(m_pending);
        m_pending = nullptr;
        m_pendingFrames = m_pendingPos = 0;
        m_audio.Close();
        if (m_reader) m_reader->Release();
        m_reader = nullptr;
    }

    // The next video frame into m_nextVideo (false: no more).
    bool ReadVideo()
    {
        for (int guard = 0; guard < 50; guard++)
        {
            DWORD stream = 0, flags = 0;
            LONGLONG ts = 0;
            IMFSample* sample = nullptr;
            HRESULT hr = m_reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &stream, &flags, &ts, &sample);
            if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)))
            {
                if (sample) sample->Release();
                if (FAILED(hr)) AppLog(L"camera %d: video read error 0x%08lX", m_sink.index + 1, (unsigned long)hr);
                m_videoEnded = true;
                return false;
            }
            if (sample)
            {
                m_nextVideo = sample;
                m_nextVideoTs = ts;
                return true;
            }
        }
        return false;
    }

    void Present(IMFSample* sample, ULONG* px, int w, int h)
    {
        IMFMediaBuffer* buf = nullptr;
        if (FAILED(sample->ConvertToContiguousBuffer(&buf))) return;
        IMF2DBuffer* b2 = nullptr;
        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        if (SUCCEEDED(buf->QueryInterface(IID_PPV_ARGS(&b2))) && SUCCEEDED(b2->Lock2D(&scan0, &pitch)))
        {
            if (m_logBuffer)
            {
                DWORD len = 0;
                buf->GetCurrentLength(&len);
                AppLog(L"camera %d: video frame: 2D buffer, pitch %ld, length %lu", m_sink.index + 1, (long)pitch, len);
                m_logBuffer = false;
            }
            FitPixels(scan0 + (LONG_PTR)m_cropY * pitch + (SIZE_T)m_cropX * 4, (int)m_cropW, (int)m_cropH, pitch, px, w, h);
            b2->Unlock2D();
        }
        else
        {
            BYTE* p = nullptr;
            DWORD max = 0, len = 0;
            if (SUCCEEDED(buf->Lock(&p, &max, &len)))
            {
                if (m_logBuffer)
                {
                    AppLog(L"camera %d: video frame: buffer, stride %ld, length %lu", m_sink.index + 1, (long)m_stride, len);
                    m_logBuffer = false;
                }
                LONG stride = m_stride;
                BYTE* top = stride < 0 ? p + (SIZE_T)(m_frameH - 1) * (SIZE_T)(-stride) : p;
                if (len >= (DWORD)(m_frameH * (stride < 0 ? -stride : stride)))
                    FitPixels(top + (LONG_PTR)m_cropY * stride + (SIZE_T)m_cropX * 4, (int)m_cropW, (int)m_cropH, stride, px, w, h);
                buf->Unlock();
            }
        }
        if (b2) b2->Release();
        buf->Release();
    }

    // Keeps the sound device fed up to kAudioLead ahead of the clock.
    void FeedAudio(LONGLONG t)
    {
        if (!m_audio.IsOpen()) return;
        UINT32 ch = m_audio.Channels();
        for (int guard = 0; guard < 64; guard++)
        {
            if (m_pendingPos >= m_pendingFrames)
            {
                if (m_audioEnded) return;
                DWORD stream = 0, flags = 0;
                LONGLONG ts = 0;
                IMFSample* sample = nullptr;
                HRESULT hr = m_reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &stream, &flags, &ts, &sample);
                if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)))
                {
                    if (sample) sample->Release();
                    m_audioEnded = true;
                    return;
                }
                if (!sample) continue;
                IMFMediaBuffer* buf = nullptr;
                BYTE* p = nullptr;
                DWORD len = 0;
                if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && SUCCEEDED(buf->Lock(&p, nullptr, &len)))
                {
                    UINT32 frames = len / (ch * sizeof(float));
                    if (frames > m_pendingCap)
                    {
                        free(m_pending);
                        m_pending = (float*)malloc((SIZE_T)frames * ch * sizeof(float));
                        m_pendingCap = m_pending ? frames : 0;
                    }
                    if (m_pending)
                    {
                        memcpy(m_pending, p, (SIZE_T)frames * ch * sizeof(float));
                        m_pendingFrames = frames;
                        m_pendingPos = 0;
                        m_pendingTs = ts;
                    }
                    buf->Unlock();
                }
                if (buf) buf->Release();
                sample->Release();
                if (m_pendingPos >= m_pendingFrames) continue;
            }
            // The chunk's time at its current position: written only when it is close to being due.
            LONGLONG at = m_pendingTs + (LONGLONG)m_pendingPos * 10000000 / m_audio.Rate();
            if (at > t + kAudioLead) return;
            if (at + (LONGLONG)(m_pendingFrames - m_pendingPos) * 10000000 / m_audio.Rate() < t - 2000000)
            {
                m_pendingPos = m_pendingFrames;     // more than 200 ms late (e.g. after a stall): dropped
                continue;
            }
            UINT32 room = m_audio.FreeFrames();
            if (!room) return;
            UINT32 n = m_pendingFrames - m_pendingPos;
            if (n > room) n = room;
            if (!m_audio.Write(m_pending + (SIZE_T)m_pendingPos * ch, n)) return;
            m_pendingPos += n;
            if (!m_audioStarted)
            {
                m_audioStarted = true;
                if (!m_clockPaused) m_audio.Start();
            }
        }
    }

    wchar_t m_folder[MAX_PATH];
    int     m_audioMode;
    wchar_t m_audioDevice[256];
    wchar_t m_audioName[256] = L"";
    wchar_t m_current[MAX_PATH] = L"", m_last[MAX_PATH] = L"", m_detail[256] = L"";
    int     m_state = StateStarting;
    DWORD   m_retryAt = GetTickCount();

    IMFSourceReader* m_reader = nullptr;
    UINT32  m_frameW = 0, m_frameH = 0, m_cropX = 0, m_cropY = 0, m_cropW = 0, m_cropH = 0;
    LONG    m_stride = 0;
    ULONG   m_fps = 0;
    bool    m_logBuffer = false;
    IMFSample* m_nextVideo = nullptr;
    IMFSample* m_shown = nullptr;
    LONGLONG m_nextVideoTs = 0;
    bool    m_videoEnded = true, m_audioEnded = true, m_audioStarted = false;

    AudioOut m_audio;
    float*  m_pending = nullptr;
    UINT32  m_pendingCap = 0, m_pendingFrames = 0, m_pendingPos = 0;
    LONGLONG m_pendingTs = 0;

    LARGE_INTEGER m_freq = {}, m_start = {};
    LONGLONG m_base = 0, m_pausedAt = 0;
    bool    m_clockPaused = false;
};

// ---------------------------------------------------------------------------
// MJPEG stream over HTTP(S): a reader thread receives and decodes the pictures; without pictures for 10 s the
// camera shows "No signal" (the last picture stays until then); the reader reconnects 3 s after a failure.
static const DWORD kNoSignalMs = 10000;

class StreamSource : public Source
{
public:
    StreamSource(const EventSink& sink, const wchar_t* url) : Source(sink)
    {
        wcsncpy(m_url, url, 511);
        m_url[511] = 0;
        InitializeCriticalSection(&m_cs);
        m_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        m_thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    }
    ~StreamSource() override
    {
        InterlockedExchange(&m_stop, 1);
        SetEvent(m_stopEvent);
        EnterCriticalSection(&m_cs);
        if (m_request) WinHttpCloseHandle(m_request);      // aborts a waiting read
        m_request = nullptr;
        LeaveCriticalSection(&m_cs);
        if (m_thread)
        {
            WaitForSingleObject(m_thread, 10000);
            CloseHandle(m_thread);
        }
        CloseHandle(m_stopEvent);
        DeleteCriticalSection(&m_cs);
    }

    DWORD Tick(ULONG* px, int w, int h, bool force, bool paused, bool* changed) override
    {
        DWORD now = GetTickCount();
        EnterCriticalSection(&m_cs);
        bool fresh = m_seq && (now - m_frameTick) < kNoSignalMs;
        if (fresh && (force || (!paused && m_seq != m_shownSeq) || m_noticeShown))
        {
            FitPixels((const BYTE*)m_latest.px, m_latest.w, m_latest.h, m_latest.w * 4, px, w, h);
            m_shownSeq = m_seq;
            *changed = true;
            if (m_noticeShown) m_sink.Post(EvSignal);
            m_noticeShown = false;
        }
        wchar_t error[160];
        wcscpy(error, m_error);
        LeaveCriticalSection(&m_cs);
        if (!m_seq && !m_noticeShown && (!m_connectingShown || force))
        {
            // Before the first picture (and before "No signal" after 10 s): "Connecting…" instead of a black screen.
            RenderNoticeScreen(px, w, h, NoticeConnecting, TR(L"Подключение…"), m_url);
            *changed = true;
            m_connectingShown = true;
        }
        if (!fresh && (!m_noticeShown || force) && (int)(now - m_started) >= (int)kNoSignalMs)
        {
            wchar_t detail[700];
            if (error[0]) _snwprintf(detail, 700, L"%ls\n%ls · %ls", m_url, error, TR(L"переподключение…"));
            else _snwprintf(detail, 700, L"%ls\n%ls", m_url, TR(L"переподключение…"));
            detail[699] = 0;
            RenderNoticeScreen(px, w, h, NoticeNoSignal, TR(L"Нет сигнала"), detail);
            *changed = true;
            if (!m_noticeShown) m_sink.Post(EvNoSignal);
            m_noticeShown = true;
        }
        return 15;
    }

    bool Native(ULONG* w, ULONG* h, ULONG* fps) override
    {
        EnterCriticalSection(&m_cs);
        bool ok = m_latest.w > 0;
        *w = (ULONG)m_latest.w;
        *h = (ULONG)m_latest.h;
        *fps = 30;
        LeaveCriticalSection(&m_cs);
        return ok;
    }

    void Describe(CamRunStatus* s) override
    {
        EnterCriticalSection(&m_cs);
        s->state = m_noticeShown ? StateNoSignal : (m_seq ? StateOk : StateStarting);
        wcsncpy(s->current, m_url, MAX_PATH - 1);
        s->current[MAX_PATH - 1] = 0;
        wcsncpy(s->detail, m_error, 255);
        s->detail[255] = 0;
        LeaveCriticalSection(&m_cs);
    }

private:
    static DWORD WINAPI ThreadProc(LPVOID p)
    {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ((StreamSource*)p)->Run();
        CoUninitialize();
        return 0;
    }

    void SetError(const wchar_t* fmt, unsigned long code)
    {
        EnterCriticalSection(&m_cs);
        _snwprintf(m_error, 160, fmt, code);
        m_error[159] = 0;
        LeaveCriticalSection(&m_cs);
    }

    void Run()
    {
        HINTERNET session = WinHttpOpen(L"Show2Cam", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
        int failures = 0;
        while (!m_stop && session)
        {
            int rc = Connect(session);
            if (m_stop) break;
            if (rc == 1) { failures = 0; continue; }                 // snapshot URL: ask again right away
            if (++failures == 1 || failures % 20 == 0)
                AppLog(L"camera %d: stream %ls: %ls, reconnecting", m_sink.index + 1, m_url, m_error);
            WaitForSingleObject(m_stopEvent, 3000);
        }
        if (session) WinHttpCloseHandle(session);
    }

    void Publish(const BYTE* data, size_t size)
    {
        Picture pic;
        if (!DecodeImageMemory(data, size, &pic))
        {
            SetError(TR(L"не удалось декодировать кадр"), 0);
            return;
        }
        EnterCriticalSection(&m_cs);
        // swap the decoded picture in
        ULONG* px = m_latest.px;
        int w = m_latest.w, h = m_latest.h;
        m_latest.px = pic.px;
        m_latest.w = pic.w;
        m_latest.h = pic.h;
        pic.px = px;
        pic.w = w;
        pic.h = h;
        m_seq++;
        m_frameTick = GetTickCount();
        m_error[0] = 0;
        LeaveCriticalSection(&m_cs);
    }

    // One connection. 0: failed / ended (reconnect after a pause), 1: a single picture was received.
    int Connect(HINTERNET session)
    {
        URL_COMPONENTS uc = { sizeof(uc) };
        wchar_t host[256], path[2048], user[128], pass[128];
        uc.lpszHostName = host; uc.dwHostNameLength = 256;
        uc.lpszUrlPath = path; uc.dwUrlPathLength = 2048;
        uc.lpszUserName = user; uc.dwUserNameLength = 128;
        uc.lpszPassword = pass; uc.dwPasswordLength = 128;
        wchar_t extra[1024];
        uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 1024;
        wchar_t url[600];
        // "10.0.28.101:8001" alone is taken as http://
        if (!wcsstr(m_url, L"://")) _snwprintf(url, 600, L"http://%ls", m_url);
        else wcsncpy(url, m_url, 599);
        url[599] = 0;
        if (!WinHttpCrackUrl(url, 0, 0, &uc))
        {
            SetError(TR(L"неверный адрес"), 0);
            return 0;
        }
        wchar_t object[3072];
        _snwprintf(object, 3072, L"%ls%ls", path[0] ? path : L"/", extra);
        object[3071] = 0;
        HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
        if (!connect)
        {
            SetError(TR(L"ошибка подключения %lu"), GetLastError());
            return 0;
        }
        HINTERNET req = WinHttpOpenRequest(connect, L"GET", object, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                           uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
        int rc = 0;
        if (req)
        {
            EnterCriticalSection(&m_cs);
            bool stopping = m_stop != 0;
            if (!stopping) m_request = req;
            LeaveCriticalSection(&m_cs);
            if (!stopping) rc = Receive(req, uc.dwUserNameLength ? user : nullptr, pass);
            EnterCriticalSection(&m_cs);
            if (m_request) WinHttpCloseHandle(m_request);       // (closed by the destructor otherwise)
            m_request = nullptr;
            LeaveCriticalSection(&m_cs);
        }
        else
            SetError(TR(L"ошибка подключения %lu"), GetLastError());
        WinHttpCloseHandle(connect);
        return rc;
    }

    int Receive(HINTERNET req, const wchar_t* user, const wchar_t* pass)
    {
        WinHttpSetTimeouts(req, 5000, 5000, 5000, 5000);
        if (user)
        {
            // Basic authentication from "http://user:password@host/...".
            char plain[300];
            _snprintf(plain, 300, "%ls:%ls", user, pass);
            plain[299] = 0;
            char b64[420];
            DWORD n = 420;
            if (CryptBinaryToStringA((const BYTE*)plain, (DWORD)strlen(plain), CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, b64, &n))
            {
                wchar_t header[480];
                _snwprintf(header, 480, L"Authorization: Basic %hs", b64);
                header[479] = 0;
                WinHttpAddRequestHeaders(req, header, (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);
            }
        }
        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
            !WinHttpReceiveResponse(req, nullptr))
        {
            SetError(TR(L"сервер не отвечает (%lu)"), GetLastError());
            return 0;
        }
        DWORD code = 0, size = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &code, &size,
                            WINHTTP_NO_HEADER_INDEX);
        if (code != 200)
        {
            SetError(L"HTTP %lu", code);
            return 0;
        }
        wchar_t ctype[256] = L"";
        size = sizeof(ctype);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, ctype, &size, WINHTTP_NO_HEADER_INDEX);
        _wcslwr(ctype);
        bool single = wcsstr(ctype, L"image/") != nullptr;
        char boundary[128] = "";
        if (const wchar_t* b = wcsstr(ctype, L"boundary="))
        {
            b += 9;
            if (*b == L'"') b++;
            int i = 0;
            while (b[i] && b[i] != L'"' && b[i] != L';' && i < 120) { boundary[i] = (char)b[i]; i++; }
            boundary[i] = 0;
        }
        if (!m_logged)
        {
            AppLog(L"camera %d: stream %ls connected (%ls)", m_sink.index + 1, m_url, ctype);
            m_logged = true;
        }
        return ReadStream(req, single, boundary);
    }

    static size_t Find(const BYTE* data, size_t len, const char* what, size_t wlen, size_t from)
    {
        for (size_t i = from; i + wlen <= len; i++)
            if (data[i] == (BYTE)what[0] && memcmp(data + i, what, wlen) == 0) return i;
        return (size_t)-1;
    }

    static size_t ContentLength(const BYTE* data, size_t len)
    {
        for (size_t i = 0; i + 15 < len; i++)
            if (_strnicmp((const char*)data + i, "content-length:", 15) == 0)
                return (size_t)strtoul((const char*)data + i + 15, nullptr, 10);
        return 0;
    }

    // Reads JPEG pictures out of the response: whole body (single picture), parts with Content-Length, or the data
    // between JPEG start (FF D8) and the next boundary / JPEG end (FF D9).
    int ReadStream(HINTERNET req, bool single, const char* boundary)
    {
        const size_t kMax = 32 * 1024 * 1024;
        size_t cap = 1 << 20, len = 0;
        BYTE* buf = (BYTE*)malloc(cap);
        if (!buf) return 0;
        char marker[140];
        size_t markerLen = 0;
        if (boundary[0])
        {
            _snprintf(marker, 140, "--%s", strncmp(boundary, "--", 2) == 0 ? boundary + 2 : boundary);
            marker[139] = 0;
            markerLen = strlen(marker);
        }
        int rc = 0;
        while (!m_stop)
        {
            if (cap - len < 65536)
            {
                if (cap >= kMax)
                {
                    SetError(TR(L"поток не похож на MJPEG"), 0);
                    break;
                }
                BYTE* bigger = (BYTE*)realloc(buf, cap * 2);
                if (!bigger) break;
                buf = bigger;
                cap *= 2;
            }
            DWORD got = 0;
            if (!WinHttpReadData(req, buf + len, (DWORD)(cap - len), &got))
            {
                SetError(TR(L"связь прервалась (%lu)"), GetLastError());
                break;
            }
            if (got == 0)
            {
                // End of the response.
                if (single && len)
                {
                    Publish(buf, len);
                    rc = 1;
                    WaitForSingleObject(m_stopEvent, 100);      // snapshot URL: about 10 pictures a second
                }
                else
                    SetError(TR(L"сервер закрыл соединение"), 0);
                break;
            }
            len += got;
            if (single) continue;
            // As many complete pictures as there are in the buffer.
            for (;;)
            {
                size_t soi = Find(buf, len, "\xFF\xD8\xFF", 3, 0);
                if (soi == (size_t)-1)
                {
                    if (len > 2) { memmove(buf, buf + len - 2, 2); len = 2; }
                    break;
                }
                size_t end = (size_t)-1, next = (size_t)-1;
                size_t cl = ContentLength(buf, soi);
                if (cl > 0 && cl < kMax)
                {
                    if (soi + cl <= len) end = next = soi + cl;
                }
                else if (markerLen)
                {
                    size_t m = Find(buf, len, marker, markerLen, soi);
                    if (m != (size_t)-1)
                    {
                        end = m;
                        while (end > soi && (buf[end - 1] == '\n' || buf[end - 1] == '\r')) end--;
                        next = m;
                    }
                }
                else
                {
                    size_t eoi = Find(buf, len, "\xFF\xD9", 2, soi + 2);
                    if (eoi != (size_t)-1) end = next = eoi + 2;
                }
                if (end == (size_t)-1) break;              // not complete yet
                Publish(buf + soi, end - soi);
                memmove(buf, buf + next, len - next);
                len -= next;
            }
        }
        free(buf);
        return rc;
    }

    wchar_t m_url[512];
    CRITICAL_SECTION m_cs;
    HANDLE  m_thread = nullptr, m_stopEvent = nullptr;
    volatile LONG m_stop = 0;
    HINTERNET m_request = nullptr;
    Picture m_latest;
    ULONGLONG m_seq = 0, m_shownSeq = 0;
    DWORD   m_frameTick = 0, m_started = GetTickCount();
    wchar_t m_error[160] = L"";
    bool    m_noticeShown = false, m_logged = false, m_connectingShown = false;
};

// ---------------------------------------------------------------------------
// The camera's worker

class CameraRunner
{
public:
    int      index;
    wchar_t  path[512];
    EventSink sink;
    HANDLE   thread = nullptr, wake = nullptr;
    volatile LONG stop = 0;
    CRITICAL_SECTION cs;
    CamConfig config;
    bool     configChanged = true;
    CamRunStatus status = {};
    bool     preview = false, previewWanted = false;
    Picture  previewPic;
    bool     previewValid = false;
    DWORD    previewLast = 0;
};

static Source* CreateSource(const CamConfig& c, const EventSink& sink)
{
    CamConfig cfg = c;
    if (!cfg.imageFolder[0]) DefaultMediaFolder(MediaImages, cfg.imageFolder);
    if (!cfg.videoFolder[0]) DefaultMediaFolder(MediaVideo, cfg.videoFolder);
    switch (cfg.kind)
    {
    case SourceImages: return new ImageSource(sink, cfg.imageFolder);
    case SourceVideo:  return new VideoSource(sink, cfg);
    case SourceStream: return new StreamSource(sink, cfg.url);
    default:           return new TextSource(sink, cfg.text);
    }
}

// Does the change need a new source (not just pause / format)?
static bool SourceDiffers(const CamConfig& a, const CamConfig& b)
{
    return a.kind != b.kind || wcscmp(a.text, b.text) || wcscmp(a.imageFolder, b.imageFolder) || wcscmp(a.videoFolder, b.videoFolder) ||
           wcscmp(a.url, b.url) || a.audioMode != b.audioMode || wcscmp(a.audioDevice, b.audioDevice);
}

static void CopyPreview(CameraRunner* r, CamFrame& frame, bool always)
{
    DWORD now = GetTickCount();
    if (!always && now - r->previewLast < 40) return;          // up to 25 pictures a second
    r->previewLast = now;
    EnterCriticalSection(&r->cs);
    if (r->previewPic.Alloc((int)frame.width, (int)frame.height))
    {
        memcpy(r->previewPic.px, frame.Pixels(), (SIZE_T)frame.width * frame.height * 4);
        r->previewValid = true;
    }
    LeaveCriticalSection(&r->cs);
    r->sink.Post(EvPreviewFrame);
}

static DWORD WINAPI RunnerThread(LPVOID p)
{
    CameraRunner* r = (CameraRunner*)p;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HANDLE cam = INVALID_HANDLE_VALUE;
    CamFrame frame;
    Source* src = nullptr;
    CamConfig cfg;
    S2C_STATUS st = {};
    bool have = false, resend = true, force = true, waiting = false, previewWas = false;
    DWORD nextOpen = 0, nextStatus = 0, nextFormat = 0;
    ULONG lastWantW = 0, lastWantH = 0, lastWantFps = 0;

    while (!r->stop)
    {
        DWORD now = GetTickCount();
        // New settings from the panel.
        EnterCriticalSection(&r->cs);
        bool changed = r->configChanged;
        CamConfig next = r->config;
        r->configChanged = false;
        bool previewOn = r->previewWanted;
        LeaveCriticalSection(&r->cs);
        if (changed)
        {
            if (!src || SourceDiffers(cfg, next))
            {
                delete src;
                src = CreateSource(next, r->sink);
                force = true;
            }
            if (cfg.width != next.width || cfg.height != next.height || cfg.fps != next.fps) nextFormat = 0;
            cfg = next;
        }

        // The camera device (gone while the driver restarts).
        if (cam == INVALID_HANDLE_VALUE && (int)(now - nextOpen) >= 0)
        {
            cam = CamOpen(r->path);
            if (cam == INVALID_HANDLE_VALUE)
            {
                nextOpen = now + 2000;
                have = false;
            }
            else
            {
                nextStatus = 0;
                resend = true;
            }
        }
        if (cam != INVALID_HANDLE_VALUE && (int)(now - nextStatus) >= 0)
        {
            if (CamGetStatus(cam, &st)) have = true;
            else
            {
                AppLog(L"camera %d: lost the device (%lu)", r->index + 1, GetLastError());
                CloseHandle(cam);
                cam = INVALID_HANDLE_VALUE;
                have = false;
                nextOpen = now + 1000;
            }
            nextStatus = now + 500;
        }

        // The camera's size and rate: the chosen ones, or the source's own.
        if (have && src)
        {
            ULONG w = cfg.width, h = cfg.height, fps = cfg.fps, nw = 0, nh = 0, nfps = 0;
            bool native = src->Native(&nw, &nh, &nfps);
            if (!w || !h)
            {
                w = native ? nw : st.Width;
                h = native ? nh : st.Height;
            }
            if (!fps) fps = native ? nfps : st.Fps;
            NormalizeFormat(&w, &h, &fps);
            if (w != st.Width || h != st.Height || fps != st.Fps)
            {
                if ((int)(now - nextFormat) >= 0)
                {
                    DWORD err = CamSetFormat(cam, w, h, fps);
                    if (err == ERROR_SUCCESS)
                    {
                        AppLog(L"camera %d: format %lux%lu %lu fps (was %lux%lu %lu fps)", r->index + 1, w, h, fps, st.Width, st.Height, st.Fps);
                        CamGetStatus(cam, &st);
                        waiting = false;
                        EnterCriticalSection(&r->cs);
                        r->status.driver = st;              // the panel reads the new format with the event
                        LeaveCriticalSection(&r->cs);
                        r->sink.Post(EvFormatChanged);
                    }
                    else if (err == ERROR_BUSY)
                    {
                        if (!waiting || w != lastWantW || h != lastWantH || fps != lastWantFps)
                        {
                            EnterCriticalSection(&r->cs);
                            r->status.wantW = w;
                            r->status.wantH = h;
                            r->status.wantFps = fps;
                            LeaveCriticalSection(&r->cs);
                            r->sink.Post(EvFormatWaiting);
                        }
                        waiting = true;
                        nextFormat = now + 1000;
                    }
                    else
                    {
                        AppLog(L"camera %d: format %lux%lu %lu fps refused (%lu)", r->index + 1, w, h, fps, err);
                        nextFormat = now + 30000;
                    }
                }
            }
            else
                waiting = false;
            lastWantW = w;
            lastWantH = h;
            lastWantFps = fps;
            if (frame.width != st.Width || frame.height != st.Height)
            {
                if (frame.Resize(st.Width, st.Height)) force = true;
            }
        }

        DWORD wait = 200;
        if (src && frame.buffer && frame.width)
        {
            bool fresh = false;
            wait = src->Tick(frame.Pixels(), (int)frame.width, (int)frame.height, force, cfg.paused, &fresh);
            force = false;
            if ((fresh || resend) && cam != INVALID_HANDLE_VALUE)
            {
                if (CamSendFrame(cam, frame)) resend = false;
                else
                {
                    AppLog(L"camera %d: sending a picture failed (%lu)", r->index + 1, GetLastError());
                    nextStatus = 0;
                }
            }
            if (previewOn && (fresh || !previewWas)) CopyPreview(r, frame, !previewWas);
        }
        previewWas = previewOn && frame.width;

        // Status for the panel.
        EnterCriticalSection(&r->cs);
        CamRunStatus& s = r->status;
        ZeroMemory(&s, sizeof(s));
        s.deviceOpen = have;
        s.driver = st;
        s.formatWaiting = waiting;
        s.wantW = lastWantW;
        s.wantH = lastWantH;
        s.wantFps = lastWantFps;
        if (src)
        {
            src->Native(&s.sourceW, &s.sourceH, &s.sourceFps);
            src->Describe(&s);
        }
        LeaveCriticalSection(&r->cs);

        WaitForSingleObject(r->wake, wait > 500 ? 500 : wait);
    }
    delete src;
    if (cam != INVALID_HANDLE_VALUE)
    {
        CamSendTestPattern(cam);
        CloseHandle(cam);
    }
    CoUninitialize();
    return 0;
}

CameraRunner* RunnerStart(int index, const wchar_t* path, const CamConfig& config, HWND notify, UINT msg)
{
    CameraRunner* r = new CameraRunner;
    r->index = index;
    wcsncpy(r->path, path, 511);
    r->path[511] = 0;
    r->sink = { notify, msg, index };
    r->config = config;
    InitializeCriticalSection(&r->cs);
    r->wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    r->thread = CreateThread(nullptr, 0, RunnerThread, r, 0, nullptr);
    return r;
}

void RunnerConfigure(CameraRunner* r, const CamConfig& config)
{
    EnterCriticalSection(&r->cs);
    r->config = config;
    r->configChanged = true;
    LeaveCriticalSection(&r->cs);
    SetEvent(r->wake);
}

void RunnerStop(CameraRunner* r)
{
    if (!r) return;
    InterlockedExchange(&r->stop, 1);
    SetEvent(r->wake);
    if (r->thread)
    {
        WaitForSingleObject(r->thread, 15000);
        CloseHandle(r->thread);
    }
    CloseHandle(r->wake);
    DeleteCriticalSection(&r->cs);
    delete r;
}

void RunnerGetStatus(CameraRunner* r, CamRunStatus* status)
{
    EnterCriticalSection(&r->cs);
    *status = r->status;
    LeaveCriticalSection(&r->cs);
}

void RunnerSetPreview(CameraRunner* r, bool on)
{
    EnterCriticalSection(&r->cs);
    r->previewWanted = on;
    if (!on)
    {
        r->previewPic.Free();
        r->previewValid = false;
    }
    LeaveCriticalSection(&r->cs);
    SetEvent(r->wake);
}

bool RunnerGetPreviewFrame(CameraRunner* r, Picture* out)
{
    EnterCriticalSection(&r->cs);
    bool ok = r->previewValid && out->Alloc(r->previewPic.w, r->previewPic.h);
    if (ok) memcpy(out->px, r->previewPic.px, (SIZE_T)r->previewPic.w * r->previewPic.h * 4);
    LeaveCriticalSection(&r->cs);
    return ok;
}
