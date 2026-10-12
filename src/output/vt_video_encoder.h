// VtVideoEncoder — VideoToolbox H.264 session (plan A0). Surfaces are
// NV12 CVPixelBuffers from the session's pool, IOSurface-backed and Metal
// compatible, so the renderer's conversion kernel writes them in place.
// Output: Annex-B access units, SPS/PPS prepended on keyframes, delivered
// on VideoToolbox's callback thread.

#pragma once

#include "video_encoder.h"

#include <atomic>
#include <memory>
#include <mutex>

namespace qcv::output {

class VtVideoEncoder final : public VideoEncoder {
public:
    VtVideoEncoder();
    ~VtVideoEncoder() override;

    bool prepareSurfaces(uint32_t width, uint32_t height, std::string *error) override;
    void releaseSurfaces() override;
    bool open(const EncoderSpec &spec, Callback cb, std::string *error) override;
    void close() override;
    bool isOpen() const override;
    const char *name() const override { return "VideoToolbox H.264"; }

    void *acquireSurface() override;
    void  retainSurface(void *surface) override;
    void  releaseSurface(void *surface) override;

    bool encode(void *surface, int64_t pts90k, int64_t dur90k, bool forceKeyframe) override;
    Stats stats() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace qcv::output
