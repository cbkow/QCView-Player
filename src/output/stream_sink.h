// StreamSink — the stream output's worker (plan A, "Architecture"): takes
// the renderer's NV12 surfaces through IGpuFrameSink, ticks a constant
// clock, encodes the newest picture (or repeats the last one) through the
// platform's VideoEncoder, encodes the players' audio tap as AAC with
// silence where the tap is idle, and muxes both to the network.
//
// Threads:
//   render thread      acquireSurface() / submit() / release()
//   device thread      pushAudio() (lock-free into an SPSC ring)
//   paced thread       the clock, the video session, repeat-last
//   audio thread       1024-sample frames from the ring, silence when behind
//   encoder callback   enqueues access units only
//   mux thread         av_interleaved_write_frame (may block on SRT)
//   GUI thread         start() / stop() / setSourceFps() / status()
//
// Lifetime: the renderer may still hold surfaces (a GPU completion handler
// pending) when stop() is called; stop() waits for them to come back
// before the pool goes, so the sink must outlive its OutputTarget's
// removal from the renderer (OutputBus does pushTargets() first).

#pragma once

#include "aac_encoder.h"
#include "audio/audio_ring_buffer.h"
#include "render/gpu_frame_sink.h"
#include "stream_muxer.h"
#include "video_encoder.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace qcv::output {

struct StreamConfig {
    std::string url;                   // srt://host:port[?streamid=...]
    uint32_t    width  = 1920, height = 1080;
    uint32_t    bitrateKbps = 6000;
    H264Profile profile = H264Profile::Baseline;
    int         srtLatencyMs = 120;
    std::string streamId, passphrase;
    // The clock. 0/0 = take the source rate at the first frame (30 when
    // the source has none).
    uint32_t    fpsNum = 0, fpsDen = 0;
    uint32_t    audioKbps = 160;       // 0 = video only
};

class StreamSink final : public IGpuFrameSink {
public:
    explicit StreamSink(std::unique_ptr<VideoEncoder> encoder);
    ~StreamSink() override;

    bool start(const StreamConfig &cfg, std::string *error);
    void stop();
    bool running() const { return m_running.load(std::memory_order_acquire); }
    // The session ended on its own (connect failed, encoder failed).
    bool failed() const { return m_failed.load(std::memory_order_acquire); }

    // What the source runs at, from the bus (poll). Read once, when the
    // clock starts.
    void setSourceFps(uint32_t num, uint32_t den)
    {
        m_srcFpsNum.store(num, std::memory_order_relaxed);
        m_srcFpsDen.store(den, std::memory_order_relaxed);
    }

    // IGpuFrameSink (render thread / GPU completion)
    void *acquireSurface(uint32_t w, uint32_t h) override;
    void  submit(void *surface, const GpuFrameDesc &desc) override;
    void  release(void *surface) override;

    // The players' audio tap: interleaved stereo float, 48 kHz, device
    // thread. Zeros when muted. Lock-free; drops when a second producer
    // overlaps (the ring is single-producer).
    void pushAudio(const float *stereo, uint32_t frames, bool muted);

    struct Status {
        std::string state;             // "waiting for the first frame", "connecting", "streaming", "error: ..."
        std::string encoder;
        uint64_t framesIn = 0;         // surfaces submitted by the renderer
        uint64_t framesRepeated = 0;   // clock ticks with no new picture
        uint64_t framesSkipped = 0;    // pictures replaced before a tick took them
        uint64_t framesDropped = 0;    // units dropped from the network queue
        uint64_t framesEncoded = 0, keyframes = 0;
        uint64_t audioFrames = 0, audioSilent = 0;
        uint64_t packets = 0, bytes = 0, muxErrors = 0;
        double   fps = 0.0;
        double   kbps = 0.0;           // over the last second
    };
    Status status() const;

private:
    void pacedThread();
    void audioThread();
    void muxThread();
    void setState(const std::string &s);
    void enqueue(EncodedUnit &&u);

    std::unique_ptr<VideoEncoder> m_encoder;
    AacEncoder  m_aac;
    StreamMuxer m_muxer;
    StreamConfig m_cfg;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_stopping{false};
    std::atomic<bool> m_failed{false};
    std::thread m_paced, m_audio, m_mux;

    // the newest picture, handed over by submit()
    std::mutex m_latestMutex;
    std::condition_variable m_latestCv;
    void *m_latest = nullptr;
    GpuFrameDesc m_latestDesc {};
    std::atomic<int> m_inFlight{0};    // acquired, not yet submitted/released

    // the clock: steady_clock ns at tick 0, 0 until the first picture
    std::atomic<int64_t> m_clockT0{0};
    std::atomic<bool>    m_muxReady{false};

    // audio: device thread → ring → audio thread
    AudioRingBuffer m_audioRing{ 48000 * 2 * sizeof(float) * 2 };   // 2 s
    std::atomic<bool> m_audioBusy{false};

    // access units from the encoders to the mux thread
    std::mutex m_auMutex;
    std::condition_variable m_auCv;
    std::deque<EncodedUnit> m_aus;
    static constexpr size_t kMaxQueuedUnits = 96;   // ~1.5 s of video + audio

    std::atomic<uint32_t> m_srcFpsNum{0}, m_srcFpsDen{0};
    std::atomic<uint64_t> m_framesIn{0}, m_framesRepeated{0}, m_framesSkipped{0}, m_framesDroppedQueue{0};
    std::atomic<uint64_t> m_audioFrames{0}, m_audioSilent{0};
    std::atomic<double>   m_fps{0.0}, m_kbps{0.0};
    mutable std::mutex m_stateMutex;
    std::string m_state;
};

} // namespace qcv::output
