#include "MacAudioEngine.h"

#include <algorithm>
#include <cstdio>

namespace {
// Duplex period: 256 frames @ 48 kHz ≈ 5.3 ms per direction — low enough for
// live monitoring, high enough to stay glitch-free across the duplex ring
// buffer even when capture and playback sit on different devices.
constexpr int32_t kPeriodFrames = 256;
constexpr int32_t kMaxBlockFrames = 8192;
}  // namespace

bool MacAudioEngine::ensureContextLocked() {
    if (mContextReady) {
        return true;
    }
    ma_context_config config = ma_context_config_init();
    if (ma_context_init(nullptr, 0, &config, &mContext) != MA_SUCCESS) {
        std::fprintf(stderr, "NicheLooper: failed to init audio context\n");
        return false;
    }
    mContextReady = true;
    return true;
}

bool MacAudioEngine::refreshDevices() {
    std::lock_guard<std::mutex> lock(mLock);
    if (!ensureContextLocked()) {
        return false;
    }

    ma_device_info* playbackInfos = nullptr;
    ma_uint32 playbackCount = 0;
    ma_device_info* captureInfos = nullptr;
    ma_uint32 captureCount = 0;
    if (ma_context_get_devices(&mContext, &playbackInfos, &playbackCount,
                               &captureInfos, &captureCount) != MA_SUCCESS) {
        return false;
    }

    mInputIds.clear();
    mInputNames.clear();
    mDefaultInput = -1;
    for (ma_uint32 i = 0; i < captureCount; ++i) {
        mInputIds.push_back(captureInfos[i].id);
        mInputNames.emplace_back(captureInfos[i].name);
        if (captureInfos[i].isDefault) {
            mDefaultInput = static_cast<int32_t>(i);
        }
    }

    mOutputIds.clear();
    mOutputNames.clear();
    mDefaultOutput = -1;
    for (ma_uint32 i = 0; i < playbackCount; ++i) {
        mOutputIds.push_back(playbackInfos[i].id);
        mOutputNames.emplace_back(playbackInfos[i].name);
        if (playbackInfos[i].isDefault) {
            mDefaultOutput = static_cast<int32_t>(i);
        }
    }
    return true;
}

std::vector<std::string> MacAudioEngine::inputDeviceNames() {
    std::lock_guard<std::mutex> lock(mLock);
    return mInputNames;
}

std::vector<std::string> MacAudioEngine::outputDeviceNames() {
    std::lock_guard<std::mutex> lock(mLock);
    return mOutputNames;
}

int32_t MacAudioEngine::defaultInputIndex() {
    std::lock_guard<std::mutex> lock(mLock);
    return mDefaultInput;
}

int32_t MacAudioEngine::defaultOutputIndex() {
    std::lock_guard<std::mutex> lock(mLock);
    return mDefaultOutput;
}

bool MacAudioEngine::start(int32_t inputIndex, int32_t outputIndex) {
    std::lock_guard<std::mutex> lock(mLock);
    stopLocked();
    mDisconnected.store(false, std::memory_order_relaxed);

    if (!ensureContextLocked()) {
        return false;
    }

    ma_device_id* inputId = nullptr;
    if (inputIndex >= 0 && inputIndex < static_cast<int32_t>(mInputIds.size())) {
        inputId = &mInputIds[static_cast<size_t>(inputIndex)];
    }
    ma_device_id* outputId = nullptr;
    if (outputIndex >= 0 && outputIndex < static_cast<int32_t>(mOutputIds.size())) {
        outputId = &mOutputIds[static_cast<size_t>(outputIndex)];
    }

    ma_device_config config = ma_device_config_init(ma_device_type_duplex);
    config.sampleRate = 0;  // native rate of the playback device
    config.periodSizeInFrames = kPeriodFrames;
    config.performanceProfile = ma_performance_profile_low_latency;
    config.capture.pDeviceID = inputId;
    config.capture.format = ma_format_f32;
    config.capture.channels = 0;  // native channel count
    config.capture.shareMode = ma_share_mode_shared;
    config.playback.pDeviceID = outputId;
    config.playback.format = ma_format_f32;
    config.playback.channels = 0;
    config.playback.shareMode = ma_share_mode_shared;
    config.dataCallback = &MacAudioEngine::dataTrampoline;
    config.notificationCallback = &MacAudioEngine::notificationTrampoline;
    config.pUserData = this;

    mDevice = std::make_unique<ma_device>();
    if (ma_device_init(&mContext, &config, mDevice.get()) != MA_SUCCESS) {
        std::fprintf(stderr, "NicheLooper: failed to open duplex device (in=%d out=%d)\n",
                     inputIndex, outputIndex);
        mDevice.reset();
        return false;
    }

    mSampleRate = static_cast<int32_t>(mDevice->sampleRate);
    mInputChannels = static_cast<int32_t>(mDevice->capture.channels);
    mOutputChannels = static_cast<int32_t>(mDevice->playback.channels);
    mFramesPerBurst = static_cast<int32_t>(mDevice->playback.internalPeriodSizeInFrames);

    mLooper.prepare(mSampleRate, kMaxLoopSeconds);
    mPlugins.prepare(mSampleRate, kMaxBlockFrames);

    mMaxBlockFrames = kMaxBlockFrames;
    mMonoIn.assign(static_cast<size_t>(mMaxBlockFrames), 0.0f);
    mMonoOut.assign(static_cast<size_t>(mMaxBlockFrames), 0.0f);

    if (ma_device_start(mDevice.get()) != MA_SUCCESS) {
        std::fprintf(stderr, "NicheLooper: failed to start duplex device\n");
        ma_device_uninit(mDevice.get());
        mDevice.reset();
        return false;
    }

    mRunning.store(true, std::memory_order_relaxed);
    std::fprintf(stderr, "NicheLooper: engine started (rate=%d burst=%d inCh=%d outCh=%d)\n",
                 mSampleRate, mFramesPerBurst, mInputChannels, mOutputChannels);
    return true;
}

