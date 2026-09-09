#pragma once

// Threaded swscale for the CPU publish paths (2026-09-09).
//
// The stateful pointer API (`sws_getContext` + `sws_scale`) and even a
// legacy-initialised context driven through `sws_scale_frame` run the
// "unscaled" special converters (Bayer demosaic, planar→packed RGB) as
// ONE whole-frame call — measured 30 ms/frame for 4224x3024 Bayer→RGBA64
// with 1 or 16 threads alike, 48 ms for 8294x3164 yuv444p12→RGBA64. Only
// swscale 10's DYNAMIC mode (an un-initialised context + sws_scale_frame,
// properties taken from the AVFrames) slices those converters across
// `SwsContext.threads` (graph.c splits every pass into num_threads
// slices). CLI reference: 2 ms and 4 ms respectively with 16 threads.
//
// Exception: Bayer (ProRes RAW) demosaic stays single-threaded — see
// swsConvertToBuffer. Parity with the old path was verified by dumping
// first frames from both modes (QCV_SWS_LEGACY=1 / QCV_DUMP_FRAME=<dir>)
// and diffing: VVC 4:2:0 10-bit, FFV1 4:2:2 10-bit, FFV1 4:2:0 8-bit,
// FFV1 RGB 12-bit and DNxHR RGB 12-bit are md5-identical.
//
// Colour policy in dynamic mode — the app is OCIO-only, swscale must do
// matrix + range and NOTHING else:
//   * the YUV matrix and the source range are set on a shallow ref of
//     the source frame (effectiveMatrix / effectiveSourceRange — the
//     same rules every site used with sws_setColorspaceDetails: tagged
//     matrix, else HD→BT.709 / SD→BT.601; Range pill override; RGB
//     sources declare NO range change because rgb_range.h owns the
//     legal→full expansion);
//   * the destination is full-range RGB with the SOURCE's primaries and
//     transfer copied over, so ff_infer_colors() sees identical colour
//     on both sides and no gamut / tone / transfer mapping is built.
//
// The unstable ops backends (SWS_UNSTABLE / x86 SIMD) are deliberately
// NOT enabled — see the swscale-ops-backend-watch memory note.

#include <cstdint>
#include <cstdlib>

