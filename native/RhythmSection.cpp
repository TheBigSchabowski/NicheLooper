#include "RhythmSection.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kTwoPi = 6.28318530718f;

// Deterministic white noise so drum sounds are identical on every prepare().
class NoiseGen {
public:
    float next() {
        mSeed = mSeed * 1664525u + 1013904223u;
        return static_cast<int32_t>(mSeed) / 2147483648.0f;
    }

private:
    uint32_t mSeed = 0x1D872B41u;
};

void synthClick(std::vector<float>& buf, int32_t sr, float freq, float amp) {
    const int32_t n = sr * 30 / 1000;
    buf.resize(static_cast<size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / sr;
        buf[i] = amp * std::sin(kTwoPi * freq * t) * std::exp(-t / 0.005f);
    }
}

void synthKick(std::vector<float>& buf, int32_t sr) {
    const int32_t n = sr * 200 / 1000;
    buf.resize(static_cast<size_t>(n));
    float phase = 0.0f;
    for (int32_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / sr;
        // Bottoms out at 55 Hz — deep enough to read as a kick, shallow
        // enough not to make a guitar speaker flap.
        const float freq = 55.0f + 110.0f * std::exp(-t / 0.045f);
        phase += kTwoPi * freq / sr;
        buf[i] = 0.5f * std::sin(phase) * std::exp(-t / 0.11f);
    }
}

void synthSnare(std::vector<float>& buf, int32_t sr) {
    const int32_t n = sr * 160 / 1000;
    buf.resize(static_cast<size_t>(n));
    NoiseGen noise;
    for (int32_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / sr;
        const float tone = 0.22f * std::sin(kTwoPi * 185.0f * t) * std::exp(-t / 0.05f);
        const float snap = 0.32f * noise.next() * std::exp(-t / 0.055f);
        buf[i] = tone + snap;
    }
}

void synthHat(std::vector<float>& buf, int32_t sr) {
    const int32_t n = sr * 60 / 1000;
    buf.resize(static_cast<size_t>(n));
    NoiseGen noise;
    // One-pole highpass (~6 kHz) so the noise reads as metal, not static.
    const float r = std::max(0.0f, 1.0f - kTwoPi * 6000.0f / sr);
    float y = 0.0f;
    float xPrev = 0.0f;
    for (int32_t i = 0; i < n; ++i) {
        const float t = static_cast<float>(i) / sr;
        const float x = noise.next();
        y = r * (y + x - xPrev);
        xPrev = x;
        buf[i] = 0.25f * y * std::exp(-t / 0.015f);
    }
}

// Linear-resample src (fromRate) into dst (toRate) and scale to targetPeak.
void resampleAndScale(std::vector<float>& dst, const std::vector<float>& src,
                      int32_t fromRate, int32_t toRate, float targetPeak) {
    if (src.empty() || fromRate <= 0 || toRate <= 0) {
        dst.clear();
        return;
    }
    if (fromRate == toRate) {
        dst = src;
    } else {
        const auto outLen = static_cast<size_t>(
                static_cast<int64_t>(src.size()) * toRate / fromRate);
        dst.resize(outLen);
        const double step = static_cast<double>(fromRate) / toRate;
        const size_t last = src.size() - 1;
        for (size_t i = 0; i < outLen; ++i) {
            const double pos = i * step;
            const auto idx = static_cast<size_t>(pos);
            const float frac = static_cast<float>(pos - idx);
            const float a = src[std::min(idx, last)];
            const float b = src[std::min(idx + 1, last)];
            dst[i] = a + (b - a) * frac;
        }
    }
    float peak = 0.0f;
    for (const float s : dst) {
        peak = std::max(peak, std::abs(s));
    }
    if (peak > 0.0f) {
        const float scale = targetPeak / peak;
        for (float& s : dst) {
            s *= scale;
        }
    }
}

}  // namespace

const RhythmSection::TimeSignature
        RhythmSection::kTimeSignatures[RhythmSection::kNumTimeSignatures] = {
    {"4/4", 4},
    {"3/4", 3},
    {"2/4", 2},
    {"6/8", 6},  // BPM counts eighths here
};

