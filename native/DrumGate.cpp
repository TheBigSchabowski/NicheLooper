#include "DrumGate.h"

#include <algorithm>
#include <cmath>
#include <complex>

namespace drum_gate {
namespace {

using Complex = std::complex<float>;

constexpr double kPi = 3.14159265358979323846;  // M_PI is not standard C++ (MSVC lacks it)
constexpr int32_t kFft = 2048;           // ~43 ms frames at 48 kHz
constexpr int32_t kHop = kFft / 4;       // ~11 ms
constexpr int32_t kBins = kFft / 2 + 1;
constexpr float kOverSubtract = 1.5f;    // remove a bit more than measured
constexpr float kFloor = 0.1f;           // never below −20 dB: keeps the guitar natural
constexpr float kTailSeconds = 0.15f;    // drums ring on in the room this long (−60 dB)
constexpr float kQuantile = 0.25f;       // drum level per band: low quantile of mic/ref

// Level ratios mic/ref are histogrammed in log2 steps of 1/4 over [-20, 4).
constexpr int32_t kRatioSteps = 96;
constexpr float kRatioMin = -20.0f;
constexpr float kRatioStep = 0.25f;

// In-place radix-2 FFT; inverse is unscaled.
void fft(std::vector<Complex>& a, bool inverse) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double angle = (inverse ? 2.0 : -2.0) * kPi / static_cast<double>(len);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; ++k) {
                const Complex wf(static_cast<float>(w.real()), static_cast<float>(w.imag()));
                const Complex u = a[i + k];
                const Complex v = a[i + k + len / 2] * wf;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

// Positive changes of a 1-block RMS envelope: where hits start.
std::vector<float> onsets(const float* x, int32_t n, int32_t block) {
    const int32_t blocks = n / block;
    std::vector<float> out(std::max(blocks, 0), 0.0f);
    float previous = 0.0f;
    for (int32_t b = 0; b < blocks; ++b) {
        double sum = 0.0;
        for (int32_t i = 0; i < block; ++i) {
            const double v = x[b * block + i];
            sum += v * v;
        }
        const auto level = static_cast<float>(std::sqrt(sum / block));
        out[b] = std::max(0.0f, level - previous);
        previous = level;
    }
    return out;
}

// Speaker → mic delay over the take: delay(n) = start + slope * n samples.
// Mic and speakers are separate devices whose clocks differ by some ppm, so
// the delay drifts by several ms over a long loop.
struct Delay {
    double start = 0.0;
    double slope = 0.0;
    int32_t at(int64_t n) const { return static_cast<int32_t>(std::lround(start + slope * n)); }
};

// Onset match of mic blocks [from, to) against ref, lags [lo, hi] (blocks).
int32_t bestLag(const std::vector<float>& micOn, const std::vector<float>& refOn, int32_t preBlocks,
                size_t from, size_t to, int32_t lo, int32_t hi, double* scoreOut) {
    int32_t best = lo;
    double bestScore = -1.0;
    for (int32_t lag = lo; lag <= hi; ++lag) {
        double score = 0.0;
        for (size_t k = from; k < to; ++k) {
            const int64_t r = static_cast<int64_t>(k) + preBlocks - lag;
            if (r >= 0 && r < static_cast<int64_t>(refOn.size())) score += micOn[k] * refOn[r];
        }
        if (score > bestScore) {
            bestScore = score;
            best = lag;
        }
    }
    *scoreOut = bestScore;
    return best;
}

Delay findDelay(const float* mic, int32_t length, const float* ref, int32_t pre,
                int32_t sampleRate) {
    const int32_t block = std::max(1, sampleRate / 1000);  // 1 ms grid
    const std::vector<float> micOn = onsets(mic, length, block);
    const std::vector<float> refOn = onsets(ref, pre + length, block);
    const int32_t preBlocks = pre / block;

    // Overall delay from the first minute, then a local one every 2 s
    // (within ±40 ms of it) and a straight line through those.
    double score = 0.0;
    const size_t head = std::min(micOn.size(), static_cast<size_t>(60000));
    const int32_t overall = bestLag(micOn, refOn, preBlocks, 0, head, 0, preBlocks, &score);
    const size_t segment = 2000;
    std::vector<double> times;
    std::vector<double> lags;
    for (size_t from = 0; from + segment / 2 <= micOn.size(); from += segment) {
        const size_t to = std::min(micOn.size(), from + segment);
        const int32_t lag = bestLag(micOn, refOn, preBlocks, from, to,
                                    std::max(0, overall - 40), std::min(preBlocks, overall + 40), &score);
        if (score > 0.0) {
            times.push_back(0.5 * static_cast<double>(from + to) * block);
            lags.push_back(static_cast<double>(lag) * block);
        }
    }
    // Theil–Sen line (median of pairwise slopes): a segment matched to the
    // wrong hit cannot tilt it.
    Delay delay{static_cast<double>(overall) * block, 0.0};
    if (times.size() >= 3) {
        std::vector<double> slopes;
        for (size_t i = 0; i < times.size(); ++i) {
            for (size_t j = i + 1; j < times.size(); ++j) {
                slopes.push_back((lags[j] - lags[i]) / (times[j] - times[i]));
            }
        }
        std::nth_element(slopes.begin(), slopes.begin() + slopes.size() / 2, slopes.end());
        const double slope = slopes[slopes.size() / 2];
        if (std::fabs(slope) < 2e-4) {  // ±200 ppm; more is a misfit
            std::vector<double> starts;
            for (size_t i = 0; i < times.size(); ++i) starts.push_back(lags[i] - slope * times[i]);
            std::nth_element(starts.begin(), starts.begin() + starts.size() / 2, starts.end());
            delay.start = starts[starts.size() / 2];
            delay.slope = slope;
        }
    }
    return delay;
}

// Short-time spectra of the take and of ref shifted by the delay. Frame t
// covers padded samples [t * kHop, t * kHop + kFft); the take starts at
// padded sample kFft, so its edges get full overlap-add coverage.
class Frames {
public:
    Frames(const float* mic, int32_t length, const float* ref, int32_t pre, Delay delay)
        : mMic(mic), mLength(length), mRef(ref), mPre(pre), mDelay(delay),
          mWindow(kFft) {
        for (int32_t i = 0; i < kFft; ++i) {
            mWindow[i] = 0.5f - 0.5f * static_cast<float>(std::cos(2.0 * kPi * i / kFft));
        }
    }

    int32_t count() const { return (mLength + kFft) / kHop + 1; }
    const std::vector<float>& window() const { return mWindow; }

    void mic(int32_t t, std::vector<Complex>& out) { load(t, out, true); }
    void ref(int32_t t, std::vector<Complex>& out) { load(t, out, false); }

private:
    void load(int32_t t, std::vector<Complex>& out, bool fromMic) {
        out.assign(kFft, Complex(0.0f, 0.0f));
        const int32_t delay = mDelay.at(static_cast<int64_t>(t) * kHop - kFft / 2);  // at the frame centre
        for (int32_t i = 0; i < kFft; ++i) {
            const int64_t n = static_cast<int64_t>(t) * kHop + i - kFft;  // take sample
            float v = 0.0f;
            if (fromMic) {
                if (n >= 0 && n < mLength) v = mMic[n];
            } else {
                const int64_t r = mPre + n - delay;  // what the mic heard at n
                if (n < mLength && r >= 0 && r < mPre + mLength) v = mRef[r];
            }
            out[i] = Complex(v * mWindow[i], 0.0f);
        }
        fft(out, false);
    }

    const float* mMic;
    int32_t mLength;
    const float* mRef;
    int32_t mPre;
    Delay mDelay;
    std::vector<float> mWindow;
};

}  // namespace

std::vector<float> apply(const float* mic, int32_t length, const float* ref, int32_t pre,
                         int32_t sampleRate) {
    if (length <= 0 || pre < 0 || sampleRate <= 0) return {};
    const Delay delay = findDelay(mic, length, ref, pre, sampleRate);
    Frames frames(mic, length, ref, pre, delay);
    const int32_t count = frames.count();
    std::vector<Complex> y;
    std::vector<Complex> r;

    // Pass 1: average drum level per band, to pick the frames where drums dominate.
    std::vector<double> refMean(kBins, 0.0);
    for (int32_t t = 0; t < count; ++t) {
        frames.ref(t, r);
        for (int32_t f = 0; f < kBins; ++f) refMean[f] += std::abs(r[f]);
    }
    double total = 0.0;
    for (double& m : refMean) {
        m /= count;
        total += m;
    }
    if (!(total > 1e-6)) return {};  // the speakers played nothing

    // Pass 2: how loud the drums reach the mic, per band. Where a hit is
    // strong, mic/ref is the speaker → mic gain plus whatever the guitar
    // adds on top; a low quantile keeps just the gain.
    std::vector<int32_t> histogram(static_cast<size_t>(kBins) * kRatioSteps, 0);
    std::vector<int32_t> hits(kBins, 0);
    for (int32_t t = 0; t < count; ++t) {
        frames.mic(t, y);
        frames.ref(t, r);
        for (int32_t f = 0; f < kBins; ++f) {
            const float rm = std::abs(r[f]);
            if (rm <= refMean[f] || rm < 1e-9f) continue;
            const float ratio = std::log2(std::max(std::abs(y[f]), 1e-12f) / rm);
            const auto step = static_cast<int32_t>((ratio - kRatioMin) / kRatioStep);
            ++histogram[static_cast<size_t>(f) * kRatioSteps + std::clamp(step, 0, kRatioSteps - 1)];
            ++hits[f];
        }
    }
    std::vector<float> gainLog(kBins, kRatioMin);  // log2 of the drum gain per band
    std::vector<bool> measured(kBins, false);
    for (int32_t f = 0; f < kBins; ++f) {
        if (hits[f] < 8) continue;
        const auto target = static_cast<int32_t>(kQuantile * hits[f]);
        int32_t seen = 0;
        for (int32_t s = 0; s < kRatioSteps; ++s) {
            seen += histogram[static_cast<size_t>(f) * kRatioSteps + s];
            if (seen > target) {
                gainLog[f] = kRatioMin + (s + 0.5f) * kRatioStep;
                measured[f] = true;
                break;
            }
        }
    }
    // Smooth across neighbouring bands; bands without hits carry no drums.
    std::vector<float> gain(kBins, 0.0f);
    for (int32_t f = 0; f < kBins; ++f) {
        if (!measured[f]) continue;
        float sum = 0.0f;
        int32_t n = 0;
        for (int32_t g = std::max(0, f - 3); g <= std::min(kBins - 1, f + 3); ++g) {
            if (measured[g]) {
                sum += gainLog[g];
                ++n;
            }
        }
        gain[f] = std::exp2(sum / n);
    }

    // Pass 3: gate each band by its drum share and resynthesize.
    const float decay = std::pow(10.0f, -3.0f * kHop / (kTailSeconds * sampleRate));
    std::vector<float> bleed(kBins, 0.0f);
    std::vector<float> mask(kBins);
    std::vector<float> out(static_cast<size_t>(count) * kHop + kFft, 0.0f);
    const std::vector<float>& window = frames.window();
    // Hann analysis + synthesis at 75 % overlap sums to 1.5; the inverse FFT is unscaled.
    const float norm = 1.0f / (1.5f * kFft);
    for (int32_t t = 0; t < count; ++t) {
        frames.mic(t, y);
        frames.ref(t, r);
        for (int32_t f = 0; f < kBins; ++f) {
            bleed[f] = std::max(gain[f] * std::abs(r[f]), bleed[f] * decay);
            const float power = std::norm(y[f]);
            const float drums = kOverSubtract * bleed[f];
            mask[f] = power > 0.0f ? std::max(kFloor, 1.0f - drums * drums / power) : 1.0f;
        }
        for (int32_t f = 0; f < kBins; ++f) {
            const float m = (mask[std::max(0, f - 1)] + mask[f] + mask[std::min(kBins - 1, f + 1)]) / 3.0f;
            y[f] *= m;
            if (f > 0 && f < kFft / 2) y[kFft - f] = std::conj(y[f]);
        }
        fft(y, true);
        for (int32_t i = 0; i < kFft; ++i) {
            out[static_cast<size_t>(t) * kHop + i] += y[i].real() * window[i] * norm;
        }
    }

    return std::vector<float>(out.begin() + kFft, out.begin() + kFft + length);
}

}  // namespace drum_gate
