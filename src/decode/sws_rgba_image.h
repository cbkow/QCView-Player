#pragma once

// swsFrameToRgbaImage — convert a planar/NV12 AVFrame to a packed-RGBA QImage
// via swscale, writing into a destination buffer with the padding swscale's
// SIMD output routines require.
//
// Why the padding matters: swscale's vectorized YUV->RGBA writers store the
// final scanline in fixed pixel blocks (16 px on the AVX2 path), rounding the
// row width up to a whole block. For a width that is NOT a multiple of that
// block — e.g. a portrait 1080-wide clip (1080 % 16 != 0) — the last row's
// store runs a few bytes past the nominal end. A bare QImage(w, h) ends
// exactly at its allocation (often a page boundary), so that overshoot writes
// into an unmapped page and access-violates (0xC0000005). Landscape widths
// (1920/1280/3840) are 16-multiples and never tripped it, which is why only
// odd-width clips crashed. See the scrub-sws-crash diagnosis.
//
// Fix: allocate our own 32-byte-aligned RGBA target with one extra row of
// slack, scale into that, and hand the QImage ownership of the buffer (freed
// via av_free through the QImage cleanup hook). Zero-copy from here on — the
// QImage references the buffer directly. Shared by every scrub publish path
// (single-flow + dual A/B, Windows + macOS).

#include "decode/rgb_range.h"
#include "decode/sws_threaded.h"

#include <QImage>

#include <cstddef>
#include <cstdint>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}

namespace qcv {

// Destination pitch for every swscale → packed RGBA site: the row plus a
// full 16-px SIMD block of padding, 64-byte aligned. The yuv420p → RGBA8
// writer overshoots EVERY row by up to a block (32 bytes at 1080 wide), not
// just the last one. With a tight pitch that lands in the next row's first
// pixels — harmless single-threaded (the next row is written afterwards),
// but under sliced threading the next slice's first row is already done, so
// 8 garbage pixels survive at the left edge of each slice boundary (guard-
// page harness 2026-09-18: 14 of 1920 rows differ 16 threads vs 1 at
// 1080x1920; 0 with this pitch). Consumers must honour bytesPerLine().
inline int swsPaddedStride(int w, int bytesPerPixel)
{
    return (w * bytesPerPixel + 16 * bytesPerPixel + 63) & ~63;
}

// swsAllocImage — a QImage for the CPU publish sites that convert in place
// (playback / dual playback / live: swsConvertToBuffer into image.bits()).
// Padded pitch + the same tail slack as swsFrameToRgbaImage. A bare QImage
// here crashed the 2.3.2 Store build on software-decoded 1080x1920 H.264
// (hardware decode off): packaged apps run on the segment heap, where the
// 8.29 MB block ends on a page edge and the last row's overshoot faults;
// the NT heap of an unpackaged build happens to leave slack there.
// RGBA8888 / RGBA64 only.
inline QImage swsAllocImage(int w, int h, QImage::Format format)
{
    if (w <= 0 || h <= 0) return {};
    const int bpp = (format == QImage::Format_RGBA64) ? 8 : 4;
    const std::size_t stride = static_cast<std::size_t>(swsPaddedStride(w, bpp));
    // +1 row of trailing slack, see the header comment.
    auto *buf = static_cast<uint8_t *>(av_malloc(stride * (h + 1)));
    if (!buf) return {};
    return QImage(buf, w, h, static_cast<qsizetype>(stride), format,
                  [](void *p) { av_free(p); }, buf);
}

// `expandLegalRgb`: apply the RGB legal→full expansion after the scale
// (see rgb_range.h) — callers pass rgbFrameNeedsLegalExpansion(yf, ov).
inline QImage swsFrameToRgbaImage(SwsContext *sws, const AVFrame *yf,
                                  bool expandLegalRgb = false,
                                  int rangeOverride = 0)
{
    if (!sws || !yf || yf->width <= 0 || yf->height <= 0) return {};
    const int w = yf->width;
    const int h = yf->height;
    const int stride = swsPaddedStride(w, 4);   // row + SIMD block, see above
    // +1 row of trailing slack absorbs swscale's last-row block overshoot.
    const std::size_t bufSize = static_cast<std::size_t>(stride) * (h + 1);
    auto *buf = static_cast<uint8_t *>(av_malloc(bufSize));
    if (!buf) return {};

    // Frame API so a context built by swsCreateThreaded slices across
    // its threads (the pointer API is always single-threaded).
    if (swsConvertToBuffer(sws, yf, AV_PIX_FMT_RGBA, buf, stride, rangeOverride) < 0) {
        av_free(buf);
        return {};
    }
    if (expandLegalRgb) expandRgba8LegalToFull(buf, w, h, stride);

    return QImage(buf, w, h, stride, QImage::Format_RGBA8888,
                  [](void *p) { av_free(p); }, buf);
}

} // namespace qcv
