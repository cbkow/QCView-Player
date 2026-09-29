// DualPixbufConverterImpl — see header.
//
// Encodes the YUV→RGB compute pass on the caller-supplied command
// buffer; the compositor's renderFrame() runs the composite draw on
// the same cb afterwards. Single-cb-per-frame keeps Metal's intra-
// buffer dependency tracking active so the composite samples the
// freshly-written RGBA texture without a CPU-side waitUntilCompleted.

#include "render/metal/dual_pixbuf_converter_impl.h"

#include "render/metal/cv_pixbuf_metal_bridge.h"
#include "render/metal/metal_device_manager.h"
#include "render/metal/metal_upload_ring.h"
#include "render/metal/metal_yuv_renderer.h"
#include "decode/frame_handle.h"
#include "decode/yuv_planar.h"

extern "C" {
#include <libavutil/frame.h>
}

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>

#include <QtLogging>
#include <array>

namespace qcv {

struct DualPixbufConverterImpl::Impl {
    bool initialized = false;

    // Per-slot bridge — each owns its own CVMetalTextureCache (Apple
    // docs: cache is single-thread-owned; the render thread is the
    // single thread for both slots, so two caches is just bookkeeping
    // separation, not a threading concern).
    std::array<CvPixbufMetalBridge, 2> bridges;

    // Shared YUV→RGBA renderer. Pipelines + output pool are internal.
    MetalYuvRenderer yuv;

    // Per-slot strong refs to the last-rendered RGBA texture. The
    // YUV renderer hands back +1-retained id<MTLTexture> as void*;
    // we absorb via __bridge_transfer so ARC releases on overwrite.
    id<MTLTexture> lastOutput[2] = { nil, nil };

