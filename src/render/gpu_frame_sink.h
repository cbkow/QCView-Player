// IGpuFrameSink — an output target that takes the wire picture on the GPU
// instead of in a ring slot (the stream output, plan A0/A1).
//
// The renderer's output pass renders the corrected canvas at the target's
// raster as for a ring target (steps 1–3), then, instead of blitting into
// a slot, converts it to NV12 (BT.709, limited range) into a surface the
// sink hands out and gives the surface back once the GPU has written it.
//
// Surfaces are opaque: a CVPixelBufferRef on macOS (from the encoder's
// pool, retained for the renderer), an ID3D11Texture2D* on Windows. The
// renderer owns a surface from acquire() until it calls submit() or
// release() with it, exactly once, from any thread.
//
// No Qt here: qcv_render's Metal and D3D11 implementations include this,
// qcv_output implements it.

#pragma once

#include <cstdint>

namespace qcv::output {

struct GpuFrameDesc {
    uint32_t width  = 0;
    uint32_t height = 0;
    int64_t  presentation_ns   = 0;   // qcbae::OutputRing::now_ns() when the frame was encoded
    int64_t  source_frame_index = -1;
};

class IGpuFrameSink {
public:
    virtual ~IGpuFrameSink() = default;

    // Render thread. An NV12 surface of exactly w x h the renderer may
    // write, or nullptr when the sink has none to spare (the frame is
    // skipped; the sink repeats its last one).
    virtual void *acquireSurface(uint32_t w, uint32_t h) = 0;

    // The GPU has finished writing `surface`: it is the sink's newest
    // picture. Called from a GPU completion handler or the render thread.
    virtual void submit(void *surface, const GpuFrameDesc &desc) = 0;

    // The renderer could not use the surface after all.
    virtual void release(void *surface) = 0;
};

} // namespace qcv::output
