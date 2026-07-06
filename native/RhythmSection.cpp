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

// Step bit i (1 << i) = 16th note i of the bar. One groove per signature.
// Hats outside hatAccent play soft, which is what keeps the pulse from
// sounding mechanical.
const RhythmSection::TimeSignature
        RhythmSection::kTimeSignatures[RhythmSection::kNumTimeSignatures] = {
    // 4/4: kick 1 & 3, snare 2 & 4, hats in 8ths accented on the quarters
    {4, 16, 4, /*kick*/ 0x0101, /*snare*/ 0x1010,
     /*hat*/ 0x5555, /*hatAccent*/ 0x1111, /*mid*/ 0},
    // 3/4 waltz (boom-chick-chick): kick on 1, snare on 2 & 3, hats only
    // on the beats — 8th hats in between blurred the feel.
    {3, 12, 4, /*kick*/ 0x001, /*snare*/ 0x110,
     /*hat*/ 0x111, /*hatAccent*/ 0x001, /*mid*/ 0},
    // 6/8: kick on 1, snare on 4, hats on every 8th accented on 1 and 4
    // (the two pulses of compound meter — an extra kick before the snare
    // was tried and rejected). Metronome: primary accent 1, secondary 4.
    {6, 12, 2, /*kick*/ 0x001, /*snare*/ 0x040,
     /*hat*/ 0x555, /*hatAccent*/ 0x041, /*mid*/ 0x040},
};

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
    mHatGain = 1.0f;
    mLastStep = -1;
    mSig = &kTimeSignatures[0];
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
    mSig = &kTimeSignatures[sigIndex];
    mVolumeBlock = mVolume.load(std::memory_order_relaxed);

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
    if (framesPerBar <= 0 || mSig == nullptr) {
        return;
    }
    int32_t step = static_cast<int32_t>(
            static_cast<int64_t>(phaseFrames) * mSig->stepsPerBar / framesPerBar);
    step = std::clamp(step, 0, mSig->stepsPerBar - 1);
    if (step == mLastStep) {
        return;
    }
    mLastStep = step;

    const uint16_t bit = static_cast<uint16_t>(1u << step);
    if (clicks && step % mSig->stepsPerBeat == 0) {
        mClickBuf = (step == 0)              ? &mClickAccent
                    : (mSig->midAccent & bit) ? &mClickMid
                                              : &mClickNormal;
        mClickPos = 0;
    }
    if (drums) {
        if (mSig->kick & bit) mKickPos = 0;
        if (mSig->snare & bit) mSnarePos = 0;
        if (mSig->hat & bit) {
            mHatPos = 0;
            mHatGain = (mSig->hatAccent & bit) ? 1.0f : 0.55f;
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
        sample += mKick[mKickPos];
        if (++mKickPos >= static_cast<int32_t>(mKick.size())) mKickPos = -1;
    }
    if (mSnarePos >= 0) {
        sample += mSnare[mSnarePos];
        if (++mSnarePos >= static_cast<int32_t>(mSnare.size())) mSnarePos = -1;
    }
    if (mHatPos >= 0) {
        sample += mHat[mHatPos] * mHatGain;
        if (++mHatPos >= static_cast<int32_t>(mHat.size())) mHatPos = -1;
    }
    return sample * mVolumeBlock;
}