    // Clean YUV: per-slot plane uploads (the ring keeps an upload off
    // textures an in-flight frame still reads).
    MetalUploadRing planeRingA{"dual A planes"};
    MetalUploadRing planeRingB{"dual B planes"};
};

DualPixbufConverterImpl::DualPixbufConverterImpl()
    : m_impl(new Impl())
{
}

DualPixbufConverterImpl::~DualPixbufConverterImpl()
{
    shutdown();
    delete m_impl;
}

bool DualPixbufConverterImpl::initialize()
{
    if (m_impl->initialized) return true;
    if (!m_impl->bridges[0].initialize()
        || !m_impl->bridges[1].initialize()) {
        qWarning("DualPixbufConverterImpl: bridge init failed");
        return false;
    }
    if (!m_impl->yuv.initialize()) {
        qWarning("DualPixbufConverterImpl: yuv renderer init failed");
        return false;
    }
    m_impl->initialized = true;
    return true;
}

void DualPixbufConverterImpl::shutdown()
{
    if (!m_impl) return;
    m_impl->lastOutput[0] = nil;
    m_impl->lastOutput[1] = nil;
    m_impl->yuv.shutdown();
    m_impl->bridges[0].shutdown();
    m_impl->bridges[1].shutdown();
    m_impl->initialized = false;
}

bool DualPixbufConverterImpl::isInitialized() const
{
    return m_impl && m_impl->initialized;
}

void *DualPixbufConverterImpl::convertToRgba(void *cmdBuffer,
                                                void *cvPixelBuffer,
                                                int slot,
                                                int *outW, int *outH,
                                                int rangeOverride)
{
    if (!isInitialized() || !cmdBuffer || !cvPixelBuffer) return nullptr;
    if (slot < 0 || slot > 1) return nullptr;

    // Build a transient FrameHandle::metal() wrapper so we can reuse
    // the existing CvPixbufMetalBridge API without duplicating its
    // CVMetalTextureCache plumbing. FrameHandle::metal takes
    // ownership, so we CFRetain first to balance.
    CFRetain(static_cast<CVPixelBufferRef>(cvPixelBuffer));
    auto pix = static_cast<CVPixelBufferRef>(cvPixelBuffer);
    FrameHandle handle = FrameHandle::metal(
        cvPixelBuffer,
        static_cast<int>(CVPixelBufferGetWidth(pix)),
        static_cast<int>(CVPixelBufferGetHeight(pix)),
        /*pts=*/0);

    CvPixbufMetalBridge::PlaneSet planes;
    const auto rc = m_impl->bridges[slot].bridge(handle, &planes);
    if (rc != CvPixbufMetalBridge::Result::Ok || !planes.valid()) {
        return nullptr;
    }

    // Per-clip range override stomps the CV-derived fullRange flag,
    // exactly like single-flow's metal_player_renderer.mm:1224-1226.
    bool fullRange = planes.fullRange;
    if (rangeOverride == 1) fullRange = true;
    else if (rangeOverride == 2) fullRange = false;

    void *output = nullptr;
    if (planes.isPacked()) {
        output = m_impl->yuv.renderInterleavedToRgba(
            cmdBuffer,
            planes.packedTexture,
            planes.width, planes.height,
            planes.bitDepth, fullRange,
            planes.isBt2020, planes.isHdr);
    } else {
        output = m_impl->yuv.renderToRgba(
            cmdBuffer,
            planes.yTexture, planes.uvTexture,
            planes.width, planes.height,
            planes.bitDepth, fullRange,
            planes.isBt2020, planes.isHdr,
            planes.subsampling);
    }
    if (!output) return nullptr;

    // Absorb the +1 retain into ARC. Overwriting the prior slot
    // entry releases the previous frame's RGBA texture.
    m_impl->lastOutput[slot] = (__bridge_transfer id<MTLTexture>)output;

    if (outW) *outW = planes.width;
    if (outH) *outH = planes.height;

    // Hand back as void* — the compositor binds via __bridge cast.
    // Caller does NOT take ownership; we hold the retain across the
    // single-frame window via lastOutput[slot].
    return (__bridge void *)m_impl->lastOutput[slot];
}

void *DualPixbufConverterImpl::convertPlanarToRgba(void *cmdBuffer, const void *avFrame,
                                                   int slot, int *outW, int *outH,
                                                   int rangeOverride)
{
    if (!isInitialized() || !cmdBuffer || !avFrame || slot < 0 || slot > 1) return nullptr;
    const auto *f = static_cast<const AVFrame *>(avFrame);
    const YuvPlanarDesc d = yuvPlanarDesc(f, rangeOverride);
    if (!d.ok) return nullptr;

    id<MTLDevice> device = (__bridge id<MTLDevice>)MetalDeviceManager::instance().device();
    if (!device) return nullptr;
    MetalUploadRing &ring = slot == 0 ? m_impl->planeRingA : m_impl->planeRingB;
    ring.beginFrame((__bridge id<MTLCommandBuffer>)cmdBuffer);
    MetalUploadRing::PlaneUpload up[4];
    for (int i = 0; i < d.planeCount; ++i) {
        const auto &pl = d.planes[i];
        if (f->linesize[pl.dataIndex] <= 0) return nullptr;
        up[i].bytes       = f->data[pl.dataIndex];
        up[i].bytesPerRow = f->linesize[pl.dataIndex];
        up[i].width       = pl.width;
        up[i].height      = pl.height;
        up[i].fmt = pl.channels == 2
            ? (d.bytesPerSample == 2 ? MTLPixelFormatRG16Unorm : MTLPixelFormatRG8Unorm)
            : (d.bytesPerSample == 2 ? MTLPixelFormatR16Unorm : MTLPixelFormatR8Unorm);
    }
    const __strong id<MTLTexture> *tex = ring.uploadPlanes(device, up, d.planeCount);
    if (!tex) return nullptr;
    void *planes[4] = {nullptr, nullptr, nullptr, nullptr};
    for (int i = 0; i < d.planeCount; ++i) planes[i] = (__bridge void *)tex[i];
    void *output = m_impl->yuv.renderPlanarToRgba(cmdBuffer, planes, d);
    if (!output) return nullptr;
    m_impl->lastOutput[slot] = (__bridge_transfer id<MTLTexture>)output;
    if (outW) *outW = d.width;
    if (outH) *outH = d.height;
    return (__bridge void *)m_impl->lastOutput[slot];
}

} // namespace qcv
