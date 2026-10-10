// CoreAudio output (macOS; see backend.h): the DefaultOutput audio unit, which plays on the
// system's default output device and follows it when the user picks another one (converting to
// that device's rate itself). The unit takes interleaved float32 stereo at the default device's
// rate when the backend starts, so that the mixer runs at the device rate (no resampling stage
// while the device stays the same), and pulls it from its render callback on CoreAudio's real-time
// I/O thread, which is the audio thread here. NaN is silence and the rest is clamped to [-1, 1], as
// the other backends do. The null backend runs instead when no output unit can be started.
#ifdef __APPLE__
#include "backend.h"
#include "../core/log.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string>
#include <thread>

namespace audio {
namespace {

// A property of the audio object id (global scope, main element) into value.
template <typename T>
bool objectProperty(AudioObjectID id, AudioObjectPropertySelector selector, T& value) {
    const AudioObjectPropertyAddress address{selector, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 size = sizeof(T);
    return AudioObjectGetPropertyData(id, &address, 0, nullptr, &size, &value) == noErr && size == sizeof(T);
}

std::string deviceName(AudioObjectID device) {
    CFStringRef name = nullptr;
    if (!objectProperty(device, kAudioObjectPropertyName, name) || !name) return "unknown device";
    char buf[256];
    std::string s = CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8) ? buf : "unnamed device";
    CFRelease(name);
    return s;
}

class CoreAudioBackend final : public Backend {
public:
    ~CoreAudioBackend() override {
        stop();
        close();
    }

    // Makes and starts nothing yet: finds the default output unit and sets its format and callback.
    // false: why says what failed.
    bool open(std::string& why) {
        AudioComponentDescription desc{};
        desc.componentType = kAudioUnitType_Output;
        desc.componentSubType = kAudioUnitSubType_DefaultOutput;
        desc.componentManufacturer = kAudioUnitManufacturer_Apple;
        AudioComponent component = AudioComponentFindNext(nullptr, &desc);
        if (!component) {
            why = "no default output unit";
            return false;
        }
        OSStatus err = AudioComponentInstanceNew(component, &unit_);
        if (err != noErr || !unit_) {
            unit_ = nullptr;
            return failed(why, "AudioComponentInstanceNew", err);
        }
        // The device side of the unit (its output scope) has the device's rate.
        AudioStreamBasicDescription device{};
        UInt32 size = sizeof(device);
        err = AudioUnitGetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &device, &size);
        rate_ = fallbackDeviceRate(err == noErr && device.mSampleRate > 0.0 ? (unsigned long)(device.mSampleRate + 0.5) : 0ul);

        AudioStreamBasicDescription format{};
        format.mSampleRate = Float64(rate_);
        format.mFormatID = kAudioFormatLinearPCM;
        format.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;   // interleaved
        format.mChannelsPerFrame = 2;
        format.mBitsPerChannel = 32;
        format.mFramesPerPacket = 1;
        format.mBytesPerFrame = 8;
        format.mBytesPerPacket = 8;
        err = AudioUnitSetProperty(unit_, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &format, sizeof(format));
        if (err != noErr) return failed(why, "StreamFormat", err);

        AURenderCallbackStruct callback{};
        callback.inputProc = &CoreAudioBackend::render;
        callback.inputProcRefCon = this;
        err = AudioUnitSetProperty(unit_, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &callback, sizeof(callback));
        if (err != noErr) return failed(why, "SetRenderCallback", err);
        err = AudioUnitInitialize(unit_);
        if (err != noErr) return failed(why, "AudioUnitInitialize", err);
        initialized_ = true;

        AudioObjectID dev = kAudioObjectUnknown;
        size = sizeof(dev);
        UInt32 frames = 0;
        if (AudioUnitGetProperty(unit_, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, &size) == noErr &&
            dev != kAudioObjectUnknown) {
            name_ = deviceName(dev);
            objectProperty(dev, kAudioDevicePropertyBufferFrameSize, frames);
        }
        bufferFrames_ = frames > 0 ? int(frames) : 512;
        return true;
    }

