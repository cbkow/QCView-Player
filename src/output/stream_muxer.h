// StreamMuxer — Annex-B H.264 and AAC into MPEG-TS over SRT, or FLV over
// RTMP / RTMPS (plan A3), through libavformat. FLV wants the parameter
// sets at the header: given up front (videoExtradata, Annex-B SPS+PPS) the
// header is written on open; otherwise it waits for the first keyframe
// and is written from the SPS/PPS inside it (the units before are dropped).
// libavformat's flv muxer converts the Annex-B units to length-prefixed
// ones itself when the extradata is not avcC.
//
// One thread writes; open() blocks on the connect (the SRT caller
// handshake) and honours interrupt(), as the SRT receiver does
// (decode/live_stream_decoder.cpp). The URL carries the protocol and
// address; SRT's options (`mode`, `latency` in µs, `streamid`,
// `passphrase`/`pbkeylen`, `connect_timeout` in ms) go as protocol options
// so a key never has to be spliced into the URL.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct AVFormatContext;
struct AVStream;

namespace qcv::output {

struct MuxConfig {
    std::string url;                 // srt://host:port[?streamid=...]
    uint32_t width = 0, height = 0;
    uint32_t fpsNum = 30, fpsDen = 1;
    // SRT
    int         srtLatencyMs     = 120;
    int         connectTimeoutMs = 5000;
    std::string streamId;            // empty: whatever the URL carries
    std::string passphrase;          // empty: no encryption
    int         pbKeyLen = 16;
    // Audio (AAC-LC 48 kHz stereo); empty extradata = video only.
    std::vector<uint8_t> audioExtradata;
    uint32_t audioKbps = 160;
    // Video parameter sets (Annex-B SPS + PPS), FLV only; empty = take
    // them from the first keyframe.
    std::vector<uint8_t> videoExtradata;
};

class StreamMuxer {
public:
    StreamMuxer();
    ~StreamMuxer();

    bool open(const MuxConfig &cfg, std::string *error);
    void close();
    // Connected (the header may still be waiting for a keyframe, FLV).
    bool isOpen() const { return m_ctx != nullptr && m_connected; }
    bool headerWritten() const { return m_headerWritten; }
    static bool isRtmp(const std::string &url);
    // The Annex-B SPS and PPS units inside an access unit, with their
    // start codes; false when it carries none.
    static bool parameterSets(const uint8_t *data, size_t size, std::vector<uint8_t> *out);

    // Thread-safe wake for a blocked open()/write.
    void interrupt() { m_interrupt.store(true, std::memory_order_release); }

    // One access unit, Annex-B, pts/dts in 90 kHz (no B-frames: equal).
    bool writeVideo(const uint8_t *data, size_t size, int64_t pts90k, int64_t dur90k, bool keyframe);
    // One AAC frame, pts/dur in samples (1/48000).
    bool writeAudio(const uint8_t *data, size_t size, int64_t ptsSamples, int64_t durSamples);
    bool hasAudio() const { return m_audio != nullptr; }

    struct Stats { uint64_t packets = 0, bytes = 0, errors = 0; };
    Stats stats() const { return { m_packets.load(), m_bytes.load(), m_errors.load() }; }
    std::string lastError() const { return m_lastError; }

private:
    static int interruptCb(void *opaque);
    bool writeHeader(std::string *error);

    AVFormatContext *m_ctx   = nullptr;
    AVStream        *m_video = nullptr;
    AVStream        *m_audio = nullptr;
    bool             m_headerWritten = false;
    bool             m_connected = false;
    bool             m_flv = false;
    std::atomic<bool> m_interrupt{false};
    std::atomic<uint64_t> m_packets{0}, m_bytes{0}, m_errors{0};
    std::string m_lastError;
};

} // namespace qcv::output
