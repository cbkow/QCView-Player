// StreamMuxer — Annex-B H.264 (and, from A1, AAC) into MPEG-TS over SRT
// through libavformat; FLV over RTMP comes with A3.
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
};

class StreamMuxer {
public:
    StreamMuxer();
    ~StreamMuxer();

    bool open(const MuxConfig &cfg, std::string *error);
    void close();
    bool isOpen() const { return m_ctx != nullptr && m_headerWritten; }

    // Thread-safe wake for a blocked open()/write.
    void interrupt() { m_interrupt.store(true, std::memory_order_release); }

    // One access unit, Annex-B, pts/dts in 90 kHz (no B-frames: equal).
    bool writeVideo(const uint8_t *data, size_t size, int64_t pts90k, int64_t dur90k, bool keyframe);

    struct Stats { uint64_t packets = 0, bytes = 0, errors = 0; };
    Stats stats() const { return { m_packets.load(), m_bytes.load(), m_errors.load() }; }
    std::string lastError() const { return m_lastError; }

private:
    static int interruptCb(void *opaque);

    AVFormatContext *m_ctx   = nullptr;
    AVStream        *m_video = nullptr;
    bool             m_headerWritten = false;
    std::atomic<bool> m_interrupt{false};
    std::atomic<uint64_t> m_packets{0}, m_bytes{0}, m_errors{0};
    std::string m_lastError;
};

} // namespace qcv::output
