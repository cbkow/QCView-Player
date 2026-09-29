// D3D11YuvPlanarBridge — Clean YUV (colour plan, stage 1) on D3D11.
//
// Software-decoded YUV frames (FrameHandle::Kind::CpuYuv, and dual view's
// DualFramePayload::Kind::CpuYuv) arrive as an AVFrame of planes. This
// bridge uploads Y, U, V[, A] (or Y, UV[, A] for NV12-style chroma) into
// R8 / R16 / R8G8 / R16G16 UNORM textures and runs a compute pass that
// writes unclamped RGBA16F — the same output shape D3D11VaDecodeBridge
// hands the renderer, so the videoA slot code is shared (SlotOwner::
// Bridge). The maths is decode/yuv_planar.h, identical to Metal's
// yuv_planar_to_rgba: super-whites and sub-blacks reach OCIO instead of
// being clipped by swscale's UNORM output.
//
// Plane textures and the output are recreated only on a size / format
// change. UpdateSubresource renames under an in-flight read, so reusing
// them each frame is safe on the one immediate context.

#pragma once

#include <QtGlobal>

#if defined(Q_OS_WIN)

#include <memory>

#include "d3d11_vulkan_decode_bridge.h"   // ImportedFrame / ImportedPlane

extern "C" {
struct AVFrame;
}

namespace qcv {

class FrameHandle;

class D3D11YuvPlanarBridge
{
public:
    D3D11YuvPlanarBridge();
    ~D3D11YuvPlanarBridge();
    D3D11YuvPlanarBridge(const D3D11YuvPlanarBridge &) = delete;
    D3D11YuvPlanarBridge &operator=(const D3D11YuvPlanarBridge &) = delete;

    // Compiles the compute shader on the shared D3D11 device. On failure
    // the renderer leaves cpuYuvRenderingEnabled() off and decoders keep
    // the swscale path.
    bool initialize();
    void shutdown();
    bool isInitialized() const;

    // Consume a FrameHandle::CpuYuv. `rangeOverride`: 0 Auto / 1 Full /
    // 2 Limited (per-clip pill).
    const D3D11VulkanDecodeBridge::ImportedFrame *
    consume(const FrameHandle &fh, int rangeOverride = 0);

    // Same, from a raw software AVFrame (dual view). The caller keeps the
    // frame alive until this returns.
    const D3D11VulkanDecodeBridge::ImportedFrame *
    consumeAVFrame(const AVFrame *avFrame, int rangeOverride = 0);

    // Frees the plane and output textures (dual source change / exit).
    void releaseTextures();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace qcv

#endif // Q_OS_WIN
