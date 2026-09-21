// LiveStreamDecoder — v2.2.3, Blender-bridge stage 2.
//
// Contained "live mode" decode path for srt:// (and other FFmpeg
// network-protocol) URLs. Deliberately bypasses the file-oriented
// architecture: no FrameIndex, no ScrubDecoder/GOP cache, no seeking,
// no duration, no pacing — frames arrive network-paced from the
// sender and are published immediately, latest-wins.
//
// It does NOT render into its own slot: decoded frames are pushed into
// the main VideoDecoder's publish slot via publishExternalFrame() (the
// same sanctioned inlet ScrubDecoder uses), so the renderer's video
// path — Metal and D3D11, single and A/B — needs no changes and no new
// binding. The sink VideoDecoder stays closed (Idle) the whole time;
// only its latest-wins publish slot is borrowed. WindowManager
// guarantees ordering: stopLiveStream() joins this worker before the
// sink's publish slot is cleared by VideoDecoder::close().
//
// Connection lifecycle (stage-0 lessons, see the qcbridge repo's
// streaming-pipeline.md): an SRT listener accepts ONE connection and
// stream-end orphans the peer, so reconnect is entirely the receiver's
// job. The worker loops connect → read/decode/publish → on error or
// EOF back to connect, with capped exponential backoff, until close().
// All blocking FFmpeg calls are bounded by an AVIO interrupt callback
// so close() joins promptly.
//
// Startup honesty: decode starts at the first keyframe (packets before
// it are dropped), which avoids the classic ~1 GOP of benign "missing
// references" decode-error spam a mid-GOP join produces. Sender GOP is
// 1 s, so the gate costs at most that.
//
// Latency discipline (v2.2.4, bridge work order 4c): arrears must be
// structurally impossible. Every connect/reconnect first drains the
// demux backlog to the live edge (packets that read without blocking
// are pre-join history — discarded undecoded) before the keyframe
// gate runs, so the backlog accumulated during stream probe never
// becomes standing latency. In steady state, a burst of non-blocking
// reads means the receiver fell behind (stall, hiccup); the loop then
// skips non-reference frames and the publish conversion until it hits
// the live edge again, publishing at most every 250 ms mid-drain so
// the image keeps moving. Presentation is latest-wins throughout —
// frames are never paced by PTS.

#pragma once

#include "decode/live_source.h"

#include <QString>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

struct AVCodecContext;
struct AVFormatContext;
struct AVFrame;
struct AVRational;
struct SwsContext;

namespace qcv {

class VideoDecoder;

// The QML-facing surface (properties, Status, signals) lives on LiveSource.
class LiveStreamDecoder : public LiveSource
{
    Q_OBJECT

public:
    explicit LiveStreamDecoder(QObject *parent = nullptr);
    ~LiveStreamDecoder() override;

    void setSink(VideoDecoder *sink) override { m_sink = sink; }

    // WindowManager installs the renderer's requestUpdate here (see
    // LiveSource). Same cross-thread contract as the dual sources'
    // setFrameAvailableCallback.
    void setFrameCallback(std::function<void()> cb) override;

    bool open(const QString &url) override;
    void close() override;

    Status  status() const override {
        return static_cast<Status>(m_status.load(std::memory_order_acquire));
    }
    QString url() const override { return m_url; }
    int     width() const override { return m_width.load(std::memory_order_acquire); }
    int     height() const override { return m_height.load(std::memory_order_acquire); }
    bool    hasAudio() const override { return m_hasAudio.load(std::memory_order_acquire); }
    int     reconnectCount() const override {
        return m_reconnects.load(std::memory_order_acquire);
    }
    qint64  framesReceived() const {
        return m_framesReceived.load(std::memory_order_acquire);
    }
    QString codecName() const override;
    QString pixelFormatName() const override;

    // Polled by the live strip's 1 s QML Timer (deltas → fps/Mbps).
    double statFramesReceived() const override {
        return double(m_framesReceived.load(std::memory_order_acquire));
    }
    double statBytesReceived() const override {
        return double(m_bytesReceived.load(std::memory_order_acquire));
    }
    // Frames decoded but not published because the loop was draining
    // backlog to the live edge. A steadily climbing value means the
    // receiver can't keep up with the stream in real time.
    double statFramesConflated() const override {
        return double(m_framesConflated.load(std::memory_order_acquire));
    }
    // Seconds since this session went Live; 0 when not live.
    int statLiveSeconds() const override;

private:
    // Worker internals — all run on m_thread.
    void workerLoop();
    bool connectOnce();          // one connect+read/decode session
    void teardownSession(AVFormatContext **fmt, AVCodecContext **cctx,
                         SwsContext **sws);
    void publishFrame(AVFrame *frame, AVCodecContext *cctx,
                      SwsContext **sws, const AVRational &tb,
                      const char *srcLabel = "sw→RGBA");
    void setStatus(Status s);
    void setSessionMetadata(int w, int h, const QString &codec,
                            const QString &pixFmt, bool hasAudio);
    bool interruptibleSleep(int ms);   // false when close() interrupted it

    static int interruptCb(void *opaque);

    VideoDecoder         *m_sink = nullptr;
    QString               m_url;

    std::thread           m_thread;
    std::atomic<bool>     m_stopRequested{false};
    std::mutex            m_sleepMutex;
    std::condition_variable m_sleepCv;

    std::atomic<int>      m_status{Idle};
    std::atomic<int>      m_width{0};
    std::atomic<int>      m_height{0};
    std::atomic<bool>     m_hasAudio{false};
    std::atomic<int>      m_reconnects{0};
    std::atomic<qint64>   m_framesReceived{0};
    std::atomic<qint64>   m_framesConflated{0};
    std::atomic<qint64>   m_bytesReceived{0};
    std::atomic<qint64>   m_liveSinceMs{0};   // steady_clock ms; 0 = not live

    mutable std::mutex    m_metaMutex;   // guards the two strings below
    QString               m_codecName;
    QString               m_pixelFormatName;

    std::mutex            m_frameCbMutex;
    std::function<void()> m_frameCb;
};

} // namespace qcv
