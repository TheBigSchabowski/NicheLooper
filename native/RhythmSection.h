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
 * Two independent selections:
 *   - the time signature sets the bar length and the metronome accents,
 *   - the groove sets the drum pattern *within* that bar.
 * Every groove declares which signature it belongs to; beginBlock() falls
 * back to the first groove of the selected signature if the pair does not
 * match, so a stale index can never desync drums from the bar clock.
 *
 * BPM always means clicks per minute (quarter notes in 4/4, 3/4 and 2/4,
 * eighth notes in 6/8).
 */
class RhythmSection {
public:
    static constexpr int32_t kNumTimeSignatures = 4;  // 0=4/4 1=3/4 2=2/4 3=6/8
    static constexpr int32_t kNumGrooves = 17;
    static constexpr int32_t kMinBpm = 40;
    static constexpr int32_t kMaxBpm = 240;

    // ---- Static tables, for the UI (any thread; pure reads of constants) ----
    static const char* timeSignatureName(int32_t index);
    static int32_t beatsPerBar(int32_t timeSignatureIndex);
    static const char* grooveName(int32_t index);
    // Which time signature groove [index] is written for.
    static int32_t grooveTimeSignature(int32_t index);

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

    // Silences the drums without stopping the bar clock — unlike
    // setDrumsEnabled(false), which lets the clock go away entirely. Voices
    // already sounding ring out; only new hits are suppressed, so unmuting
    // drops straight back into the groove at whatever step is current.
    // The metronome is unaffected.
    void setDrumsMuted(bool muted) { mDrumsMuted.store(muted, std::memory_order_relaxed); }

    void setTimeSignature(int32_t index) { mTimeSigIndex.store(index, std::memory_order_relaxed); }
    void setGroove(int32_t index) { mGrooveIndex.store(index, std::memory_order_relaxed); }
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
        const char* name;
        int32_t beatsPerBar;
    };

    // One drum pattern for one bar. Bit i of each mask = step i of the bar;
    // the accent masks pick the steps played at full strength, everything
    // else in the pattern plays soft (ghost notes / unaccented hats), which
    // is what keeps a one-bar loop from sounding mechanical.
    struct Groove {
        const char* name;
        int32_t timeSig;       // index into kTimeSignatures
        int32_t stepsPerBar;   // sequencer resolution of THIS groove
        int32_t stepsPerBeat;  // stepsPerBar / beatsPerBar (3 = triplet feel)
        uint16_t kick;
        uint16_t kickAccent;
        uint16_t snare;
        uint16_t snareAccent;
        uint16_t hat;
        uint16_t hatAccent;
        uint16_t midAccent;    // steps with a SECONDARY metronome accent
                               // (beat 1 always gets the primary accent)
    };
    static const TimeSignature kTimeSignatures[kNumTimeSignatures];
    static const Groove kGrooves[kNumGrooves];

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
    float mKickGain = 1.0f;   // set per trigger: accented vs soft
    float mSnareGain = 1.0f;
    float mHatGain = 1.0f;
    int32_t mLastStep = -1;
    int32_t mLastStepsPerBar = 0;  // detects a grid change mid-bar
    const TimeSignature* mSig = nullptr;   // cached in beginBlock()
    const Groove* mGroove = nullptr;
    int32_t mCachedSigIndex = -1;          // inputs the cache above was built from
    int32_t mCachedGrooveIndex = -1;
    float mVolumeBlock = 1.0f;
    bool mDrumsMutedBlock = false;

    int32_t mSampleRate = 48000;

    std::atomic<int32_t> mBpm{120};
    std::atomic<bool> mMetronome{false};
    std::atomic<bool> mDrums{false};
    std::atomic<bool> mDrumsMuted{false};
    std::atomic<int32_t> mTimeSigIndex{0};
    std::atomic<int32_t> mGrooveIndex{0};
    std::atomic<float> mVolume{1.0f};
};
