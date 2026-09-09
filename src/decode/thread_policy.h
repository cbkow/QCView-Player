#pragma once

// Software-decode threading policy shared by every decoder that opens
// an AVCodecContext (VideoDecoder, ScrubDecoder, the dual pair).
//
// Measured 2026-09-09 (vendored FFmpeg 9.0.1 CLI, 32-thread box, 24
// frames, milliseconds; "frame" = FF_THREAD_FRAME|SLICE which FFmpeg
// resolves to frame threading whenever the codec supports it):
//
//   ProRes RAW 4224x3024   frame 4815  slice 323   first frame 1823 → 118
//   DNxHR 444 12-bit 1080p frame  171  slice 118
//   ProRes 422 HQ 1080p    frame  104  slice  75
//   ProRes 4444 720p       frame  102  slice  76
//   APV 4:2:2 10-bit 1080p frame   94  slice  72
//   FFV1 1080p (1 slice)   frame  225  slice 186
//
// Frame threading pipelines N frames before the first one comes out
// (a full second of black on 4K RAW) and, for these intra codecs, is
// slower in steady state too — every worker carries a private codec
// context and the per-frame work is already slice-parallel. Inter
// codecs (H.264 / HEVC / AV1 / VP9 / VVC in software) keep frame
// threading: their slices are few and the inter-frame dependency is
// what frame threads pipeline.

#include <QtGlobal>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_desc.h>
#include <libavutil/cpu.h>
}

namespace qcv {

// `userThreads` — 0 = FFmpeg auto (core count), else the user's
// performance/ffmpegThreads value. Call before avcodec_open2.
inline void applySoftwareThreadPolicy(AVCodecContext *ctx, const AVCodec *codec,
                                      int userThreads)
{
    if (!ctx || !codec) return;
    const AVCodecDescriptor *desc = avcodec_descriptor_get(codec->id);
    const bool intraOnly = desc && (desc->props & AV_CODEC_PROP_INTRA_ONLY);
    const bool sliceCap  = (codec->capabilities & AV_CODEC_CAP_SLICE_THREADS) != 0;
    ctx->thread_count = userThreads > 0 ? userThreads : 0;
    ctx->thread_type  = (intraOnly && sliceCap)
                        ? FF_THREAD_SLICE
                        : (FF_THREAD_FRAME | FF_THREAD_SLICE);
}

// Dual view runs two software decoders at once; give each half the
// machine instead of letting both ask for every core (2026-09-09: the
// 8K ProRes XQ clip decodes in the same 33 ms/frame on 16 slice threads
// as on 32, so the split costs nothing and removes the contention).
inline int dualSideThreadCount()
{
    const int n = av_cpu_count();
    return n > 1 ? n / 2 : 1;
}

inline const char *threadPolicyName(const AVCodecContext *ctx)
{
    if (!ctx) return "?";
    return (ctx->thread_type == FF_THREAD_SLICE) ? "slice" : "frame+slice";
}

} // namespace qcv
