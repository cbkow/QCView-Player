#include "yuv_planar.h"

#include "sws_threaded.h"   // effectiveMatrix / effectiveSourceRange

#include <atomic>

extern "C" {
#include <libavutil/common.h>
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

namespace qcv {

namespace {

std::atomic<bool> g_cpuYuvSupported{false};

// Layout of a supported format, independent of frame size.
struct Layout {
    bool ok = false;
    int  planeCount = 0;
    bool interleaved = false;
    bool alpha = false;
    int  bytes = 1;
    int  codeBits = 8;
    int  shiftX = 0, shiftY = 0;
    int  dataIndex[4] = {0, 1, 2, 3};
};

Layout layoutFor(int avPixFmt)
{
    Layout l;
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(avPixFmt));
    if (!d) return l;
    const uint64_t excluded = AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_HWACCEL
                            | AV_PIX_FMT_FLAG_BAYER | AV_PIX_FMT_FLAG_FLOAT | AV_PIX_FMT_FLAG_BE;
    if (d->flags & excluded) return l;
    if (d->nb_components != 3 && d->nb_components != 4) return l;
    if (d->log2_chroma_w > 1 || d->log2_chroma_h > 1) return l;
    const int depth = d->comp[0].depth, shift = d->comp[0].shift;
    for (int c = 1; c < d->nb_components; ++c) {
        if (d->comp[c].depth != depth || d->comp[c].shift != shift) return l;
    }
    if (depth < 8 || depth + shift > 16) return l;
    const int bytes = depth + shift > 8 ? 2 : 1;
    const bool alpha = d->nb_components == 4;

    // Luma alone in plane 0, one sample per step.
    if (d->comp[0].plane != 0 || d->comp[0].offset != 0 || d->comp[0].step != bytes) return l;

    if (d->comp[1].plane != d->comp[2].plane) {
        // Planar: U, V (and A) each alone in a plane of their own.
        if (d->comp[1].step != bytes || d->comp[2].step != bytes
            || d->comp[1].offset != 0 || d->comp[2].offset != 0) return l;
        l.planeCount  = alpha ? 4 : 3;
        l.dataIndex[1] = d->comp[1].plane;
        l.dataIndex[2] = d->comp[2].plane;
    } else {
        // Interleaved chroma in one plane, U first (NV12 / P010 / P016;
        // NV21's V-first order keeps swscale).
        if (d->comp[1].step != 2 * bytes || d->comp[1].offset != 0
            || d->comp[2].offset != bytes) return l;
        l.planeCount  = alpha ? 3 : 2;
        l.interleaved = true;
        l.dataIndex[1] = d->comp[1].plane;
    }
    if (alpha) {
        if (d->comp[3].step != bytes || d->comp[3].offset != 0) return l;
        l.dataIndex[l.planeCount - 1] = d->comp[3].plane;
    }
    l.alpha    = alpha;
    l.bytes    = bytes;
    l.codeBits = depth + shift;
    l.shiftX   = d->log2_chroma_w;
    l.shiftY   = d->log2_chroma_h;
    l.ok       = true;
    return l;
}

} // namespace

bool yuvPlanarFormatSupported(int avPixFmt)
{
    return layoutFor(avPixFmt).ok;
}

YuvPlanarDesc yuvPlanarDesc(const AVFrame *frame, int rangeOverride)
{
    YuvPlanarDesc out;
    if (!frame || frame->width <= 0 || frame->height <= 0) return out;
    const Layout l = layoutFor(frame->format);
    if (!l.ok) return out;

    out.width  = frame->width;
    out.height = frame->height;
    out.planeCount        = l.planeCount;
    out.interleavedChroma = l.interleaved;
    out.hasAlpha          = l.alpha;
    out.chromaShiftX      = l.shiftX;
    out.chromaShiftY      = l.shiftY;
    out.bytesPerSample    = l.bytes;
    const int cw = AV_CEIL_RSHIFT(frame->width, l.shiftX);
    const int ch = AV_CEIL_RSHIFT(frame->height, l.shiftY);
    for (int p = 0; p < l.planeCount; ++p) {
        auto &pl = out.planes[p];
        pl.dataIndex = l.dataIndex[p];
        const bool chroma = p > 0 && !(l.alpha && p == l.planeCount - 1);
        pl.width    = chroma ? cw : frame->width;
        pl.height   = chroma ? ch : frame->height;
        pl.channels = (chroma && l.interleaved) ? 2 : 1;
    }

    out.codeMax   = l.bytes == 2 ? 65535.0f : 255.0f;
    out.levelK    = static_cast<float>(1 << (l.codeBits - 8));
    out.fullMax   = static_cast<float>((1 << l.codeBits) - 1);
    out.chromaMid = static_cast<float>(1 << (l.codeBits - 1));
    out.fullRange = effectiveSourceRange(frame, rangeOverride) == AVCOL_RANGE_JPEG;

    // swscale's coefficient table (SWS_CS_*), as Kr / Kb.
    switch (effectiveMatrix(frame)) {
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:  out.kr = 0.299f;  out.kb = 0.114f;  break;
    case AVCOL_SPC_SMPTE240M:  out.kr = 0.212f;  out.kb = 0.087f;  break;
    case AVCOL_SPC_FCC:        out.kr = 0.30f;   out.kb = 0.11f;   break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:  out.kr = 0.2627f; out.kb = 0.0593f; break;
    default:                   out.kr = 0.2126f; out.kb = 0.0722f; break;   // BT.709
    }
    out.ok = true;
    return out;
}

void yuvPlanarToRgb(const YuvPlanarDesc &d, float yCode, float uCode, float vCode, float rgb[3])
{
    float y, u, v;
    if (d.fullRange) {
        y = yCode / d.fullMax;
        u = (uCode - d.chromaMid) / d.fullMax;
        v = (vCode - d.chromaMid) / d.fullMax;
    } else {
        y = (yCode -  16.0f * d.levelK) / (219.0f * d.levelK);
        u = (uCode - 128.0f * d.levelK) / (224.0f * d.levelK);
        v = (vCode - 128.0f * d.levelK) / (224.0f * d.levelK);
    }
    rgb[0] = y + 2.0f * (1.0f - d.kr) * v;
    rgb[2] = y + 2.0f * (1.0f - d.kb) * u;
    rgb[1] = (y - d.kr * rgb[0] - d.kb * rgb[2]) / (1.0f - d.kr - d.kb);
}

void setCpuYuvRenderingSupported(bool on)
{
    g_cpuYuvSupported.store(on, std::memory_order_release);
}

bool cpuYuvRenderingEnabled()
{
    static const bool forcedOff = qEnvironmentVariableIsSet("QCV_CPU_YUV_SWSCALE");
    return !forcedOff && g_cpuYuvSupported.load(std::memory_order_acquire);
}

} // namespace qcv
