// vulkan_hw_device_ctx — Phase F.2.12.a.
//
// Public helper that wraps qcv::VulkanDeviceManager as an
// AVVulkanDeviceContext, so FFmpeg decodes (Vulkan hwaccel) land on
// the same VkDevice the D3D11VulkanDecodeBridge consumes from. Lifted
// out of video_decoder.cpp's anonymous namespace so dual-flow's
// DualVideoDecoder can call the exact same helper — no copy-paste,
// no parallel implementation.
//
// Windows-only: this header gates everything on Q_OS_WIN because the
// Vulkan-decode handoff is a Windows-specific tier (macOS uses
// VideoToolbox; Linux Vulkan-decode is behind QCV_BUILD_LINUX_VULKAN
// and currently inactive). FFmpeg + Vulkan headers stay private to
// the cpp; this header forward-declares AVBufferRef only.

#pragma once

#include <QtGlobal>

#if defined(Q_OS_WIN)

extern "C" {
struct AVBufferRef;
struct AVCodecContext;
}

namespace qcv {

// Phase I.E (2026-09-08) — app-owned, cached Vulkan FRAME POOLS.
//
// FFmpeg's ff_get_format() unrefs avctx->hw_frames_ctx on every call,
// and a decoder may re-enter get_format mid-stream (seen on ProRes
// under frame threading). With FFmpeg-managed pools that tears the
// old pool down while the D3D11 bridge / compositor may still be
// sampling its images — the pool churn that surfaced as
// VK_ERROR_DEVICE_LOST (nvlddmkm 153 page faults) on the mixed-res
// ProRes playlist with FFmpeg 9.0. Same rule as the 2.2.8 output
// park-and-reuse, one layer earlier: nothing GPU-visible is destroyed
// mid-session.
//
// Call from the get_format callback AFTER choosing AV_PIX_FMT_VULKAN:
// builds the frames context exactly as FFmpeg would
// (avcodec_get_hw_frames_parameters → the hwaccel's frame_params fills
// usage / per-plane VkFormats / dims), then either returns a new ref to
// a cached, already-initialised pool with identical parameters or
// initialises this one and caches it. Assign the result to
// avctx->hw_frames_ctx. Returns nullptr when the codec context's
// device is not Vulkan or on failure — the caller then leaves
// hw_frames_ctx null and FFmpeg allocates as before.
AVBufferRef *acquireSharedVulkanFramesCtx(AVCodecContext *avctx);

// Drops the cache's refs. Pools whose frames are still referenced by
// a decoder / published FrameHandle survive until those drop. Called
// on device loss (VideoDecoder::releaseCachedHwDevice) and from
// VulkanDeviceManager::shutdown() BEFORE the VkDevice is destroyed.
void releaseSharedVulkanFramesCache();

// Allocates an AVBufferRef wrapping an AVVulkanDeviceContext that
// points at the shared VkDevice / VkInstance / physical device owned
// by VulkanDeviceManager. Returns nullptr on failure (caller is
// expected to fall back to software decode).
//
// Lifecycle: caller owns the returned buffer ref and must release
// with `av_buffer_unref` (or hand it to FFmpeg via
// `m_cctx->hw_device_ctx = av_buffer_ref(ref)`).
//
// Requires VulkanDeviceManager::instance().isInitialized() at call
// time — main.cpp's startup init must have run.
AVBufferRef *createSharedVulkanHwDeviceCtx();

} // namespace qcv

#endif // Q_OS_WIN