extern "C" {
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

namespace qcv {

// Thread count for conversion contexts: QCV_SWS_THREADS env override
// (tuning / A-B), else the caller's default (0 = swscale auto).
inline int swsDefaultThreads(int fallback = 0)
{
    if (const char *e = std::getenv("QCV_SWS_THREADS")) {
        const int n = std::atoi(e);
        if (n >= 0) return n;
    }
    return fallback;
}

// Dynamic-mode conversion context (NOT sws_init_context'ed). Reusable
// across frame sizes / formats; swscale rebuilds its graph internally
// when the frame properties change.
inline SwsContext *swsCreateThreaded(int threads = 0, int flags = SWS_BILINEAR)
{
    SwsContext *c = sws_alloc_context();
    if (!c) return nullptr;
    c->flags   = static_cast<unsigned>(flags);
    c->threads = swsDefaultThreads(threads);
    return c;
}

// YUV→RGB matrix for a decoded frame: the tagged one when known, else
// the HD→BT.709 / SD→BT.601 heuristic every site has always used.
inline AVColorSpace effectiveMatrix(const AVFrame *f)
{
    switch (f->colorspace) {
    case AVCOL_SPC_BT709:
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
    case AVCOL_SPC_SMPTE240M:
    case AVCOL_SPC_FCC:
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        return f->colorspace;
    default:
        return (f->width >= 1280 || f->height >= 720) ? AVCOL_SPC_BT709
                                                      : AVCOL_SPC_SMPTE170M;
    }
}

// Source range fed to the conversion. rangeOverride: 0 = Auto (the
// frame's tag, untagged → limited), 1 = Full, 2 = Limited. RGB sources
// always declare FULL so swscale leaves the levels alone — the
// legal→full expansion for RGB lives in rgb_range.h (one rule for
// playback / scrub / dual / thumbs; the 16-bit RGB path in swscale 10
// would otherwise expand a second time).
inline AVColorRange effectiveSourceRange(const AVFrame *f, int rangeOverride)
{
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(f->format));
    if (d && (d->flags & AV_PIX_FMT_FLAG_RGB)) return AVCOL_RANGE_JPEG;
    if (rangeOverride == 1) return AVCOL_RANGE_JPEG;
    if (rangeOverride == 2) return AVCOL_RANGE_MPEG;
    return (f->color_range == AVCOL_RANGE_JPEG) ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
}

// Convert `src` into a caller-owned packed buffer (`dst`, `dstStride`
// bytes per row) of `dstFormat` at the source size, sliced across the
// context's threads. Returns >= 0 on success. Nothing is allocated for
// the pixels; both frames are transient wrappers.
inline int swsConvertToBuffer(SwsContext *c, const AVFrame *src,
                              AVPixelFormat dstFormat,
                              uint8_t *dst, int dstStride,
                              int rangeOverride = 0)
{
    if (!c || !src || !dst || dstStride <= 0) return -1;

    // Parity harness: QCV_SWS_LEGACY=1 routes through the pre-2026-09-09
    // stateful API (sws_getContext + sws_setColorspaceDetails +
    // sws_scale, single-threaded, a throwaway context per call) so a
    // frame dump from each mode can be diffed. Not a production path.
    static const bool kLegacy = [] {
        const char *e = std::getenv("QCV_SWS_LEGACY");
        return e && *e && *e != '0';
    }();
    if (kLegacy) {
        SwsContext *lc = sws_getContext(src->width, src->height,
                                        static_cast<AVPixelFormat>(src->format),
                                        src->width, src->height, dstFormat,
                                        SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!lc) return -1;
        int csp;
        switch (effectiveMatrix(src)) {
        case AVCOL_SPC_BT709:      csp = SWS_CS_ITU709;    break;
        case AVCOL_SPC_BT470BG:    csp = SWS_CS_ITU601;    break;
        case AVCOL_SPC_SMPTE170M:  csp = SWS_CS_SMPTE170M; break;
        case AVCOL_SPC_SMPTE240M:  csp = SWS_CS_SMPTE240M; break;
        case AVCOL_SPC_FCC:        csp = SWS_CS_FCC;       break;
        default:                   csp = SWS_CS_BT2020;    break;
        }
        const int srcFull = (effectiveSourceRange(src, rangeOverride) == AVCOL_RANGE_JPEG) ? 1 : 0;
        sws_setColorspaceDetails(lc, sws_getCoefficients(csp), srcFull,
                                 sws_getCoefficients(SWS_CS_ITU709), 1,
                                 0, 1 << 16, 1 << 16);
        uint8_t *dd[4]  = { dst, nullptr, nullptr, nullptr };
        int      ds[4]  = { dstStride, 0, 0, 0 };
        const int r = sws_scale(lc, src->data, src->linesize, 0, src->height, dd, ds);
        sws_freeContext(lc);
        return r < 0 ? r : 0;
    }

    // Bayer demosaic is NOT slice-safe: swscale's sliced runner treats
    // every slice edge as an image edge, so the 2-D neighbourhood is
    // wrong in a 4-row band at each boundary (parity harness: 60 of
    // 3024 rows differ, PSNR 67 dB, on the iPhone ProRes RAW clip).
    // YUV / RGB conversions are row-local and came out bit-identical.
    // Bayer therefore converts on ONE thread (30 ms/frame at 4224x3024)
    // until the GPU debayer lands.
    {
        const AVPixFmtDescriptor *sd = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(src->format));
        const bool bayer = sd && (sd->flags & AV_PIX_FMT_FLAG_BAYER);
        if (bayer && c->threads != 1) c->threads = 1;
    }

    // Shallow ref (buffer refcounts only) so the colour fields can be
    // set without touching the decoder's frame.
    AVFrame *s = av_frame_alloc();
    AVFrame *d = av_frame_alloc();
    if (!s || !d) { av_frame_free(&s); av_frame_free(&d); return -1; }
    int ret = av_frame_ref(s, src);
    if (ret < 0) { av_frame_free(&s); av_frame_free(&d); return ret; }
    s->colorspace  = effectiveMatrix(src);
    s->color_range = effectiveSourceRange(src, rangeOverride);
    if (s->color_primaries == AVCOL_PRI_UNSPECIFIED) s->color_primaries = AVCOL_PRI_BT709;
    if (s->color_trc       == AVCOL_TRC_UNSPECIFIED) s->color_trc       = AVCOL_TRC_BT709;

    d->format          = dstFormat;
    d->width           = src->width;
    d->height          = src->height;
    d->colorspace      = AVCOL_SPC_RGB;
    d->color_range     = AVCOL_RANGE_JPEG;
    d->color_primaries = s->color_primaries;   // identical → no CMS
    d->color_trc       = s->color_trc;
    d->chroma_location = AVCHROMA_LOC_UNSPECIFIED;
    // sws_scale_frame only writes into a caller buffer that is
    // reference-counted; wrap our memory in a no-op-free AVBufferRef.
    d->buf[0] = av_buffer_create(dst,
                                 static_cast<size_t>(dstStride) * src->height,
                                 [](void *, uint8_t *) {}, nullptr, 0);
    if (!d->buf[0]) { av_frame_free(&s); av_frame_free(&d); return -1; }
    d->data[0]     = dst;
    d->linesize[0] = dstStride;

    ret = sws_scale_frame(c, d, s);
    av_frame_free(&d);   // unrefs buf[0] → our no-op free
    av_frame_free(&s);
    return ret;
}

} // namespace qcv
