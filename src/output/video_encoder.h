// VideoEncoder — one H.264 hardware session behind a platform-neutral
// face (plan A, "Architecture"). VideoToolbox on macOS (vt_video_encoder.mm),
// Media Foundation on Windows (mf_video_encoder.cpp, A2).
//
// The encoder owns the surfaces: the renderer writes NV12 into a surface
// from acquireSurface(), the paced clock hands it to encode(). Output is
// Annex-B access units with the parameter sets in band on every keyframe,
// delivered on the encoder's own thread through the callback; the callback
// must only enqueue.

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace qcv::output {

enum class H264Profile { Baseline, Main };

struct EncoderSpec {
    uint32_t width  = 1920;
    uint32_t height = 1080;
    uint32_t fpsNum = 30, fpsDen = 1;
    uint32_t bitrateKbps = 6000;
    uint32_t gopFrames   = 30;        // keyframe every second at the clock's rate
    H264Profile profile  = H264Profile::Baseline;
};

struct EncodedUnit {
    std::vector<uint8_t> data;        // Annex-B, one access unit
    int64_t pts90k   = 0;
    int64_t dur90k   = 0;
    bool    keyframe = false;
};

class VideoEncoder {
public:
    using Callback = std::function<void(EncodedUnit &&)>;
    virtual ~VideoEncoder() = default;

    // Surfaces exist independently of a session: the renderer starts
    // writing before the clock rate (and so the session) is known, and a
    // session can be reopened at another rate over the same pool.
    virtual bool prepareSurfaces(uint32_t width, uint32_t height, std::string *error) = 0;
    virtual void releaseSurfaces() = 0;   // after every surface came back

    virtual bool open(const EncoderSpec &spec, Callback cb, std::string *error) = 0;
    virtual void close() = 0;         // completes what is in flight, then invalidates
    virtual bool isOpen() const = 0;
    virtual const char *name() const = 0;

    // Surfaces (opaque; CVPixelBufferRef on macOS). acquire hands out a
    // +1 reference the caller must balance with release(); retain() adds
    // one more for a caller keeping the surface (the repeat-last picture).
    virtual void *acquireSurface() = 0;
    virtual void  retainSurface(void *surface) = 0;
    virtual void  releaseSurface(void *surface) = 0;

    // Encode one surface at pts (90 kHz). The encoder keeps its own
    // reference while it works; the caller's stays the caller's.
    virtual bool encode(void *surface, int64_t pts90k, int64_t dur90k, bool forceKeyframe) = 0;

    struct Stats { uint64_t submitted = 0, encoded = 0, keyframes = 0, bytes = 0, errors = 0; };
    virtual Stats stats() const = 0;
};

// The platform's hardware encoder, or nullptr where none is wired up yet.
std::unique_ptr<VideoEncoder> makePlatformVideoEncoder();

} // namespace qcv::output
