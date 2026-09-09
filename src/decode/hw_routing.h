#pragma once
// Per-codec hardware-decode routing rules shared by every get_format
// callback (VideoDecoder, ScrubDecoder, LiveStreamDecoder, the dual
// pair). Windows routes in vulkan_hw_device_ctx.h (vulkanPreferredCodec
// / attachedHwPixelFormat); this header carries the rules that hold on
// every platform.
//
// ProRes RAW (2026-09-09): FFmpeg 9.0 registers `prores_raw_videotoolbox`
// and `prores_raw_vulkan` hwaccels. Neither is usable yet — VideoToolbox
// returns a null image buffer for every frame (-12905,
// kVTVideoDecoderUnsupportedDataFormatErr) on an M5 Max / macOS 26.6.2
// with an iPhone 17 Pro RAW HQ clip (Bayer pattern 3), from the ffmpeg
// CLI as well as from the app, and the Vulkan hwaccel's output is
// flipped and range-shifted upstream. Windows already forces software
// for it (VideoDecoder::initFFmpeg skipAllHw); this makes every
// platform's get_format agree, so the decoder never even opens a
// VideoToolbox session for RAW. Software decode + swscale debayer runs
// at ~30 ms/frame for 4K with slice threads (decode/thread_policy.h).

#include <QtGlobal>

extern "C" {
#include <libavcodec/codec_id.h>
}

namespace qcv {

// True when no hwaccel may be selected for this codec — get_format must
// fall straight through to the first software format.
inline bool softwareOnlyCodec(AVCodecID id)
{
    return id == AV_CODEC_ID_PRORES_RAW;
}

} // namespace qcv
