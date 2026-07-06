#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

#include "RhythmSection.h"

// Mirrored in Kotlin: audio/LooperState.kt
enum class LooperState : int32_t {
    Empty = 0,
    Recording = 1,
    Playing = 2,
    Overdubbing = 3,
    Stopped = 4,
    CountIn = 5,
};

// Mirrored in Kotlin: audio/LooperState.kt (LooperCommand).
// Load is internal-only (issued by loadLoop(), not by the UI).
enum class LooperCommand : int32_t {
    None = 0,
    Record = 1,
    Play = 2,
    Overdub = 3,
    Stop = 4,
    Clear = 5,
    Load = 6,
};

/**
 * Mono loop buffer with a Record/Play/Overdub state machine, plus an
 * integrated rhythm section (metronome click + drum machine).
 *
 * Rhythm/loop sync: the bar phase is derived from the loop position while a
 * loop plays, so clicks and drums re-align with the loop start on every
 * repetition and can never drift. While metronome/drums/count-in are active
 * during recording, the loop length is quantized to whole 4/4 bars when the
 * loop is closed.
 *
 * Record start is grid-synced: when a rhythm clock is already audible
 * (free-running drums/metronome, or an old loop during re-record), a Record
 * command waits in CountIn until the NEXT bar line before recording begins —
 * starting the grid at the button press instead would shift the drums by the
 * press offset and make an auto-closed loop end early relative to what the
 * player hears. With count-in enabled the wait is extended by one full bar;
 * without a running clock the count-in is two full bars of clicks.
 *
 * Whenever a recording is closed ON the bar line (auto-loop, or a close
 * command that arrived before the line), the player is typically still
 * sounding — ring-out, a slightly late last hit, the phrase carrying into
 * the repeat. Input keeps being folded onto the loop start for half a bar
 * after the close so that material survives the seam, mirroring the
 * overhang fold of a close command that arrives after the line.
 *
 * Threading model:
 *  - prepare() is called from an app thread while the streams are stopped.
 *  - process() is called ONLY from the real-time audio callback. It never
 *    allocates, locks or logs.
 *  - sendCommand() may be called from any thread; commands are handed to the
 *    audio thread through a single atomic slot (last write wins) and applied
 *    at the next block boundary.
 */
class LooperEngine {
public:
    void prepare(int32_t sampleRate, int32_t maxLoopSeconds);

    void sendCommand(LooperCommand command);

    // Audio thread only. input/output are mono, numFrames samples each.
    // output receives the loop playback plus click/drums; monitoring is
    // mixed in by the caller so it works in every state.
    void process(const float* input, float* output, int32_t numFrames);

    // App thread: copy the current loop into dest (up to maxSamples mono
    // samples). Returns the number of samples copied. No reallocation
    // happens outside prepare(), so the read itself is safe while the
    // engine runs — but a copy taken during recording/overdubbing is a
    // best-effort snapshot: the audio thread may concurrently write single
    // floats (which do not tear on ARM/x86), so a few samples around the
    // write position may already contain the new take. The caller must
    // serialize against prepare().
    int32_t copyLoop(float* dest, int32_t maxSamples) const;

    // App thread: replace the loop with the given mono samples and start
    // playback. Parks the state machine in Empty first so the audio thread
    // does not touch the buffer during the copy; requires a running stream
    // (commands are only consumed by the audio callback). The caller must
    // serialize against prepare().
    bool loadLoop(const float* data, int32_t numSamples);

    RhythmSection& rhythm() { return mRhythm; }

    void setCountInEnabled(bool enabled) {
        mCountInEnabled.store(enabled, std::memory_order_relaxed);
    }

    // Playback volume of the loop track only (recording stays full-scale so
    // the loop content itself is not attenuated).
    void setLoopGain(float gain) { mLoopGain.store(gain, std::memory_order_relaxed); }

    // > 0: recording auto-closes after that many bars; 0 = off.
    void setAutoLoopBars(int32_t bars) {
        mAutoLoopBars.store(bars, std::memory_order_relaxed);
    }

    LooperState state() const {
        return static_cast<LooperState>(mState.load(std::memory_order_relaxed));
    }
    int32_t positionFrames() const { return mSharedPosition.load(std::memory_order_relaxed); }
    int32_t loopLengthFrames() const { return mSharedLoopLength.load(std::memory_order_relaxed); }
    int32_t maxLoopFrames() const { return mMaxFrames; }

private:
    void applyCommand(LooperCommand command, const RhythmParams& rp);
    void startRecording(const RhythmParams& rp);
    void armAutoClose(const RhythmParams& rp);
    void finalizeLoop();
    void finalizeLoopQuantized(const RhythmParams& rp);
    void setState(LooperState state) {
        mState.store(static_cast<int32_t>(state), std::memory_order_relaxed);
    }

    std::vector<float> mBuffer;      // mono samples, allocated in prepare()
    int32_t mMaxFrames = 0;
    int32_t mLoopLength = 0;         // audio thread only
    int32_t mPosition = 0;           // audio thread only
    int32_t mCountInPos = 0;         // audio thread only, frames into count-in/wait
    int32_t mCountInTarget = 0;      // audio thread only, count-in length in frames
    int32_t mCountInPhase = 0;       // audio thread only, bar phase during count-in
    bool mCountInClicks = false;     // audio thread only, count-in adds clicks
    int32_t mFreeClock = 0;          // audio thread only, bar phase w/o a loop
    int32_t mPendingClose = 0;       // audio thread only, close loop at this frame
    int32_t mFoldRemaining = 0;      // audio thread only, post-close input frames
                                     // still folded onto the loop start
    bool mSyncRecording = false;     // audio thread only, quantize this recording

    RhythmSection mRhythm;

    std::atomic<int32_t> mCommand{static_cast<int32_t>(LooperCommand::None)};
    std::atomic<int32_t> mState{static_cast<int32_t>(LooperState::Empty)};
    std::atomic<int32_t> mSharedPosition{0};
    std::atomic<int32_t> mSharedLoopLength{0};
    std::atomic<int32_t> mPendingLoadLength{0};
    std::atomic<bool> mCountInEnabled{false};
    std::atomic<float> mLoopGain{1.0f};
    std::atomic<int32_t> mAutoLoopBars{0};
};