// Grooves, grouped by time signature (the UI filters on Groove::timeSig, so
// the order within a signature is the order of the dropdown). Step bit i
// (1 << i) = step i of the bar at that groove's own resolution: stepsPerBeat
// 4 = 16ths, 3 = 8th triplets (shuffle), 2 = the eighths of compound meter.
// A step present in the pattern but missing from its accent mask plays soft.
const RhythmSection::Groove RhythmSection::kGrooves[RhythmSection::kNumGrooves] = {
    // ---- 4/4, 16 steps: beats on 0, 4, 8, 12 ----
    // Kick 1 & 3, snare 2 & 4, hats in 8ths accented on the quarters.
    {"Rock", 0, 16, 4,
     /*kick*/ 0x0101, 0x0101, /*snare*/ 0x1010, 0x1010,
     /*hat*/ 0x5555, 0x1111, /*mid*/ 0},
    // Adds the kick on the "and" of 3 — the everyday pop push.
    {"Pop", 0, 16, 4,
     /*kick*/ 0x0501, 0x0501, /*snare*/ 0x1010, 0x1010,
     /*hat*/ 0x5555, 0x1111, /*mid*/ 0},
    // Same backbeat, hats in 16ths: drives without changing the feel.
    {"Straight 16", 0, 16, 4,
     /*kick*/ 0x0101, 0x0101, /*snare*/ 0x1010, 0x1010,
     /*hat*/ 0xFFFF, 0x5555, /*mid*/ 0},
    // Syncopated kick plus ghost snares on the "e" of 2 and the "a" of 4;
    // only the downbeat hats are accented, which is what makes it funk.
    {"Funk", 0, 16, 4,
     /*kick*/ 0x0409, 0x0409, /*snare*/ 0x5090, 0x1010,
     /*hat*/ 0xFFFF, 0x1111, /*mid*/ 0},
    // Half-time: kick 1, snare 3. Room to breathe under a slow loop.
    {"Ballad", 0, 16, 4,
     /*kick*/ 0x0001, 0x0001, /*snare*/ 0x0100, 0x0100,
     /*hat*/ 0x5555, 0x1111, /*mid*/ 0},
    // Kick on every quarter, hats accented on the offbeats (disco/house).
    {"Four on the Floor", 0, 16, 4,
     /*kick*/ 0x1111, 0x1111, /*snare*/ 0x1010, 0x1010,
     /*hat*/ 0x5555, 0x4444, /*mid*/ 0},
    // Triplet grid: 12 steps = 4 beats x 3. Hats long-short per beat.
    {"Shuffle", 0, 12, 3,
     /*kick*/ 0x041, 0x041, /*snare*/ 0x208, 0x208,
     /*hat*/ 0xB6D, 0x249, /*mid*/ 0},
    // Bossa: surdo-ish kick, clave on the snare kept entirely ghosted so it
    // reads as a rim click rather than a backbeat.
    {"Bossa", 0, 16, 4,
     /*kick*/ 0x4141, 0x4141, /*snare*/ 0x1448, 0x0000,
     /*hat*/ 0x5555, 0x1111, /*mid*/ 0},

    // ---- 3/4, 12 steps: beats on 0, 4, 8 ----
    // Boom-chick-chick. Hats only on the beats — 8ths in between blurred it.
    {"Waltz", 1, 12, 4,
     /*kick*/ 0x001, 0x001, /*snare*/ 0x110, 0x110,
     /*hat*/ 0x111, 0x001, /*mid*/ 0},
    // Hats in 8ths, beat 2 ghosted so beat 3 lifts.
    {"Jazz Waltz", 1, 12, 4,
     /*kick*/ 0x001, 0x001, /*snare*/ 0x110, 0x100,
     /*hat*/ 0x555, 0x111, /*mid*/ 0},
    // Just 1 and 3 — the sparse version for slow 3/4.
    {"Waltz Ballad", 1, 12, 4,
     /*kick*/ 0x001, 0x001, /*snare*/ 0x100, 0x100,
     /*hat*/ 0x555, 0x111, /*mid*/ 0},

    // ---- 2/4, 8 steps: beats on 0, 4 ----
    {"March", 2, 8, 4,
     /*kick*/ 0x01, 0x01, /*snare*/ 0x10, 0x10,
     /*hat*/ 0x55, 0x11, /*mid*/ 0},
    // Kick on both beats, snare on the offbeats.
    {"Polka", 2, 8, 4,
     /*kick*/ 0x11, 0x11, /*snare*/ 0x44, 0x44,
     /*hat*/ 0x55, 0x11, /*mid*/ 0},
    // 16th hats: a fast 2/4 punk/train feel.
    {"2/4 Rock", 2, 8, 4,
     /*kick*/ 0x01, 0x01, /*snare*/ 0x10, 0x10,
     /*hat*/ 0xFF, 0x55, /*mid*/ 0},

    // ---- 6/8, 12 steps: the six eighths on 0, 2, 4, 6, 8, 10 ----
    // Kick on 1, snare on 4, hats accented on the two pulses of compound
    // meter (an extra kick before the snare was tried and rejected).
    // Metronome: primary accent on 1, secondary on 4.
    {"6/8 Basic", 3, 12, 2,
     /*kick*/ 0x001, 0x001, /*snare*/ 0x040, 0x040,
     /*hat*/ 0x555, 0x041, /*mid*/ 0x040},
    // All twelve subdivisions on the hat plus a pickup kick: 12/8 blues.
    {"6/8 Blues", 3, 12, 2,
     /*kick*/ 0x021, 0x021, /*snare*/ 0x040, 0x040,
     /*hat*/ 0xFFF, 0x041, /*mid*/ 0x040},
    // Kick on the two pulses, snare filling the eighths after each of them
    // with the second one ghosted — that lilt is what makes it a march.
    {"6/8 March", 3, 12, 2,
     /*kick*/ 0x041, 0x041, /*snare*/ 0x514, 0x104,
     /*hat*/ 0x555, 0x041, /*mid*/ 0x040},
};

