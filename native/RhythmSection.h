#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

// Per-block snapshot of the rhythm controls, taken once at the start of each
// audio callback so all frames of a block agree on tempo and enables.
struct RhythmParams {
    int32_t framesPerBeat = 0;
    int32_t framesPerBar = 0;
    int32_t beatsPerBar = 4;
    bool metronome = false;
    bool drums = false;
};

/**
 * Metronome click + drum machine, rendered straight in the audio callback.
 *
 * All sounds are synthesized into buffers in prepare() (app thread); the
 * audio thread only resets play positions and mixes samples — no allocation,
 * no locks. The bar phase (where we are within the current bar) is owned by
 * the caller (LooperEngine), which derives it from the loop position while a
 * loop plays so drums and loop can never drift apart.
 *
 * Time signatures: 4/4, 3/4 and 6/8, each with one built-in drum groove.
 * BPM always means clicks per minute (quarter notes in 4/4 and 3/4, eighth
 * notes in 6/8).
 */
class RhythmSection {
public:
    static constexpr int32_t kNumTimeSignatures = 3;  // 0=4/4, 1=3/4, 2=6/8
    static constexpr int32_t kMinBpm = 40;
    static constexpr int32_t kMaxBpm = 240;

    // App thread, streams stopped. Builds the click/drum sample banks
    // (real drum samples if staged, synthesized fallback otherwise).
    void prepare(int32_t sampleRate);

    // App thread, before the streams start (serialize with prepare()).
    // Stages real drum one-shots at sourceRate; prepare() converts them.
    // (The metronome click stays synthesized — deliberate, see .cpp.)
    void setDrumSamples(std::vector<float> kick, std::vector<float> snare,
                        std::vector<float> hat, int32_t sourceRate);

    // Controls — any thread.
    void setBpm(int32_t bpm);
    void setMetronomeEnabled(bool enabled) { mMetronome.store(enabled, std::memory_order_relaxed); }
    void setDrumsEnabled(bool enabled) { mDrums.store(enabled, std::memory_order_relaxed); }
    void setTimeSignature(int32_t index) { mTimeSigIndex.store(index, std::memory_order_relaxed); }
    void setVolume(float volume) { mVolume.store(volume, std::memory_order_relaxed); }

    // ---- Audio thread only ----

    // Snapshot the controls for this block.
    RhythmParams beginBlock();

    // phaseFrames is the position within the current bar [0, framesPerBar).
    // Starts click/drum voices when the phase enters a new sequencer step.
    void triggerStep(int32_t phaseFrames, int32_t framesPerBar, bool clicks, bool drums);

    // Call while no bar clock is running so a stale step index cannot
    // suppress the first trigger when the clock starts again.
    void resetStepTracking() { mLastStep = -1; }

    // Next mono sample of all sounding voices (already volume-scaled).
    float render();

private:
    struct TimeSignature {
        int32_t beatsPerBar;
        int32_t stepsPerBar;   // sequencer resolution (16ths)
        int32_t stepsPerBeat;
        uint16_t kick;         // bit i = step i (drum groove)
        uint16_t snare;
        uint16_t hat;
        uint16_t hatAccent;    // hat steps played full strength (others soft)
        uint16_t midAccent;    // beats with a SECONDARY metronome accent
                               // (beat 1 always gets the primary accent)
    };
    static const TimeSignature kTimeSignatures[kNumTimeSignatures];

    // Sample banks, filled in prepare().
    std::vector<float> mClickAccent;
    std::vector<float> mClickMid;
    std::vector<float> mClickNormal;
    std::vector<float> mKick;
    std::vector<float> mSnare;
    std::vector<float> mHat;

    // Optional real drum samples staged by setDrumSamples(); prepare()
    // resamples them to the engine rate. Empty = synthesized fallback.
    std::vector<float> mSrcKick;
    std::vector<float> mSrcSnare;
    std::vector<float> mSrcHat;
    int32_t mSrcRate = 0;

    // Voice play positions; -1 = idle. Audio thread only.
    const std::vector<float>* mClickBuf = nullptr;
    int32_t mClickPos = -1;
    int32_t mKickPos = -1;
    int32_t mSnarePos = -1;
    int32_t mHatPos = -1;
    float mHatGain = 1.0f;  // set per trigger: accented vs soft hat
    int32_t mLastStep = -1;
    const TimeSignature* mSig = nullptr;  // cached in beginBlock()
    float mVolumeBlock = 1.0f;

    int32_t mSampleRate = 48000;

    std::atomic<int32_t> mBpm{120};
    std::atomic<bool> mMetronome{false};
    std::atomic<bool> mDrums{false};
    std::atomic<int32_t> mTimeSigIndex{0};
    std::atomic<float> mVolume{1.0f};
};