    bool start(RenderFn fn, void* user) override {
        if (running_) return status.deviceOpen.load();
        if (!unit_ || !initialized_) return false;
        fn_ = fn;
        user_ = user;
        active_.store(true);
        const OSStatus err = AudioOutputUnitStart(unit_);
        if (err != noErr) {
            active_.store(false);
            LOGW("audio: CoreAudio output did not start (OSStatus %d)", int(err));
            return false;
        }
        running_ = true;
        status.sampleRate = rate_;
        status.bufferFrames = bufferFrames_;
        status.deviceOpen = true;
        LOGI("audio: CoreAudio '%s' %d Hz, 2 ch, float32, device buffer %d frames (%.1f ms)", name_.c_str(), rate_, bufferFrames_,
             1000.0 * bufferFrames_ / rate_);
        return true;
    }

    void stop() override {
        if (!running_) return;
        // The callback renders silence from now on; one in progress is waited for, so that the
        // render function is never called once stop() has returned.
        active_.store(false);
        while (inCallback_.load() > 0) std::this_thread::yield();
        AudioOutputUnitStop(unit_);
        running_ = false;
        status.deviceOpen = false;
    }

private:
    bool failed(std::string& why, const char* step, OSStatus err) {
        why = std::string(step) + " failed (OSStatus " + std::to_string(int(err)) + ")";
        close();
        return false;
    }

    void close() {
        if (!unit_) return;
        if (initialized_) AudioUnitUninitialize(unit_);
        AudioComponentInstanceDispose(unit_);
        unit_ = nullptr;
        initialized_ = false;
    }

    static OSStatus render(void* refCon, AudioUnitRenderActionFlags* flags, const AudioTimeStamp*, UInt32, UInt32 frames,
                           AudioBufferList* io) {
        CoreAudioBackend* self = static_cast<CoreAudioBackend*>(refCon);
        if (!io || io->mNumberBuffers < 1 || !io->mBuffers[0].mData) return noErr;
        AudioBuffer& buf = io->mBuffers[0];
        float* out = static_cast<float*>(buf.mData);
        const UInt32 n = std::min<UInt32>(frames, buf.mDataByteSize / 8u);
        self->inCallback_.fetch_add(1);
        const bool active = self->active_.load();
        if (active && n > 0) self->fn_(self->user_, out, int(n), self->rate_);
        self->inCallback_.fetch_sub(1);
        if (!active) {
            std::fill(out, out + size_t(buf.mDataByteSize / 4u), 0.0f);
            if (flags) *flags |= kAudioUnitRenderAction_OutputIsSilence;
            return noErr;
        }
        for (UInt32 i = 0; i < 2 * n; ++i) {
            float v = out[i];
            if (v != v) v = 0.0f;
            out[i] = v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
        }
        // A buffer longer than the frames asked for (never seen): its tail is silence.
        std::fill(out + size_t(2 * n), out + size_t(buf.mDataByteSize / 4u), 0.0f);
        return noErr;
    }

    AudioComponentInstance unit_ = nullptr;
    bool initialized_ = false, running_ = false;
    int rate_ = 48000, bufferFrames_ = 512;
    std::string name_ = "default output";
    RenderFn fn_ = nullptr;
    void* user_ = nullptr;
    std::atomic<bool> active_{false};
    std::atomic<int> inCallback_{0};
};

}  // namespace

std::unique_ptr<Backend> createBackend() {
    if (chooseBackend(std::getenv("SCACELITH_AUDIO"), std::getenv("SCACELITH_AUDIO_DUMP")) == BackendChoice::Device) {
        std::unique_ptr<CoreAudioBackend> b(new CoreAudioBackend());
        std::string why;
        if (b->open(why)) return b;
        LOGI("audio: CoreAudio output not available (%s), running silent", why.c_str());
    }
    return createNullBackend();
}

}  // namespace audio
#endif
