// AacEncoder — FFmpeg's built-in AAC-LC encoder for the stream output's
// audio: 48 kHz stereo planar float in, 1024-sample frames, ADTS-less raw
// AAC out with the AudioSpecificConfig as extradata for the muxer.
// One thread (the sink's audio thread) calls it.

#pragma once

#include "video_encoder.h"

#include <cstdint>
#include <string>
#include <vector>

struct AVCodecContext;
struct AVFrame;
struct AVPacket;

namespace qcv::output {

class AacEncoder {
public:
    static constexpr uint32_t kSampleRate = 48000;
    static constexpr uint32_t kChannels   = 2;
    static constexpr uint32_t kFrameSamples = 1024;

    AacEncoder();
    ~AacEncoder();

    bool open(uint32_t bitrateKbps, std::string *error);
    void close();
    bool isOpen() const { return m_ctx != nullptr; }

    const std::vector<uint8_t> &extradata() const { return m_extradata; }

    // One frame: kFrameSamples per channel, pts in samples (1/48000).
    // Packets (usually one) go to `out` with pts in samples.
    bool encode(const float *left, const float *right, int64_t ptsSamples, std::vector<EncodedUnit> *out);

    uint64_t frames() const { return m_frames; }
    uint64_t bytes() const { return m_bytes; }

private:
    AVCodecContext *m_ctx = nullptr;
    AVFrame  *m_frame = nullptr;
    AVPacket *m_pkt   = nullptr;
    std::vector<uint8_t> m_extradata;
    uint64_t m_frames = 0, m_bytes = 0;
};

} // namespace qcv::output