void MacAudioEngine::stop() {
    std::lock_guard<std::mutex> lock(mLock);
    stopLocked();
}

void MacAudioEngine::stopLocked() {
    // Clear the running flag BEFORE uninit so the "stopped" notification of
    // a deliberate stop is not misread as a disconnect.
    mRunning.store(false, std::memory_order_relaxed);
    if (mDevice) {
        ma_device_uninit(mDevice.get());
        mDevice.reset();
    }
}

void MacAudioEngine::dataTrampoline(ma_device* device, void* output,
                                    const void* input, ma_uint32 frameCount) {
    auto* self = static_cast<MacAudioEngine*>(device->pUserData);
    self->onAudio(static_cast<float*>(output), static_cast<const float*>(input),
                  static_cast<int32_t>(frameCount));
}

void MacAudioEngine::notificationTrampoline(const ma_device_notification* notification) {
    auto* self = static_cast<MacAudioEngine*>(notification->pDevice->pUserData);
    if (notification->type == ma_device_notification_type_stopped) {
        // A stop while we still believe we are running = the backend killed
        // the stream (device unplugged, format change, …). The app layer
        // polls isDisconnected() and shuts the engine down.
        if (self->mRunning.load(std::memory_order_relaxed)) {
            self->mDisconnected.store(true, std::memory_order_relaxed);
            self->mRunning.store(false, std::memory_order_relaxed);
        }
    }
}

void MacAudioEngine::onAudio(float* output, const float* input, int32_t numFrames) {
    // frameCount is not bounded by the period size, so process in chunks of
    // the pre-allocated block size.
    int32_t offset = 0;
    while (offset < numFrames) {
        const int32_t chunk = std::min(numFrames - offset, mMaxBlockFrames);
        processChunk(output + static_cast<size_t>(offset) * mOutputChannels,
                     input != nullptr
                             ? input + static_cast<size_t>(offset) * mInputChannels
                             : nullptr,
                     chunk);
        offset += chunk;
    }
}

void MacAudioEngine::processChunk(float* output, const float* input, int32_t numFrames) {
    // Downmix input to mono by SUMMING channels (not averaging): a guitar
    // sits on one channel of a stereo interface, and averaging would cost
    // 6 dB against a silent second channel.
    const float inGain = mInputGain.load(std::memory_order_relaxed);
    float inPeak = 0.0f;
    for (int32_t i = 0; i < numFrames; ++i) {
        float sum = 0.0f;
        if (input != nullptr) {
            const float* frame = input + static_cast<size_t>(i) * mInputChannels;
            for (int32_t c = 0; c < mInputChannels; ++c) {
                sum += frame[c];
            }
        }
        const float mono = sum * inGain;
        mMonoIn[static_cast<size_t>(i)] = mono;
        const float magnitude = mono < 0.0f ? -mono : mono;
        if (magnitude > inPeak) {
            inPeak = magnitude;
        }
    }
    if (inPeak > mInputPeak.load(std::memory_order_relaxed)) {
        mInputPeak.store(inPeak, std::memory_order_relaxed);
    }

    // Active plugin chain (amp sim etc.) processes the live input in place:
    // both the loop recording and the monitor hear the processed sound,
    // while existing loop content keeps the sound it was recorded with.
    mPlugins.processBlock(mMonoIn.data(), numFrames);

    float fxPeak = 0.0f;
    for (int32_t i = 0; i < numFrames; ++i) {
        const float sample = mMonoIn[static_cast<size_t>(i)];
        const float magnitude = sample < 0.0f ? -sample : sample;
        if (magnitude > fxPeak) {
            fxPeak = magnitude;
        }
    }
    if (fxPeak > mFxPeak.load(std::memory_order_relaxed)) {
        mFxPeak.store(fxPeak, std::memory_order_relaxed);
    }

    mLooper.process(mMonoIn.data(), mMonoOut.data(), numFrames);

    // Mix loop + (optional) live monitor, apply output gain, expand to all
    // output channels, and hard-limit as a final safety.
    const bool monitor = mMonitorEnabled.load(std::memory_order_relaxed);
    const float outGain = mOutputGain.load(std::memory_order_relaxed);
    float outPeak = 0.0f;
    for (int32_t i = 0; i < numFrames; ++i) {
        float sample = mMonoOut[static_cast<size_t>(i)];
        if (monitor) {
            sample += mMonoIn[static_cast<size_t>(i)];
        }
        sample = std::clamp(sample * outGain, -1.0f, 1.0f);
        const float magnitude = sample < 0.0f ? -sample : sample;
        if (magnitude > outPeak) {
            outPeak = magnitude;
        }
        float* frame = output + static_cast<size_t>(i) * mOutputChannels;
        for (int32_t c = 0; c < mOutputChannels; ++c) {
            frame[c] = sample;
        }
    }
    if (outPeak > mOutputPeak.load(std::memory_order_relaxed)) {
        mOutputPeak.store(outPeak, std::memory_order_relaxed);
    }
}