const char* RhythmSection::timeSignatureName(int32_t index) {
    if (index < 0 || index >= kNumTimeSignatures) return "";
    return kTimeSignatures[index].name;
}

int32_t RhythmSection::beatsPerBar(int32_t timeSignatureIndex) {
    if (timeSignatureIndex < 0 || timeSignatureIndex >= kNumTimeSignatures) return 4;
    return kTimeSignatures[timeSignatureIndex].beatsPerBar;
}

const char* RhythmSection::grooveName(int32_t index) {
    if (index < 0 || index >= kNumGrooves) return "";
    return kGrooves[index].name;
}

int32_t RhythmSection::grooveTimeSignature(int32_t index) {
    if (index < 0 || index >= kNumGrooves) return 0;
    return kGrooves[index].timeSig;
}

void RhythmSection::prepare(int32_t sampleRate) {
    mSampleRate = sampleRate;
    if (mSrcRate > 0 && !mSrcKick.empty() && !mSrcSnare.empty() && !mSrcHat.empty()) {
        // Real samples: convert to the engine rate with conservative peaks
        // so kick + hat + loop + live guitar stay clear of the limiter.
        resampleAndScale(mKick, mSrcKick, mSrcRate, sampleRate, 0.40f);
        resampleAndScale(mSnare, mSrcSnare, mSrcRate, sampleRate, 0.35f);
        resampleAndScale(mHat, mSrcHat, mSrcRate, sampleRate, 0.20f);
    } else {
        synthKick(mKick, sampleRate);
        synthSnare(mSnare, sampleRate);
        synthHat(mHat, sampleRate);
    }
    // Metronome stays synthesized on purpose — real stick samples were
    // tried and rejected; the classic pitched clicks read better. What
    // matters is the accent hierarchy: primary / secondary / plain.
    synthClick(mClickAccent, sampleRate, 1568.0f, 0.55f);  // G6, primary
    synthClick(mClickMid, sampleRate, 1318.5f, 0.45f);     // E6, secondary
    synthClick(mClickNormal, sampleRate, 1046.5f, 0.35f);  // C6, plain
    mClickBuf = &mClickNormal;
    mClickPos = mKickPos = mSnarePos = mHatPos = -1;
    mKickGain = mSnareGain = mHatGain = 1.0f;
    mLastStep = -1;
    mLastStepsPerBar = 0;
    mSig = &kTimeSignatures[0];
    mGroove = &kGrooves[0];
    mCachedSigIndex = -1;
    mCachedGrooveIndex = -1;
}

void RhythmSection::setDrumSamples(std::vector<float> kick, std::vector<float> snare,
                                   std::vector<float> hat, int32_t sourceRate) {
    mSrcKick = std::move(kick);
    mSrcSnare = std::move(snare);
    mSrcHat = std::move(hat);
    mSrcRate = sourceRate;
}

void RhythmSection::setBpm(int32_t bpm) {
    mBpm.store(std::clamp(bpm, kMinBpm, kMaxBpm), std::memory_order_relaxed);
}

