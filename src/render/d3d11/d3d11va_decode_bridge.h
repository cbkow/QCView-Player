// D3D11VaDecodeBridge — Phase K.1 (2026-09-08): zero-copy consumption
// of D3D11VA-decoded frames (FrameHandle::Kind::D3D11).
//
// The decoder (via d3d11va_hw_device_ctx) now decodes H.264 / HEVC /
// AV1 / VP9 into a texture array that lives on the renderer's own
// ID3D11Device. Each frame is one slice of that array in NV12 (8-bit)
// or P010 (10-bit). This bridge creates per-slice shader views (luma
// R8/R16, chroma R8G8/R16G16) and runs a compute pass that writes the
// YUV→RGB result into a bridge-owned RGBA16F texture — the same shape
// D3D11VulkanDecodeBridge hands the renderer, so the videoA slot code
// is shared (SlotOwner::Bridge). Colour math mirrors the Vulkan
// compositor exactly (BT.601 / 709 / 2020, limited / full, Range pill
// override).
//
// Lifetime: the FrameHandle owns an AVFrame ref, so the pool slice
// cannot be recycled while we read it; everything runs on the one
// immediate context (multithread-protected), so ordering between
// FFmpeg's decode writes and our sampling is the runtime's job. The
// output texture is recreated only on a size change; D3D11 defers the
// actual release past in-flight reads.

#pragma once

#include <QtGlobal>

#if defined(Q_OS_WIN)

#include <memory>

#include "d3d11_vulkan_decode_bridge.h"   // ImportedFrame / ImportedPlane

namespace qcv {

class FrameHandle;

class D3D11VaDecodeBridge
{
public:
    D3D11VaDecodeBridge();
    ~D3D11VaDecodeBridge();
    D3D11VaDecodeBridge(const D3D11VaDecodeBridge &) = delete;
    D3D11VaDecodeBridge &operator=(const D3D11VaDecodeBridge &) = delete;

    // Compiles the compute shader on the shared D3D11 device. Soft
    // failure: the renderer then drops D3D11 frames (decoder logs the
    // fallback path).
    bool initialize();
    void shutdown();
    bool isInitialized() const;

    // Consume a FrameHandle::D3D11: build/reuse slice views, run the
    // YUV→RGB compute, return the RGBA16F output for the videoA slot.
    // `rangeOverride`: 0 Auto / 1 Full / 2 Limited (per-clip pill).
    const D3D11VulkanDecodeBridge::ImportedFrame *
    consume(const FrameHandle &fh, int rangeOverride = 0);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace qcv

#endif // Q_OS_WIN
