#pragma once

#include "miniaudio.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "AuPluginChain.h"
#include "LooperEngine.h"

/**
 * Full-duplex CoreAudio engine (via miniaudio) — the macOS counterpart of
 * the Android/Oboe AudioEngine. Capture and playback run in one duplex
 * ma_device; miniaudio aligns the two directions internally, so unlike the
 * Oboe version there is no manual input drain.
 *
 * Device selection works by index into the most recent enumeration
 * (refreshDevices()); index < 0 selects the system default device.
 *
 * The audio callback is allocation-free, lock-free and log-free. All buffers
 * are sized in start(). Control values cross threads via atomics only.
 */
class MacAudioEngine {
public:
    static constexpr int32_t kMaxLoopSeconds = 120;

    // The engine is a static global in the JNI bridge: without this, process
    // exit destroys the members (mutexes, buffers) while the CoreAudio IO
    // thread still fires the data callback → SIGSEGV. stop() uninits the
    // device synchronously, so the callback is provably finished before any
    // member dies.
    //
    // On the normal quit path this destructor no longer runs at all: the JVM
    // shutdown hook calls nativeShutdown() (jni_bridge.cpp), which stops the
    // device and then _Exit()s past every static destructor — see the comment
    // there for why plugin teardown at process exit is not survivable.
    ~MacAudioEngine() { stop(); }

    // App thread: re-enumerates capture/playback devices. Returns false if
    // the audio context could not be created.
    bool refreshDevices();

    std::vector<std::string> inputDeviceNames();
    std::vector<std::string> outputDeviceNames();
    int32_t defaultInputIndex();
    int32_t defaultOutputIndex();

    // App thread. Opens + starts the duplex device on the given indices
    // (into the last enumeration; < 0 = system default). Returns false (and
    // leaves everything closed) on any failure.
    bool start(int32_t inputIndex, int32_t outputIndex);

    // App thread. Stops and closes the device. Safe to call repeatedly.
    void stop();

    bool isRunning() const { return mRunning.load(std::memory_order_relaxed); }
    bool isDisconnected() const { return mDisconnected.load(std::memory_order_relaxed); }

    LooperEngine& looper() { return mLooper; }
    PluginChainManager& plugins() { return mPlugins; }

    // App thread: snapshot the current loop (mono samples). Serialized with
    // start()/stop() so the buffer cannot be reallocated mid-copy.
    int32_t copyLoop(float* dest, int32_t maxSamples) {
        std::lock_guard<std::mutex> lock(mLock);
        return mLooper.copyLoop(dest, maxSamples);
    }

    // App thread: replace the loop with mono samples and start playback.
    bool loadLoop(const float* data, int32_t numSamples) {
        std::lock_guard<std::mutex> lock(mLock);
        if (!isRunning()) {
            return false;
        }
        return mLooper.loadLoop(data, numSamples);
    }

    // App thread: stage real drum one-shots. Call before start(); while the
    // engine is running they only take effect on the next (re)start.
    void setDrumSamples(std::vector<float> kick, std::vector<float> snare,
                        std::vector<float> hat, int32_t sourceRate) {
        std::lock_guard<std::mutex> lock(mLock);
        mLooper.rhythm().setDrumSamples(std::move(kick), std::move(snare),
                                        std::move(hat), sourceRate);
    }

    void setMonitorEnabled(bool enabled) { mMonitorEnabled.store(enabled, std::memory_order_relaxed); }
    void setInputGain(float gain) { mInputGain.store(gain, std::memory_order_relaxed); }
    void setOutputGain(float gain) { mOutputGain.store(gain, std::memory_order_relaxed); }

    int32_t sampleRate() const { return mSampleRate; }
    int32_t framesPerBurst() const { return mFramesPerBurst; }

    // Peak meters: max |sample| since the last read (read resets to 0).
    // Input is pre-chain (raw interface level for gain staging), FX is
    // post-chain (what the loop and monitor hear), output is post-limiter.
    float readInputPeak() { return mInputPeak.exchange(0.0f, std::memory_order_relaxed); }
    float readFxPeak() { return mFxPeak.exchange(0.0f, std::memory_order_relaxed); }
    float readOutputPeak() { return mOutputPeak.exchange(0.0f, std::memory_order_relaxed); }

private:
    static void dataTrampoline(ma_device* device, void* output,
                               const void* input, ma_uint32 frameCount);
    static void notificationTrampoline(const ma_device_notification* notification);

    void onAudio(float* output, const float* input, int32_t numFrames);
    void processChunk(float* output, const float* input, int32_t numFrames);

    bool ensureContextLocked();
    void stopLocked();

    std::mutex mLock;  // guards start/stop/enumeration from app threads (never the callback)

    ma_context mContext{};
    bool mContextReady = false;

    // Last enumeration snapshot (guarded by mLock).
    std::vector<ma_device_id> mInputIds;
    std::vector<ma_device_id> mOutputIds;
    std::vector<std::string> mInputNames;
    std::vector<std::string> mOutputNames;
    int32_t mDefaultInput = -1;
    int32_t mDefaultOutput = -1;

    std::unique_ptr<ma_device> mDevice;

    LooperEngine mLooper;
    PluginChainManager mPlugins;

    // Pre-allocated conversion buffers (sized in start()).
    std::vector<float> mMonoIn;
    std::vector<float> mMonoOut;
    int32_t mMaxBlockFrames = 0;

    int32_t mSampleRate = 0;
    int32_t mFramesPerBurst = 0;
    int32_t mInputChannels = 0;
    int32_t mOutputChannels = 0;

    std::atomic<bool> mRunning{false};
    std::atomic<bool> mDisconnected{false};
    std::atomic<bool> mMonitorEnabled{true};
    std::atomic<float> mInputGain{1.0f};
    std::atomic<float> mOutputGain{1.0f};
    std::atomic<float> mInputPeak{0.0f};
    std::atomic<float> mFxPeak{0.0f};
    std::atomic<float> mOutputPeak{0.0f};
};
