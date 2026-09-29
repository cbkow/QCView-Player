// D3D11ScopeRenderer — the vectorscope's GPU side on Windows (mirror of
// MetalScopeRenderer; math in color/scope_math.h).
//
// Per drawn frame, while the scope panel is visible, encode() records on
// the immediate context (render thread):
//   1. tap — a pixel shader samples the source into a ≤ 960 px RGBA16F
//      target, applying the OCIOScope conversion for the Assumed / Input
//      tiers (OCIO's HLSL uses Texture.Sample, which compute shaders
//      can't, hence a PS pass);
//   2. accumulate (cs_5_0) — scope-space Cb/Cr per tap pixel,
//      InterlockedAdd into one of kScopeCopies raw count grids, plus an
//      out-of-gamut count;
//   3. draw (cs_5_0) — counts → premultiplied RGBA8 (512²) into a raw
//      buffer, copied into one of three staging buffers.
// Staging buffers are mapped with DO_NOT_WAIT on later frames — the render
// thread never waits for the GPU — and the newest finished image is kept
// for latestImage() (any thread). Buffers are freed by releaseIfIdle().

#pragma once

#include "color/scope_math.h"

#include <QImage>

#include <cstdint>
#include <memory>

namespace qcv {

class OCIOConfigManager;

class D3D11ScopeRenderer {
public:
    D3D11ScopeRenderer();
    ~D3D11ScopeRenderer();

    D3D11ScopeRenderer(const D3D11ScopeRenderer &)            = delete;
    D3D11ScopeRenderer &operator=(const D3D11ScopeRenderer &) = delete;

    bool initialize();
    void shutdown();

    void setConfig(const ScopeConfig &config);   // render thread

    // `ctx` = ID3D11DeviceContext*, sources = ID3D11ShaderResourceView*.
    // Leaves no render target, shader resource or UAV bound.
    bool encode(void *ctx, OCIOConfigManager *ocio,
                void *srvA, int wA, int hA,
                void *srvB = nullptr, int wB = 0, int hB = 0);

    // Picks up finished GPU results without encoding (render thread).
    // encode() collects too, but a paused frame encodes once — its
    // result would otherwise wait for the next draw.
    void collectPending(void *ctx);

    bool latestImage(QImage *out, quint64 *serial) const;
    void releaseIfIdle(int idleMs = 10000);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace qcv
