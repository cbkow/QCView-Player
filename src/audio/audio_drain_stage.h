// AudioDrainStage — render-callback-side drain of one IAudioSource
// through the sync-servo resampler, with every discontinuity softened.
//
// Before this stage existed the two players (AudioPlayer, one per
// side in DualAudioMixer) drained the ring directly and every
// adjustment was a hard cut: instant silence the moment a seek was
// requested, instant resume mid-waveform when it completed, silence
// slammed in on underrun. Those cuts are the "pops" heard whenever
// the sync servo's re-seek tier fired. This stage owns the drain
// and guarantees:
//
//   - a seek fades the pre-seek tail out (the decoders no longer
//     clear the ring; they mark the boundary — see
//     AudioRingBuffer::markStale) and fades the post-seek audio in;
//   - an underrun fades the last real frames out and the next real
//     frames in;
//   - a hold (caller wants audio frozen, e.g. until the video lands
//     after a seek) fades out, idles silent without consuming, and
//     fades back in on release;
//   - a skip request (drop N ring frames to re-align without a
//     decoder seek) is serviced here, counted as consumption so the
//     position estimate stays truthful.
//
// Consumption accounting rule (unchanged from the players): only
// REAL post-seek ring frames are counted. Stale frames drained for a
// fade-out and frames played while a seek is still pending are not.
//
// Render-callback-only object; the Request struct is the one thing
// other threads touch (atomics). No allocation, no locks.

#pragma once

#include "fractional_resampler.h"
#include "i_audio_source.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>

namespace qcv {

class AudioDrainStage
{
public:
    // Cross-thread knobs (UI / pump thread writes, callback reads).
    struct Request {
        // Freeze: output silence, consume nothing, until cleared.
        std::atomic<bool>     hold{false};
        // Drop this many ring frames (output-domain) before resuming
        // normal playout. Counted as consumption.
        std::atomic<uint32_t> skipFrames{0};
        // The device was (re)started: forget playout continuity, so
        // nothing gets faded "out" that the listener never heard, and
        // any stale pre-seek audio is dropped silently.
        std::atomic<bool>     restart{true};
    };

    // Fade length for every ramp. 5 ms at 48 kHz: long enough to kill
    // the click of a step, short enough to be a non-event.
    static constexpr std::size_t kFadeFrames = 240;

    // Fill exactly `frameCount` interleaved-stereo frames into `out`
    // from `dec`, draining at `ratio` source frames per output frame.
    // `scratch` holds >= scratchFrames stereo frames. Real frames
    // consumed are added to `consumed`.
    // `extraHold` lets the owner add a hold condition of its own (the
    // players pass "not playing" so a pause fades out before the
    // device stops instead of cutting).
    void process(IAudioSource &dec, float *out, std::size_t frameCount,
                 double ratio, float *scratch, std::size_t scratchFrames,
                 std::atomic<uint64_t> &consumed, Request &req,
                 bool extraHold = false)
    {
        if (req.restart.exchange(false, std::memory_order_acq_rel)) {
            m_live     = false;
            m_genValid = false;
            m_fadeIn   = 0;
            m_resampler.reset();
        }

        // ---- Seek completed on the decode thread: new generation.
        const uint32_t gen = dec.flushGeneration();
        if (!m_genValid) {
            m_genValid = true;
            m_lastGen  = gen;
            dec.discardStale();            // from before we were listening
        } else if (gen != m_lastGen) {
            m_lastGen = gen;
            if (m_live) {
                // Fade the pre-seek tail out (uncounted), drop the
                // rest, resume post-seek audio next block.
                drain(dec, out, frameCount, ratio, scratch, scratchFrames,
                      nullptr, dec.staleFrames(), /*forceFadeOut=*/true);
                dec.discardStale();
                m_resampler.reset();
                return;
            }
            dec.discardStale();
            m_resampler.reset();
        }

        // ---- Seek requested, not yet serviced: the ring holds
        // pre-seek audio. Keep it rolling if we were live (the cut is
        // faded above when the seek lands); otherwise stay silent.
        // Never counted — the anchor was reset by the seek site.
        if (dec.seekPending()) {
            if (m_live) {
                drain(dec, out, frameCount, ratio, scratch, scratchFrames,
                      nullptr);
            } else {
                silence(out, frameCount);
            }
            return;
        }

        // ---- Hold: freeze without consuming.
        if (extraHold || req.hold.load(std::memory_order_acquire)) {
            if (m_live) {
                drain(dec, out, frameCount, ratio, scratch, scratchFrames,
                      &consumed, SIZE_MAX, /*forceFadeOut=*/true);
            } else {
                silence(out, frameCount);
            }
            return;
        }

        // ---- Skip: drop ring frames to re-align (counted).
        uint32_t skip = req.skipFrames.load(std::memory_order_acquire);
        if (skip > 0) {
            if (m_live) {
                // Soften the splice: fade the current audio out first.
                drain(dec, out, frameCount, ratio, scratch, scratchFrames,
                      &consumed, SIZE_MAX, /*forceFadeOut=*/true);
                return;
            }
            while (skip > 0) {
                const std::size_t n = std::min<std::size_t>(skip, scratchFrames);
                const std::size_t got = dec.read(scratch, n);
                consumed.fetch_add(got, std::memory_order_relaxed);
                skip -= static_cast<uint32_t>(got);
                if (got < n) break;            // ring drained; finish later
            }
            req.skipFrames.store(skip, std::memory_order_release);
            if (skip > 0) {
                silence(out, frameCount);
                return;
            }
            m_resampler.reset();
        }

        drain(dec, out, frameCount, ratio, scratch, scratchFrames, &consumed);
    }