RhythmParams RhythmSection::beginBlock() {
    const int32_t sigIndex =
            std::clamp(mTimeSigIndex.load(std::memory_order_relaxed), 0, kNumTimeSignatures - 1);
    const int32_t grooveIndex =
            std::clamp(mGrooveIndex.load(std::memory_order_relaxed), 0, kNumGrooves - 1);
    // Resolving the pair means a linear scan, so only redo it when the user
    // actually changed something — every other block just reuses the cache.
    if (sigIndex != mCachedSigIndex || grooveIndex != mCachedGrooveIndex) {
        mCachedSigIndex = sigIndex;
        mCachedGrooveIndex = grooveIndex;
        mSig = &kTimeSignatures[sigIndex];
        mGroove = &kGrooves[grooveIndex];
        if (mGroove->timeSig != sigIndex) {
            // Mismatched pair (e.g. the signature changed first): play the
            // signature's first groove rather than a pattern whose bar is a
            // different length than the bar clock.
            for (int32_t i = 0; i < kNumGrooves; ++i) {
                if (kGrooves[i].timeSig == sigIndex) {
                    mGroove = &kGrooves[i];
                    break;
                }
            }
        }
    }
    mVolumeBlock = mVolume.load(std::memory_order_relaxed);
    mDrumsMutedBlock = mDrumsMuted.load(std::memory_order_relaxed);

    RhythmParams p;
    const int32_t bpm = mBpm.load(std::memory_order_relaxed);
    p.framesPerBeat = bpm > 0 ? mSampleRate * 60 / bpm : 0;
    p.beatsPerBar = mSig->beatsPerBar;
    p.framesPerBar = p.framesPerBeat * p.beatsPerBar;
    p.metronome = mMetronome.load(std::memory_order_relaxed);
    p.drums = mDrums.load(std::memory_order_relaxed);
    return p;
}

void RhythmSection::triggerStep(int32_t phaseFrames, int32_t framesPerBar,
                                bool clicks, bool drums) {
    if (framesPerBar <= 0 || mGroove == nullptr) {
        return;
    }
    const int32_t stepsPerBar = mGroove->stepsPerBar;
    int32_t step = static_cast<int32_t>(
            static_cast<int64_t>(phaseFrames) * stepsPerBar / framesPerBar);
    step = std::clamp(step, 0, stepsPerBar - 1);

    if (stepsPerBar != mLastStepsPerBar) {
        // Switching to a groove with a different resolution invalidates
        // mLastStep (it counts in the old grid). Adopt the current step
        // without firing it — one missed hit beats a double trigger.
        mLastStepsPerBar = stepsPerBar;
        if (mLastStep >= 0) {
            mLastStep = step;
            return;
        }
    }
    if (step == mLastStep) {
        return;
    }
    mLastStep = step;

    const uint16_t bit = static_cast<uint16_t>(1u << step);
    if (clicks && step % mGroove->stepsPerBeat == 0) {
        mClickBuf = (step == 0)                  ? &mClickAccent
                    : (mGroove->midAccent & bit) ? &mClickMid
                                                 : &mClickNormal;
        mClickPos = 0;
    }
    if (drums && !mDrumsMutedBlock) {
        if (mGroove->kick & bit) {
            mKickPos = 0;
            mKickGain = (mGroove->kickAccent & bit) ? 1.0f : 0.60f;
        }
        if (mGroove->snare & bit) {
            mSnarePos = 0;
            mSnareGain = (mGroove->snareAccent & bit) ? 1.0f : 0.32f;
        }
        if (mGroove->hat & bit) {
            mHatPos = 0;
            mHatGain = (mGroove->hatAccent & bit) ? 1.0f : 0.55f;
        }
    }
}

float RhythmSection::render() {
    float sample = 0.0f;
    if (mClickPos >= 0) {
        const std::vector<float>& buf = *mClickBuf;
        sample += buf[mClickPos];
        if (++mClickPos >= static_cast<int32_t>(buf.size())) mClickPos = -1;
    }
    if (mKickPos >= 0) {
        sample += mKick[mKickPos] * mKickGain;
        if (++mKickPos >= static_cast<int32_t>(mKick.size())) mKickPos = -1;
    }
    if (mSnarePos >= 0) {
        sample += mSnare[mSnarePos] * mSnareGain;
        if (++mSnarePos >= static_cast<int32_t>(mSnare.size())) mSnarePos = -1;
    }
    if (mHatPos >= 0) {
        sample += mHat[mHatPos] * mHatGain;
        if (++mHatPos >= static_cast<int32_t>(mHat.size())) mHatPos = -1;
    }
    return sample * mVolumeBlock;
}
