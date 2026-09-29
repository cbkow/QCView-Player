// Clean YUV (colour plan, stage 1): software-decoded YUV frames reach the
// renderer as planes (FrameHandle::Kind::CpuYuv) and convert on the GPU,
// unclamped. swscale's UNORM RGBA output — even its float output — clipped
// super-whites and sub-blacks before OCIO saw them (a DNxHR HQX range chart
// read 100 % where the same codes through VideoToolbox read 109 %).
//
// This header is the shared description both renderers (Metal, D3D11) and
// the probes use: which pixel formats qualify, how a frame's planes map to
// textures, and the conversion constants. The maths is the Phase 0 levels
// maths in code values, generalised to any bit depth and alignment:
//
//   code    = sample (0..1 UNORM) × codeMax          (255 or 65535)
//   limited : Y = (code − 16·K) / (219·K),  C = (code − 128·K) / (224·K)
//   full    : Y = code / fullMax,           C = (code − chromaMid) / fullMax
//   R = Y + 2(1−Kr)·Cr,  B = Y + 2(1−Kb)·Cb,  G = (Y − Kr·R − Kb·B) / (1−Kr−Kb)
//
// with K = 2^(codeBits − 8), codeBits = depth + shift (10 for LSB-aligned
// yuv422p10le, 16 for MSB-aligned P010). Matrix and range follow swscale's
// rules (sws_threaded.h: effectiveMatrix / effectiveSourceRange), so the GPU
// path decodes exactly what the CPU path did, minus the clamp.

#pragma once

#include <QtGlobal>

extern "C" {
struct AVFrame;
}

namespace qcv {

struct YuvPlanarDesc {
    bool ok = false;
    int  width = 0, height = 0;

    // Textures to upload: Y, U, V[, A] (planar) or Y, UV[, A] (interleaved
    // chroma: NV12 / P010 / P016). One sample per texel, or two for UV.
    int  planeCount = 0;
    bool interleavedChroma = false;
    bool hasAlpha = false;
    int  chromaShiftX = 0, chromaShiftY = 0;   // log2 subsampling
    int  bytesPerSample = 1;                   // 1 or 2 (texture R8 / R16)
    struct Plane {
        int dataIndex = 0;   // AVFrame data[] / linesize[] index
        int width = 0, height = 0;
        int channels = 1;    // 1, or 2 for interleaved UV
    } planes[4];

    // Conversion constants (shader uniforms).
    float codeMax   = 255.0f;
    float levelK    = 1.0f;
    float fullMax   = 255.0f;
    float chromaMid = 128.0f;
    bool  fullRange = false;
    float kr = 0.2126f, kb = 0.0722f;
};

// True for the software YUV layouts the GPU converts: 8–16-bit, 4:2:0 /
// 4:2:2 / 4:4:4, planar with optional alpha, or NV12-style interleaved
// chroma; little-endian, not float. Everything else keeps swscale.
bool yuvPlanarFormatSupported(int avPixFmt);

// Layout + constants for one frame. `rangeOverride`: the Range pill
// (0 Auto, 1 Full, 2 Limited). ok = false when the format is unsupported.
YuvPlanarDesc yuvPlanarDesc(const AVFrame *frame, int rangeOverride);

// The shader's maths on the CPU, from stored integer codes (probes).
void yuvPlanarToRgb(const YuvPlanarDesc &d, float yCode, float uCode, float vCode,
                    float rgb[3]);

// Set by a renderer that converts CpuYuv frames (Metal, D3D11). Decoders
// publish planes only when it is on; QCV_CPU_YUV_SWSCALE=1 forces the old
// swscale path (A/B checks, and a field fallback).
void setCpuYuvRenderingSupported(bool on);
bool cpuYuvRenderingEnabled();

} // namespace qcv
