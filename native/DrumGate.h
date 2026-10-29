#pragma once

#include <cstdint>
#include <vector>

/**
 * Gates the click and drums out of a take recorded through a room mic.
 *
 * Looping through the built-in mic and speakers, the take also holds what
 * NicheLooper played. That signal is known (`ref`), but subtracting it
 * sample-exactly fails in practice: the mic and the speakers are separate
 * devices whose clocks drift apart by a few samples over a take, and the
 * speaker is not linear. So this works on magnitudes, like a noise gate that
 * knows when and in which frequencies each drum hit lands:
 *
 *  1. The speaker → mic delay is found by matching drum onsets (1 ms grid),
 *     every 2 s, with a line through them for the clock drift.
 *  2. Per frequency band, how loud the drums arrive at the mic is measured
 *     on the frames where the drums dominate (a low quantile, so the guitar
 *     on top does not count).
 *  3. Frame by frame (~10 ms), each band is attenuated by how much of it is
 *     drums (spectral subtraction with a floor), with a short room tail.
 *
 * Plain C++ with its own FFT, no platform API: the same on macOS, Linux and
 * Android. Not real-time; run it from an app thread after the take (well
 * under a second for a typical loop on a laptop).
 *
 * `mic` has `length` samples; `ref` (what went to the speakers) has
 * `pre + length`, so mic[n] lines up with ref[pre + n]. Delays up to `pre`
 * are found.
 */
namespace drum_gate {

// The take with the drums gated out (`length` samples), or empty when the
// speakers played nothing.
std::vector<float> apply(const float* mic, int32_t length, const float* ref, int32_t pre,
                         int32_t sampleRate);

}  // namespace drum_gate