    // True when the last block ended with real audio at unity gain
    // (callback thread only; diagnostic).
    bool live() const { return m_live; }

private:
    static void silence(float *out, std::size_t frames)
    {
        std::memset(out, 0, frames * 2 * sizeof(float));
    }

    // Drain through the resampler. `counted` null = don't account
    // (pre-seek audio). `maxSrc` caps the real source frames taken
    // (the stale tail at a seek); the remainder of the block is
    // silence. `forceFadeOut` ramps the real tail to zero — every
    // fade-out goes through here so it rides the resampler's history
    // and phase; a raw ring read would skip the frames the resampler
    // already holds as look-ahead and click.
    void drain(IAudioSource &dec, float *out, std::size_t frameCount,
               double ratio, float *scratch, std::size_t scratchFrames,
               std::atomic<uint64_t> *counted,
               std::size_t maxSrc = SIZE_MAX, bool forceFadeOut = false)
    {
        const std::size_t srcNeeded =
            m_resampler.sourceFramesNeeded(frameCount, ratio);
        if (srcNeeded > scratchFrames) {
            // Callback larger than the scratch sized at init — silence
            // beats allocating on the RT thread.
            silence(out, frameCount);
            m_live = false;
            return;
        }
        const std::size_t want = std::min(srcNeeded, maxSrc);
        const std::size_t got  = want ? dec.read(scratch, want) : 0;   // pads to want
        if (got < srcNeeded) {
            std::memset(scratch + got * 2, 0, (srcNeeded - got) * 2 * sizeof(float));
        }
        if (counted) counted->fetch_add(got, std::memory_order_relaxed);

        if (got == 0) {
            // Nothing to play (underrun / EOF tail). The previous
            // block already ended live if m_live — there is nothing
            // left to fade; this is the one cut we cannot soften.
            silence(out, frameCount);
            m_live = false;
            return;
        }
        if (!m_live) m_fadeIn = kFadeFrames;        // (re)starting audio

        const bool endsSilent = (got < srcNeeded) || forceFadeOut;
        if (endsSilent) {
            // Fade the real tail out before the silence that follows.
            const std::size_t n = std::min(got, kFadeFrames);
            for (std::size_t i = 0; i < n; ++i) {
                const float g = static_cast<float>(n - 1 - i)
                              / static_cast<float>(n);
                scratch[(got - n + i) * 2 + 0] *= g;
                scratch[(got - n + i) * 2 + 1] *= g;
            }
        }

        m_resampler.process(scratch, srcNeeded, out, frameCount, ratio);

        if (m_fadeIn > 0) {
            const std::size_t n = std::min(m_fadeIn, frameCount);
            for (std::size_t i = 0; i < n; ++i) {
                const float g = static_cast<float>(kFadeFrames - m_fadeIn + i)
                              / static_cast<float>(kFadeFrames);
                out[i * 2 + 0] *= g;
                out[i * 2 + 1] *= g;
            }
            m_fadeIn -= n;
        }
        m_live = !endsSilent;
    }

    FractionalResampler m_resampler;
    uint32_t            m_lastGen  = 0;
    bool                m_genValid = false;
    bool                m_live     = false;
    std::size_t         m_fadeIn   = 0;
};

} // namespace qcv
