// WASAPI shared-mode, event-driven output. The mixer runs at the device mix rate (no resampling
// stage) and its float stereo is converted to the mix format (float32 / int16 / int24 / int32,
// any channel count: L/R go to the front pair, mono gets their average). Device loss,
// default-device changes and stalls reopen the device; with no device the thread retries with a
// back-off and stays silent. MMCSS "Pro Audio" priority when available.
#ifdef _WIN32
#include "backend.h"
#include "dsp.h"
#include "../core/log.h"
#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace audio {
namespace {

// GUIDs defined locally (independent of which import library provides them).
const CLSID kClsidEnumerator = {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const IID kIidEnumerator = {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const IID kIidAudioClient = {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
const IID kIidRenderClient = {0xF294ACFC, 0x3146, 0x4483, {0xA7, 0xBF, 0xAD, 0xDC, 0xA7, 0xC2, 0x60, 0xE2}};
const IID kIidNotificationClient = {0x7991EEC9, 0x7E89, 0x4D85, {0x83, 0x90, 0x6C, 0x70, 0x3C, 0xEC, 0x60, 0xC0}};
const IID kIidUnknown = {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
const GUID kSubFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const GUID kSubPcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};

template <class T> void safeRelease(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// Wakes the audio thread when the default render device changes (or a device appears while none
// is open). Called on a system thread: only signals an event.
class DeviceNotifier final : public IMMNotificationClient {
public:
    DeviceNotifier(HANDLE ev, const std::atomic<bool>* open) : ev_(ev), open_(open) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ULONG(InterlockedIncrement(&ref_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        LONG r = InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return ULONG(r);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (IsEqualIID(riid, kIidUnknown) || IsEqualIID(riid, kIidNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) SetEvent(ev_);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return wakeIfClosed(); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return wakeIfClosed(); }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    HRESULT wakeIfClosed() {
        if (!open_->load()) SetEvent(ev_);
        return S_OK;
    }
    LONG ref_ = 1;
    HANDLE ev_;
    const std::atomic<bool>* open_;
};

enum class SampleType { Float32, Int16, Int24, Int32 };

class WasapiBackend final : public Backend {
public:
    ~WasapiBackend() override { stop(); }

    bool start(RenderFn fn, void* user) override {
        if (thread_.joinable()) return status.deviceOpen.load();
        fn_ = fn;
        user_ = user;
        quitEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        changeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!quitEvent_ || !changeEvent_) {
            LOGE("audio: CreateEvent failed");
            closeEvents();
            return false;
        }
        firstDone_ = false;
        firstOk_ = false;
        thread_ = std::thread([this] { run(); });
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait_for(lk, std::chrono::seconds(3), [this] { return firstDone_; });
        return firstOk_;
    }

    void stop() override {
        if (!thread_.joinable()) return;
        SetEvent(quitEvent_);
        thread_.join();
        closeEvents();
        status.deviceOpen = false;
    }

private:
    using End = StreamEnd;

    void closeEvents() {
        if (quitEvent_) CloseHandle(quitEvent_);
        if (changeEvent_) CloseHandle(changeEvent_);
        quitEvent_ = changeEvent_ = nullptr;
    }

    bool quitting() const { return WaitForSingleObject(quitEvent_, 0) == WAIT_OBJECT_0; }

    void reportFirst(bool ok) {
        std::lock_guard<std::mutex> lk(m_);
        if (firstDone_) return;
        firstDone_ = true;
        firstOk_ = ok;
        cv_.notify_all();
    }

    void run() {
        HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
        if (!mmcss) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        dsp::DenormalGuard guard;

        IMMDeviceEnumerator* enumr = nullptr;
        DeviceNotifier* notifier = nullptr;
        ReopenBackoff backoff;
        bool loggedNoDevice = false;
        while (!quitting()) {
            if (!enumr) {
                HRESULT hr = CoCreateInstance(kClsidEnumerator, nullptr, CLSCTX_ALL, kIidEnumerator, (void**)&enumr);
                if (SUCCEEDED(hr) && enumr) {
                    notifier = new DeviceNotifier(changeEvent_, &status.deviceOpen);
                    if (FAILED(enumr->RegisterEndpointNotificationCallback(notifier))) safeRelease(notifier);
                } else {
                    enumr = nullptr;
                    if (!loggedNoDevice) LOGW("audio: MMDeviceEnumerator unavailable (hr=0x%08lx)", (unsigned long)hr);
                    loggedNoDevice = true;
                }
            }
            bool opened = enumr && open(enumr, !loggedNoDevice);
            reportFirst(opened);
            End end = End::Stalled;
            bool healthy = false;
            if (opened) {
                loggedNoDevice = false;
                end = stream(healthy);
                close();
                if (end == End::Quit) break;
                status.restarts.fetch_add(1);
            } else {
                loggedNoDevice = true;
            }
            const int waitMs = backoff.waitMs(opened, end, healthy);
            if (waitMs == 0) continue;  // default changed / working device lost: reopen right away
            HANDLE hs[2] = {quitEvent_, changeEvent_};
            WaitForMultipleObjects(2, hs, FALSE, DWORD(waitMs));
        }
        reportFirst(false);
        if (enumr && notifier) enumr->UnregisterEndpointNotificationCallback(notifier);
        safeRelease(notifier);
        safeRelease(enumr);
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        if (SUCCEEDED(hrCo)) CoUninitialize();
    }

    bool parseFormat(const WAVEFORMATEX* wf) {
        channels_ = wf->nChannels;
        rate_ = int(wf->nSamplesPerSec);
        blockAlign_ = wf->nBlockAlign;
        chL_ = 0;
        chR_ = channels_ > 1 ? 1 : 0;
        bool isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        bool isPcm = wf->wFormatTag == WAVE_FORMAT_PCM;
        int bits = wf->wBitsPerSample;
        if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
            const WAVEFORMATEXTENSIBLE* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
            isFloat = IsEqualGUID(ext->SubFormat, kSubFloat);
            isPcm = IsEqualGUID(ext->SubFormat, kSubPcm);
            DWORD mask = ext->dwChannelMask;
            auto indexOf = [mask](DWORD bit) {
                if (!(mask & bit)) return -1;
                int idx = 0;
                for (DWORD b = 1; b < bit; b <<= 1) idx += (mask & b) ? 1 : 0;
                return idx;
            };
            int l = indexOf(SPEAKER_FRONT_LEFT), r = indexOf(SPEAKER_FRONT_RIGHT);
            if (l >= 0 && r >= 0 && l < channels_ && r < channels_) {
                chL_ = l;
                chR_ = r;
            }
        }
        if (channels_ < 1 || rate_ < kMinDeviceRate || rate_ > kMaxDeviceRate) return false;
        if (isFloat && bits == 32) type_ = SampleType::Float32;
        else if (isPcm && bits == 16) type_ = SampleType::Int16;
        else if (isPcm && bits == 24) type_ = SampleType::Int24;
        else if (isPcm && bits == 32) type_ = SampleType::Int32;
        else return false;
        return blockAlign_ == channels_ * (bits / 8);
    }

    bool open(IMMDeviceEnumerator* enumr, bool verbose) {
        HRESULT hr = enumr->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
        if (FAILED(hr) || !device_) {
            if (verbose) LOGW("audio: no default output device (hr=0x%08lx), audio stays silent and retries", (unsigned long)hr);
            device_ = nullptr;
            return false;
        }
        WAVEFORMATEX* wf = nullptr;
        WAVEFORMATEXTENSIBLE fallback{};
        bool converted = false;
        REFERENCE_TIME defPeriod = 0, minPeriod = 0;
        const char* step = "Activate";
        hr = device_->Activate(kIidAudioClient, CLSCTX_ALL, nullptr, (void**)&client_);
        if (SUCCEEDED(hr)) { step = "GetMixFormat"; hr = client_->GetMixFormat(&wf); }
        if (SUCCEEDED(hr) && !parseFormat(wf)) {
            // Unusual mix format: hand the engine float32 stereo and let it convert.
            fallback.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
            fallback.Format.nChannels = 2;
            fallback.Format.nSamplesPerSec = DWORD(fallbackDeviceRate(wf->nSamplesPerSec));
            fallback.Format.wBitsPerSample = 32;
            fallback.Format.nBlockAlign = 8;
            fallback.Format.nAvgBytesPerSec = fallback.Format.nSamplesPerSec * 8;
            fallback.Format.cbSize = 22;
            fallback.Samples.wValidBitsPerSample = 32;
            fallback.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
            fallback.SubFormat = kSubFloat;
            converted = parseFormat(&fallback.Format);
            if (!converted) { step = "unsupported mix format"; hr = E_FAIL; }
        }
        if (SUCCEEDED(hr)) { step = "GetDevicePeriod"; hr = client_->GetDevicePeriod(&defPeriod, &minPeriod); }
        if (SUCCEEDED(hr)) {
            step = "Initialize";
            REFERENCE_TIME dur = std::max<REFERENCE_TIME>(2 * defPeriod, 100000);  // ~20 ms
            DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
            if (converted) flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
            hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, dur, 0, converted ? &fallback.Format : wf, nullptr);
        }
        if (wf) CoTaskMemFree(wf);
        if (SUCCEEDED(hr)) {
            step = "SetEventHandle";
            audioEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            hr = audioEvent_ ? client_->SetEventHandle(audioEvent_) : E_FAIL;
        }
        if (SUCCEEDED(hr)) { step = "GetBufferSize"; hr = client_->GetBufferSize(&bufferFrames_); }
        periodFrames_ = UINT32(std::max<REFERENCE_TIME>(1, defPeriod) * rate_ / 10000000);
        periodFrames_ = std::max<UINT32>(64, std::min(periodFrames_, bufferFrames_));
        if (SUCCEEDED(hr)) { step = "GetService"; hr = client_->GetService(kIidRenderClient, (void**)&render_); }
        if (SUCCEEDED(hr)) {
            // Latency target: one period + max(half a period, 5 ms), raised on underruns.
            targetFrames_ = std::min(bufferFrames_, periodFrames_ + std::max(periodFrames_ / 2, UINT32(rate_ / 200)));
            BYTE* data = nullptr;
            if (SUCCEEDED(render_->GetBuffer(targetFrames_, &data))) render_->ReleaseBuffer(targetFrames_, AUDCLNT_BUFFERFLAGS_SILENT);
            scratch_.assign(size_t(bufferFrames_) * 2, 0.0f);
            status.sampleRate = rate_;
            status.bufferFrames = int(targetFrames_);
            status.deviceOpen = true;
            static const char* names[] = {"float32", "int16", "int24", "int32"};
            LOGI("audio: WASAPI shared %d Hz, %d ch, %s%s, buffer %u frames (%.1f ms), period %.1f ms, fill target %.1f ms",
                 rate_, channels_, names[int(type_)], converted ? " (engine-converted)" : "", unsigned(bufferFrames_),
                 1000.0 * bufferFrames_ / rate_, defPeriod / 10000.0, 1000.0 * targetFrames_ / rate_);
            return true;
        }
        LOGW("audio: WASAPI open failed at %s (hr=0x%08lx)", step, (unsigned long)hr);
        close();
        return false;
    }

    void close() {
        status.deviceOpen = false;
        safeRelease(render_);
        safeRelease(client_);
        safeRelease(device_);
        if (audioEvent_) CloseHandle(audioEvent_);
        audioEvent_ = nullptr;
    }

    // 'healthy': the stream delivered at least a second of audio before it ended.
    End stream(bool& healthy) {
        healthy = false;
        HRESULT hr = client_->Start();
        if (FAILED(hr)) {
            LOGW("audio: IAudioClient::Start failed (hr=0x%08lx)", (unsigned long)hr);
            return End::Stalled;
        }
        HANDLE hs[3] = {quitEvent_, changeEvent_, audioEvent_};
        int timeouts = 0;
        uint64_t rendered = 0;
        unsigned dryBuffers = 0;  // the first 4 of each stream are tolerated: not counted, no adaptation
        End end = End::Quit;
        for (;;) {
            DWORD w = WaitForMultipleObjects(3, hs, FALSE, 200);
            if (w == WAIT_OBJECT_0) { end = End::Quit; break; }
            if (w == WAIT_OBJECT_0 + 1) {
                LOGI("audio: default output device changed, reopening");
                end = End::Changed;
                break;
            }
            if (w == WAIT_TIMEOUT) {
                if (++timeouts >= 10) {
                    LOGW("audio: device delivers no events, reopening");
                    end = End::Stalled;
                    break;
                }
                continue;
            }
            if (w != WAIT_OBJECT_0 + 2) { end = End::Lost; break; }
            timeouts = 0;
            UINT32 padding = 0;
            hr = client_->GetCurrentPadding(&padding);
            if (FAILED(hr)) {
                LOGW("audio: device lost (hr=0x%08lx), reopening", (unsigned long)hr);
                end = End::Lost;
                break;
            }
            if (padding == 0 && ++dryBuffers > 4) {  // buffer ran dry: glitch -> allow more latency
                status.underruns.fetch_add(1);
                targetFrames_ = std::min(bufferFrames_, targetFrames_ + UINT32(rate_ / 400));
                status.bufferFrames = int(targetFrames_);
            }
            if (padding >= targetFrames_) continue;
            UINT32 avail = targetFrames_ - padding;
            BYTE* data = nullptr;
            hr = render_->GetBuffer(avail, &data);
            if (FAILED(hr) || !data) {
                LOGW("audio: GetBuffer failed (hr=0x%08lx), reopening", (unsigned long)hr);
                end = End::Lost;
                break;
            }
            fn_(user_, scratch_.data(), int(avail), rate_);
            convert(scratch_.data(), data, int(avail));
            hr = render_->ReleaseBuffer(avail, 0);
            if (FAILED(hr)) { end = End::Lost; break; }
            rendered += avail;
        }
        client_->Stop();
        healthy = rendered >= uint64_t(rate_);
        return end;
    }

    void convert(const float* in, BYTE* out, int frames) const {
        const int ch = channels_;
        for (int i = 0; i < frames; ++i) {
            float l = in[2 * i], r = in[2 * i + 1];
            BYTE* f = out + size_t(i) * size_t(blockAlign_);
            for (int c = 0; c < ch; ++c) {
                float v = ch == 1 ? 0.5f * (l + r) : (c == chL_ ? l : (c == chR_ ? r : 0.0f));
                if (v != v) v = 0.0f;  // NaN: silence, never handed to the engine (the clamp keeps it)
                v = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
                switch (type_) {
                    case SampleType::Float32: std::memcpy(f + c * 4, &v, 4); break;
                    case SampleType::Int16: {
                        int16_t s = int16_t(std::lrint(v * 32767.0f));
                        std::memcpy(f + c * 2, &s, 2);
                    } break;
                    case SampleType::Int24: {
                        int32_t s = int32_t(std::lrint(v * 8388607.0f));
                        f[c * 3 + 0] = BYTE(s & 0xFF);
                        f[c * 3 + 1] = BYTE((s >> 8) & 0xFF);
                        f[c * 3 + 2] = BYTE((s >> 16) & 0xFF);
                    } break;
                    case SampleType::Int32: {
                        int32_t s = int32_t(std::llrint(double(v) * 2147483647.0));
                        std::memcpy(f + c * 4, &s, 4);
                    } break;
                }
            }
        }
    }

    RenderFn fn_ = nullptr;
    void* user_ = nullptr;
    std::thread thread_;
    HANDLE quitEvent_ = nullptr, changeEvent_ = nullptr, audioEvent_ = nullptr;
    std::mutex m_;
    std::condition_variable cv_;
    bool firstDone_ = false, firstOk_ = false;

    IMMDevice* device_ = nullptr;
    IAudioClient* client_ = nullptr;
    IAudioRenderClient* render_ = nullptr;
    UINT32 bufferFrames_ = 0, periodFrames_ = 480, targetFrames_ = 960;
    int channels_ = 2, rate_ = 48000, blockAlign_ = 8, chL_ = 0, chR_ = 1;
    SampleType type_ = SampleType::Float32;
    std::vector<float> scratch_;
};

}  // namespace

std::unique_ptr<Backend> createBackend() { return std::unique_ptr<Backend>(new WasapiBackend()); }

}  // namespace audio
#endif
