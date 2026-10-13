// MfVideoEncoder — Media Foundation H.264 (plan A2), the Windows twin of
// VtVideoEncoder. The vendor's hardware MFT (NVENC, AMF, QuickSync) on the
// renderer's D3D11 device through IMFDXGIDeviceManager; surfaces are NV12
// ID3D11Texture2D from a pool here, written by the renderer's two
// plane passes; the asynchronous MFT is driven by its own NeedInput /
// HaveOutput events on a Media Foundation work queue. Output is Annex-B
// (the H.264 MFTs emit byte streams) with SPS/PPS cached and prepended on
// keyframes that lack them.

#pragma once

#include "video_encoder.h"

#include <memory>

namespace qcv::output {

class MfVideoEncoder final : public VideoEncoder {
public:
    MfVideoEncoder();
    ~MfVideoEncoder() override;

    bool prepareSurfaces(uint32_t width, uint32_t height, std::string *error) override;
    void releaseSurfaces() override;
    bool open(const EncoderSpec &spec, Callback cb, std::string *error) override;
    void close() override;
    bool isOpen() const override;
    const char *name() const override;

    void *acquireSurface() override;
    void  retainSurface(void *surface) override;
    void  releaseSurface(void *surface) override;

    bool encode(void *surface, int64_t pts90k, int64_t dur90k, bool forceKeyframe) override;
    Stats stats() const override;

    struct Impl;   // public: the Media Foundation event callback drives it

private:
    std::unique_ptr<Impl> m_impl;
};

} // namespace qcv::output
