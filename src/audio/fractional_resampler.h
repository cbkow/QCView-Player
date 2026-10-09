// FractionalResampler — RT-safe fractional-rate resampler for
// interleaved stereo float32.
//
// Two consumers:
//   - AudioPlayer / DualAudioMixer render callbacks (sync servo):
//     ratios within 1 ± 0.002 — trims audio consumption rate so the
//     playout position converges on the master clock without seeks.
//   - ShuttleAudioEngine grains (varispeed): ratios 0.05 .. 1 — the
//     tape-style pitch-follows-speed sound for sub-1x scrubs (pitch
//     is capped at natural above 1x; see kPitchCap there).
//
// Cubic Catmull-Rom interpolation over a 4-frame window. A 5-frame
// history (two past, the current base, two look-ahead) plus a
// fractional phase carry across process() calls so back-to-back
// calls are sample-continuous at any ratio. No allocation, no locks,
// no Qt/FFmpeg — safe to call from the audio device's render callback.
//
// Contract per call:
//   1. srcNeeded = sourceFramesNeeded(dstFrames, ratio)
//   2. obtain exactly srcNeeded frames of source (pad with silence on
//      underrun — the padded region plays as silence, which is the
//      same audible result the ring buffer's own underrun path gives)
//   3. consumed = process(src, srcNeeded, dst, dstFrames, ratio)
//      — always writes exactly dstFrames output frames and consumes
//      EXACTLY srcNeeded source frames (returned for convenience).
// reset() on any discontinuity (seek, grain snap, mode change).
//
// History note: the original 3-frame-history version returned a
// srcNeeded one larger than its stream advance (the interpolation
// look-ahead was read, used, then not kept), so every block lost one
// source sample at the drain — a ~100 Hz crackle under all servo
// playback. The look-ahead now lives in the history, so the stream
// advance equals srcNeeded exactly.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstring>

namespace qcv {

class FractionalResampler
{
public:
    FractionalResampler() { reset(); }

    void reset()
    {
        std::memset(m_hist, 0, sizeof(m_hist));
        m_frac = 0.0;
    }

    // Source frames required to produce dstFrames at `ratio` (source
    // frames per destination frame), given the current phase. Equal
    // to the stream advance process() will perform.
    std::size_t sourceFramesNeeded(std::size_t dstFrames, double ratio) const
    {
        if (dstFrames == 0) return 0;
        const double r = clampRatio(ratio);
        const auto newBase = static_cast<long>(
            std::floor(kBase + m_frac + static_cast<double>(dstFrames) * r));
        return static_cast<std::size_t>((newBase > kBase) ? (newBase - kBase)
                                                          : 0);
    }

    // Produce exactly dstFrames interleaved-stereo frames from src
    // (which must hold >= sourceFramesNeeded(dstFrames, ratio) frames).
    // Returns source frames consumed (== sourceFramesNeeded).
    std::size_t process(const float *src, std::size_t srcFrames,
                        float *dst, std::size_t dstFrames, double ratio)
    {
        if (dstFrames == 0) return 0;
        const double r = clampRatio(ratio);

        for (std::size_t k = 0; k < dstFrames; ++k) {
            const double pos = kBase + m_frac + static_cast<double>(k) * r;
            const long i = static_cast<long>(std::floor(pos));
            const float f = static_cast<float>(pos - static_cast<double>(i));
            for (int ch = 0; ch < 2; ++ch) {
                const float p0 = sampleAt(src, srcFrames, i - 1, ch);
                const float p1 = sampleAt(src, srcFrames, i,     ch);
                const float p2 = sampleAt(src, srcFrames, i + 1, ch);
                const float p3 = sampleAt(src, srcFrames, i + 2, ch);
                dst[k * 2 + ch] = catmullRom(p0, p1, p2, p3, f);
            }
        }

        const double newPos = kBase + m_frac
                              + static_cast<double>(dstFrames) * r;
        const long newBase = static_cast<long>(std::floor(newPos));
        m_frac = newPos - static_cast<double>(newBase);
        // Re-seat the history around the new base: C[newBase-2 ..
        // newBase+2]. The two look-ahead frames stay with us, so the
        // caller's stream advance is exactly newBase - kBase.
        for (int j = 0; j < kHist; ++j) {
            const long idx = newBase - 2 + j;
            m_hist2[j * 2 + 0] = sampleAt(src, srcFrames, idx, 0);
            m_hist2[j * 2 + 1] = sampleAt(src, srcFrames, idx, 1);
        }
        std::memcpy(m_hist, m_hist2, sizeof(m_hist));
        return static_cast<std::size_t>((newBase > kBase) ? (newBase - kBase)
                                                          : 0);
    }

private:
    static double clampRatio(double r)
    {
        if (r < 0.05) return 0.05;
        if (r > 64.0) return 64.0;
        return r;
    }

    // C[i] accessor: C[0..4] = history frames, C[5..] = src frames.
    float sampleAt(const float *src, std::size_t srcFrames,
                   long i, int ch) const
    {
        if (i < 0) i = 0;
        if (i < kHist) return m_hist[i * 2 + ch];
        const long s = i - kHist;
        if (s >= static_cast<long>(srcFrames)) {
            return srcFrames
                ? src[(srcFrames - 1) * 2 + ch]
                : m_hist[(kHist - 1) * 2 + ch];
        }
        return src[s * 2 + ch];
    }

    static float catmullRom(float p0, float p1, float p2, float p3, float f)
    {
        const float f2 = f * f;
        const float f3 = f2 * f;
        return 0.5f * ((2.0f * p1)
                       + (-p0 + p2) * f
                       + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * f2
                       + (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * f3);
    }

    static constexpr int  kHist = 5;      // 2 past, base, 2 look-ahead
    static constexpr long kBase = 2;      // index of the base frame in C

    float  m_hist[kHist * 2];   // interleaved stereo history frames
    float  m_hist2[kHist * 2];  // scratch for re-seating (no aliasing)
    double m_frac = 0.0;        // fractional phase past the base frame
};

} // namespace qcv
