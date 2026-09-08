#pragma once

// rgb_range — legal-range (16–235) → full-range expansion for RGB-family
// sources on the CPU RGBA8 paths.
//
// swscale applies no range handling to an RGB→RGB conversion (its
// srcRange only feeds YUV→RGB matrices), so every place that turns an
// RGB AVFrame (gbrp*, rgb48, …) into an RGBA8888 buffer has to do the
// expansion itself — otherwise the Range pill is inert for RGB and
// legal-range RGB masters (Avid DNxHR 444 RGB, MXF RGBA tagged 64/940)
// come up flat. One rule for all of them, identical to the GPU branches
// (Windows Vulkan compositor RGB path):
//
//   override Full    → never
//   override Limited → always
//   Auto             → when the frame is tagged limited (container tag,
//                      or our vendored dnxhddec's Avid legal-range
//                      convention stamp)
//
// Used by VideoDecoder::publishCpuFrame (playback), swsFrameToRgbaImage
// (single + dual scrub decoders), DualVideoDecoder's CPU path and the
// thumbnail loader — so scrub, play, dual and thumbs all agree.

#include <algorithm>
#include <array>
#include <cstdint>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
}

namespace qcv {

inline bool isRgbPixelFormat(int avPixFmt)
{
    const AVPixFmtDescriptor *desc =
        av_pix_fmt_desc_get(static_cast<AVPixelFormat>(avPixFmt));
    return desc && (desc->flags & AV_PIX_FMT_FLAG_RGB);
}

// rangeOverride: 0 = Auto, 1 = Full, 2 = Limited (MediaItem pill).
inline bool rgbFrameNeedsLegalExpansion(const AVFrame *frame, int rangeOverride)
{
    if (!frame || !isRgbPixelFormat(frame->format)) return false;
    if (rangeOverride == 1) return false;
    if (rangeOverride == 2) return true;
    return frame->color_range == AVCOL_RANGE_MPEG;
}

// Phase J.1 (2026-09-08) — CPU publish depth. Sources deeper than 8
// bits (10/12-bit YUV, 16-bit RGB, 16-bit Bayer from ProRes RAW) are
// converted to RGBA64 (16-bit unsigned) and uploaded as UNORM16, so
// OCIO sees the full source precision instead of an 8-bit truncation.
// Integer, not half: video is 0..1 with no super-whites and UNORM16
// keeps every bit; half float has 11 significant bits and would lose
// precision near white. RGBA16F stays with EXR.
inline bool sourceNeeds16BitPublish(int avPixFmt)
{
    const AVPixFmtDescriptor *desc =
        av_pix_fmt_desc_get(static_cast<AVPixelFormat>(avPixFmt));
    if (!desc) return false;
    if (desc->flags & AV_PIX_FMT_FLAG_BAYER) return true;
    for (int i = 0; i < desc->nb_components; ++i) {
        if (desc->comp[i].depth > 8) return true;
    }
    return false;
}

inline AVPixelFormat cpuPublishPixelFormat(int avPixFmt)
{
    return sourceNeeds16BitPublish(avPixFmt) ? AV_PIX_FMT_RGBA64LE
                                             : AV_PIX_FMT_RGBA;
}

// In-place 16–235 → 0–255 on packed RGBA8888 rows; alpha untouched.
inline void expandRgba8LegalToFull(uint8_t *data, int width, int height, int stride)
{
    static const std::array<uint8_t, 256> kLut = [] {
        std::array<uint8_t, 256> t{};
        for (int v = 0; v < 256; ++v) {
            const int e = ((v - 16) * 255 + 109) / 219;   // round
            t[v] = static_cast<uint8_t>(std::clamp(e, 0, 255));
        }
        return t;
    }();
    if (!data || width <= 0 || height <= 0) return;
    for (int y = 0; y < height; ++y) {
        uint8_t *row = data + static_cast<std::ptrdiff_t>(y) * stride;
        for (int x = 0; x < width; ++x, row += 4) {
            row[0] = kLut[row[0]];
            row[1] = kLut[row[1]];
            row[2] = kLut[row[2]];
        }
    }
}

// 16-bit sibling: in-place 4096–60160 (16–235 << 8) → 0–65535 on packed
// RGBA64 rows (uint16 per channel); alpha untouched. `stride` in bytes.
inline void expandRgba16LegalToFull(uint16_t *data, int width, int height, int stride)
{
    if (!data || width <= 0 || height <= 0) return;
    constexpr int64_t kLo = 16 << 8, kHi = 235 << 8, kSpan = kHi - kLo;
    // 64-bit: (v - lo) * 65535 overflows int32 above v ≈ 36 800, which
    // showed up as bright regions wrapping to black / magenta.
    auto expand = [](uint16_t v) -> uint16_t {
        const int64_t e = ((static_cast<int64_t>(v) - kLo) * 65535 + kSpan / 2) / kSpan;
        return static_cast<uint16_t>(std::clamp<int64_t>(e, 0, 65535));
    };
    for (int y = 0; y < height; ++y) {
        uint16_t *row = reinterpret_cast<uint16_t *>(
            reinterpret_cast<uint8_t *>(data) + static_cast<std::ptrdiff_t>(y) * stride);
        for (int x = 0; x < width; ++x, row += 4) {
            row[0] = expand(row[0]);
            row[1] = expand(row[1]);
            row[2] = expand(row[2]);
        }
    }
}

} // namespace qcv
