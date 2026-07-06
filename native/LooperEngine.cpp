#include "LooperEngine.h"

#include <algorithm>
#include <chrono>
#include <thread>

void LooperEngine::prepare(int32_t sampleRate, int32_t maxLoopSeconds) {
    mMaxFrames = sampleRate * maxLoopSeconds;
    mBuffer.assign(static_cast<size_t>(mMaxFrames), 0.0f);
    mLoopLength = 0;
    mPosition = 0;
    mCountInPos = 0;
    mCountInTarget = 0;
    mCountInPhase = 0;
    mCountInClicks = false;
    mFreeClock = 0;
    mPendingClose = 0;
    mFoldRemaining = 0;
    mSyncRecording = false;
    mRhythm.prepare(sampleRate);
    mSharedPosition.store(0, std::memory_order_relaxed);
    mSharedLoopLength.store(0, std::memory_order_relaxed);
    mState.store(static_cast<int32_t>(LooperState::Empty), std::memory_order_relaxed);
    mCommand.store(static_cast<int32_t>(LooperCommand::None), std::memory_order_relaxed);
}

int32_t LooperEngine::copyLoop(float* dest, int32_t maxSamples) const {
    const int32_t n = std::min(loopLengthFrames(), maxSamples);
    if (n <= 0) {
        return 0;
    }
    std::copy_n(mBuffer.data(), n, dest);
    return n;
}

void LooperEngine::sendCommand(LooperCommand command) {
    mCommand.store(static_cast<int32_t>(command), std::memory_order_release);
}

