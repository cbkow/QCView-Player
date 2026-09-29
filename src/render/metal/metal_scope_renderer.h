// MetalScopeRenderer — the vectorscope's GPU side (color/scope_math.h).
//
// Per frame, while the scope panel is visible, encode() records into the
// frame's existing command buffer (no CPU wait, never blocks the render
// thread):
//   1. clear the count grids (blit fill);
//   2. scope_accum — one thread per tap pixel (source sampled at ≤ 960 px
//      wide): optional OCIOScope conversion, scope-space Cb/Cr, one
//      atomic add into one of kScopeCopies grid copies (picked by
//      threadgroup, to cut contention on flat frames), plus an
//      out-of-gamut count; for the waveform, the same kernel again over
//      every source pixel (peak pass: atomic max of level and channel);
//   3. scope_draw — sums the copies, maps counts to intensity, tints
//      (mono / colourised / dual cyan-orange / out-of-gamut), and writes
//      512² premultiplied RGBA8 straight into one of three shared
//      readback buffers (the peaks are copied in after the image).
// A completion handler publishes the finished buffer; latestImage()
// copies the newest one out on any thread. The scope is allowed to lag.
//
// Resources (~13 MB) are allocated on first use and freed by releaseIfIdle()
// after the scope has been inactive for a while.

#pragma once

#include "color/scope_math.h"

#include <QImage>

#include <cstdint>
#include <memory>
#include <vector>

namespace qcv {

class OCIOConfigManager;

class MetalScopeRenderer {
public:
    MetalScopeRenderer();
    ~MetalScopeRenderer();

    MetalScopeRenderer(const MetalScopeRenderer &)            = delete;
    MetalScopeRenderer &operator=(const MetalScopeRenderer &) = delete;

    bool initialize();
    void shutdown();

    void setConfig(const ScopeConfig &config);   // render thread
    const ScopeConfig &config() const;

    // Record one scope update into `cmdBuf` (id<MTLCommandBuffer>) for
    // source A (and B when config().dual). Textures are id<MTLTexture>
    // (RGBA16F / RGBA8 / RGBA16, any size). Returns false when skipped.
    bool encode(void *cmdBuf, OCIOConfigManager *ocio,
                void *srcA, int wA, int hA,
                void *srcB = nullptr, int wB = 0, int hB = 0);

    // Newest finished scope image (512² RGBA8, premultiplied). `serial`
    // bumps with each new image. Any thread.
    // The newest image; for the waveform also its peaks (`peaks`).
    bool latestImage(QImage *out, quint64 *serial, ScopePeaks *peaks = nullptr) const;
    quint64 serial() const;

    // Free the buffers if encode() hasn't run for `idleMs`. Render thread.
    void releaseIfIdle(int idleMs = 10000);

    // Probe support: the side-A count grid (copies summed) from the most
    // recent encode, after the caller waited for the command buffer.
    bool readCounts(std::vector<uint32_t> &counts, std::vector<uint32_t> &oog) const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace qcv
