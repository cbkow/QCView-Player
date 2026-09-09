// d3d11va_hw_device_ctx — Phase K.1 (2026-09-08): zero-copy D3D11VA.
//
// Until now every D3D11VA-decoded frame (H.264 / HEVC / AV1 / VP9 on
// Windows) was decoded on a D3D11 device FFmpeg created for itself,
// read back to system memory (av_hwframe_transfer_data), converted by
// swscale and re-uploaded — the readback was the throughput ceiling
// (bench: 4K 4444 ProRes 610 → 88 fps through the same kind of copy).
//
// This helper wraps the RENDERER's ID3D11Device as an
// AVD3D11VADeviceContext, exactly the way vulkan_hw_device_ctx wraps
// the shared VkDevice, so the decoder's texture-array slices are
// already on the device the compositor samples from. Rules carried
// over from Phase I.E: the app owns the frame pool (handed to FFmpeg
// in get_format, cached per size/format), and nothing GPU-visible is
// torn down mid-session; D3D11's refcounting plus the immediate
// context's ordering make the lifetime side far simpler than Vulkan.
//
// The render library registers the device (setSharedD3D11Device) once
// its D3D11DeviceManager is up — qcv_decode cannot link qcv_render, so
// the pointer comes in through this hook. FFmpeg decodes on its own
// threads against the same immediate context; setSharedD3D11Device
// turns on ID3D10Multithread protection, which FFmpeg's D3D11VA
// documentation requires for a user-supplied device.
//
// Windows-only.

#pragma once

#include <QtGlobal>

#if defined(Q_OS_WIN)

extern "C" {
struct AVBufferRef;
struct AVCodecContext;
struct AVFrame;
}

namespace qcv {

// Called by the D3D11 renderer after D3D11DeviceManager::initialize().
// AddRef's both; enables multithread protection on the context.
void setSharedD3D11Device(void *id3d11Device, void *id3d11ImmediateContext);

// Drops the frame-pool cache and the device refs. Call BEFORE the
// renderer's device goes away.
void clearSharedD3D11Device();

bool hasSharedD3D11Device();

// True when `hwDeviceCtx` (an AVHWDeviceContext ref) is a D3D11VA
// context wrapping the shared device — i.e. a decoder attached via
// createSharedD3D11VaHwDeviceCtx(), whose frames are zero-copy
// consumable by the renderer.
bool isSharedD3D11VaDeviceCtx(const AVBufferRef *hwDeviceCtx);

// AVHWDeviceContext(D3D11VA) over the shared device. nullptr when no
// device is registered (dev builds / before the renderer is up) —
// callers then fall back to av_hwdevice_ctx_create (FFmpeg's own
// device + readback), exactly the pre-K.1 behaviour.
AVBufferRef *createSharedD3D11VaHwDeviceCtx();

// From the get_format callback after choosing AV_PIX_FMT_D3D11 on a
// shared-device context: hand FFmpeg a ref to an app-owned, cached
// texture-array pool built through avcodec_get_hw_frames_parameters
// (so it carries the hwaccel's surface count and DECODER bind flag)
// plus D3D11_BIND_SHADER_RESOURCE so the renderer can sample the
// slices. Keyed on (device, w, h, sw_format). nullptr → FFmpeg
// allocates as before.
AVBufferRef *acquireSharedD3D11VaFramesCtx(AVCodecContext *avctx);

void releaseSharedD3D11VaFramesCache();

// True when a decoded AV_PIX_FMT_D3D11 frame's pool is one the
// renderer can consume directly: NV12 or P010 sw_format.
bool d3d11FrameIsZeroCopyConsumable(const AVFrame *frame);

} // namespace qcv

#endif // Q_OS_WIN