bool LooperEngine::loadLoop(const float* data, int32_t numSamples) {
    if (numSamples <= 0 || numSamples > mMaxFrames) {
        return false;
    }
    // Park the state machine: in Empty the audio thread neither reads nor
    // writes the buffer. The Clear command is consumed at the next block
    // boundary (blocks are a few ms); poll up to ~200 ms instead of trusting
    // a single fixed sleep so a scheduling hiccup cannot fail the load.
    sendCommand(LooperCommand::Clear);
    for (int attempt = 0; attempt < 40 && state() != LooperState::Empty; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (state() != LooperState::Empty) {
        // Stream not processing (or a user command raced us) — abort rather
        // than write a buffer the audio thread might be using.
        return false;
    }
    std::copy_n(data, numSamples, mBuffer.begin());
    mPendingLoadLength.store(numSamples, std::memory_order_release);
    sendCommand(LooperCommand::Load);
    return true;
}

void LooperEngine::finalizeLoop() {
    mLoopLength = mPosition;
    mSharedLoopLength.store(mLoopLength, std::memory_order_relaxed);
    mPosition = 0;
    mPendingClose = 0;
    mFoldRemaining = 0;
    // A zero-length loop (double-tap within one block) degenerates to Empty.
    setState(mLoopLength > 0 ? LooperState::Playing : LooperState::Empty);
}

void LooperEngine::finalizeLoopQuantized(const RhythmParams& rp) {
    if (!mSyncRecording || rp.framesPerBar <= 0) {
        finalizeLoop();
        return;
    }
    const int32_t bar = rp.framesPerBar;
    int64_t bars = (static_cast<int64_t>(mPosition) + bar / 2) / bar;  // nearest
    if (bars < 1) {
        bars = 1;
    }
    const int64_t target = bars * bar;
    if (target > mMaxFrames) {
        // Recording nearly filled the whole buffer; bar alignment is moot.
        finalizeLoop();
        return;
    }
    const int32_t length = static_cast<int32_t>(target);
    if (mPosition >= length) {
        // Pressed just after the bar line: fold the overhang onto the loop
        // start (the player kept playing over the repeat) and continue
        // playback in place. Overhang < half a bar by construction.
        const int32_t overhang = mPosition - length;
        for (int32_t k = 0; k < overhang; ++k) {
            mBuffer[k] = std::clamp(mBuffer[k] + mBuffer[length + k], -1.0f, 1.0f);
        }
        mLoopLength = length;
        mSharedLoopLength.store(length, std::memory_order_relaxed);
        mPosition = overhang;
        mPendingClose = 0;
        setState(LooperState::Playing);
    } else {
        // Pressed just before the bar line: keep recording until it.
        mPendingClose = length;
    }
}

void LooperEngine::startRecording(const RhythmParams& rp) {
    // Bar phase of whatever rhythm clock is currently audible; -1 = none.
    // Recording must start on THAT clock's bar line, not at the button
    // press: the press lands somewhere mid-bar, and restarting the grid
    // there shifts the drums by the press offset and makes an auto-closed
    // loop end early relative to what the player hears.
    int32_t gridPhase = -1;
    if ((rp.metronome || rp.drums) && rp.framesPerBar > 0) {
        const LooperState s = state();
        if (s == LooperState::Empty || s == LooperState::Stopped) {
            gridPhase = mFreeClock % rp.framesPerBar;
        } else if ((s == LooperState::Playing || s == LooperState::Overdubbing) &&
                   mLoopLength > 0) {
            gridPhase = mPosition % rp.framesPerBar;  // re-record: keep the groove
        }
    }

    // Replaces any existing loop. No buffer wipe needed — recording
    // overwrites and playback never reads past mLoopLength.
    mLoopLength = 0;
    mSharedLoopLength.store(0, std::memory_order_relaxed);
    mPosition = 0;
    mPendingClose = 0;

    const bool countIn =
            mCountInEnabled.load(std::memory_order_relaxed) && rp.framesPerBar > 0;
    int32_t wait = 0;
    if (gridPhase >= 0) {
        // Sync to the running clock: finish the current bar, plus one full
        // count-in bar if enabled (so the lead-in is between 1 and 2 bars).
        wait = (rp.framesPerBar - gridPhase) % rp.framesPerBar;
        if (countIn) {
            wait += rp.framesPerBar;
        }
    } else if (countIn) {
        wait = 2 * rp.framesPerBar;  // no clock running: two full bars
    }

    if (wait > 0) {
        mCountInPos = 0;
        mCountInTarget = wait;
        mCountInPhase = std::max(gridPhase, 0);
        mCountInClicks = countIn;  // a pure bar-line wait adds no clicks
        setState(LooperState::CountIn);
    } else {
        mSyncRecording = rp.metronome || rp.drums;
        armAutoClose(rp);
        setState(LooperState::Recording);
    }
}

void LooperEngine::armAutoClose(const RhythmParams& rp) {
    const int32_t bars = mAutoLoopBars.load(std::memory_order_relaxed);
    if (bars <= 0 || rp.framesPerBar <= 0) {
        return;
    }
    const int64_t target = static_cast<int64_t>(bars) * rp.framesPerBar;
    if (target <= mMaxFrames) {
        mPendingClose = static_cast<int32_t>(target);
        mSyncRecording = true;
    }
}

void LooperEngine::applyCommand(LooperCommand command, const RhythmParams& rp) {
    // Any explicit user action ends a post-close fold: Overdub would
    // otherwise write the same input twice, and Stop/Record/Play/Clear/Load
    // all move the play head away from the fold region.
    mFoldRemaining = 0;
    const LooperState s = state();
    switch (command) {
        case LooperCommand::Record:
            if (s == LooperState::Recording) {
                finalizeLoopQuantized(rp);
            } else if (s != LooperState::CountIn) {
                startRecording(rp);
            }
            break;
        case LooperCommand::Play:
            if (s == LooperState::Recording) {
                finalizeLoopQuantized(rp);
            } else if (s == LooperState::Overdubbing) {
                setState(LooperState::Playing);
            } else if (s == LooperState::Stopped && mLoopLength > 0) {
                mPosition = 0;
                setState(LooperState::Playing);
            }
            break;
        case LooperCommand::Overdub:
            if (s == LooperState::Overdubbing) {
                setState(LooperState::Playing);
            } else if (s == LooperState::Playing) {
                setState(LooperState::Overdubbing);
            } else if (s == LooperState::Stopped && mLoopLength > 0) {
                mPosition = 0;
                setState(LooperState::Overdubbing);
            }
            break;
        case LooperCommand::Stop:
            if (s == LooperState::CountIn) {
                // Cancel the count-in. The previous loop was already
                // discarded when the count-in started (same as re-record).
                // The free clock takes over at the count-in's bar phase so
                // running drums keep their place.
                setState(LooperState::Empty);
                mFreeClock = mCountInPhase;
                break;
            }
            if (s == LooperState::Recording) {
                finalizeLoop();  // keep what was recorded, exactly as played
                if (state() == LooperState::Playing) {
                    mPosition = 0;
                    setState(LooperState::Stopped);
                }
            } else if (s == LooperState::Playing || s == LooperState::Overdubbing) {
                mPosition = 0;
                setState(LooperState::Stopped);
            }
            mFreeClock = 0;  // metronome/drums restart on beat 1
            break;
        case LooperCommand::Clear:
            mLoopLength = 0;
            mSharedLoopLength.store(0, std::memory_order_relaxed);
            mPosition = 0;
            mPendingClose = 0;
            mFreeClock = 0;
            setState(LooperState::Empty);
            break;
        case LooperCommand::Load:
            // Buffer contents were written by the app thread while the
            // machine was parked in Empty (see loadLoop()).
            mLoopLength = mPendingLoadLength.load(std::memory_order_acquire);
            mSharedLoopLength.store(mLoopLength, std::memory_order_relaxed);
            mPosition = 0;
            setState(mLoopLength > 0 ? LooperState::Playing : LooperState::Empty);
            break;
        case LooperCommand::None:
            break;
    }
}

void LooperEngine::process(const float* input, float* output, int32_t numFrames) {
    const RhythmParams rp = mRhythm.beginBlock();

    const auto pending = static_cast<LooperCommand>(
            mCommand.exchange(static_cast<int32_t>(LooperCommand::None),
                              std::memory_order_acquire));
    if (pending != LooperCommand::None) {
        applyCommand(pending, rp);
    }

    const bool clockWanted = rp.metronome || rp.drums;
    const float loopGain = mLoopGain.load(std::memory_order_relaxed);
    LooperState s = state();
    for (int32_t i = 0; i < numFrames; ++i) {
        // Bar phase for this frame; -1 = no rhythm clock running.
        int32_t phase = -1;
        bool clicks = rp.metronome;
        switch (s) {
            case LooperState::Recording:
                mBuffer[mPosition] = input[i];
                output[i] = 0.0f;
                // Recording always starts on beat 1, so the record head IS
                // the bar phase.
                if ((mSyncRecording || clockWanted) && rp.framesPerBar > 0) {
                    phase = mPosition % rp.framesPerBar;
                }
                ++mPosition;
                if (mPendingClose > 0 && mPosition >= mPendingClose) {
                    // Quantized close: the bar line has been reached. The
                    // player is still sounding, so keep folding input onto
                    // the loop start for half a bar (see class comment).
                    mLoopLength = mPendingClose;
                    mSharedLoopLength.store(mLoopLength, std::memory_order_relaxed);
                    mPosition = 0;
                    mPendingClose = 0;
                    mFoldRemaining = std::min(rp.framesPerBar / 2, mLoopLength);
                    setState(LooperState::Playing);
                    s = LooperState::Playing;
                } else if (mPosition >= mMaxFrames) {
                    // Buffer full: close the loop automatically.
                    finalizeLoop();
                    s = state();
                }
                break;
            case LooperState::Playing:
                output[i] = mBuffer[mPosition] * loopGain;
                if (mFoldRemaining > 0) {
                    // Post-close fold: overdub-style write of the input that
                    // is still sounding after a bar-line close. Runs from the
                    // loop start (the close set mPosition = 0).
                    mBuffer[mPosition] =
                            std::clamp(mBuffer[mPosition] + input[i], -1.0f, 1.0f);
                    --mFoldRemaining;
                }
                if (clockWanted && rp.framesPerBar > 0) {
                    phase = mPosition % rp.framesPerBar;
                }
                if (++mPosition >= mLoopLength) {
                    mPosition = 0;
                }
                break;
            case LooperState::Overdubbing: {
                const float existing = mBuffer[mPosition];
                output[i] = existing * loopGain;
                // Hard limit so repeated overdubs cannot blow up the buffer.
                mBuffer[mPosition] = std::clamp(existing + input[i], -1.0f, 1.0f);
                if (clockWanted && rp.framesPerBar > 0) {
                    phase = mPosition % rp.framesPerBar;
                }
                if (++mPosition >= mLoopLength) {
                    mPosition = 0;
                }
                break;
            }
            case LooperState::CountIn:
                output[i] = 0.0f;
                // The count-in / bar-line wait runs on its own bar phase so
                // already-running drums and clicks continue in time (the
                // phase was seeded from the free clock / old loop).
                phase = mCountInPhase;
                clicks = rp.metronome || mCountInClicks;
                if (++mCountInPhase >= rp.framesPerBar) {
                    mCountInPhase = 0;
                }
                if (++mCountInPos >= mCountInTarget) {
                    // Wait over: recording begins on the next frame, exactly
                    // on beat 1 of the (continuing) grid.
                    mPosition = 0;
                    mSyncRecording = true;
                    mPendingClose = 0;
                    armAutoClose(rp);
                    setState(LooperState::Recording);
                    s = LooperState::Recording;
                }
                break;
            case LooperState::Empty:
            case LooperState::Stopped:
                output[i] = 0.0f;
                if (clockWanted && rp.framesPerBar > 0) {
                    // Free-running clock so you can practice to the drums
                    // without a loop. Restarts at beat 1 when re-enabled.
                    phase = mFreeClock;
                    if (++mFreeClock >= rp.framesPerBar) {
                        mFreeClock = 0;
                    }
                } else {
                    mFreeClock = 0;
                }
                break;
        }

        if (phase >= 0) {
            mRhythm.triggerStep(phase, rp.framesPerBar, clicks, rp.drums);
        } else {
            mRhythm.resetStepTracking();
        }
        output[i] += mRhythm.render();
    }

    // During count-in the UI shows the beat within the bar, so share the
    // bar phase rather than the raw wait counter.
    mSharedPosition.store(s == LooperState::CountIn ? mCountInPhase : mPosition,
                          std::memory_order_relaxed);
}
