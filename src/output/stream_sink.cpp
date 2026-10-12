#include "stream_sink.h"

#include "decode/qcbae/output_ring.h"

#include <QtLogging>

#include <chrono>
#include <cmath>

namespace qcv::output {

namespace {
using Clock = std::chrono::steady_clock;
}

StreamSink::StreamSink(std::unique_ptr<VideoEncoder> encoder)
    : m_encoder(std::move(encoder))
{
}

StreamSink::~StreamSink()
{
    stop();
}

void StreamSink::setState(const std::string &s)
{
    std::lock_guard<std::mutex> lk(m_stateMutex);
    if (m_state != s) {
        m_state = s;
        qInfo("StreamSink: %s", s.c_str());
    }
}

bool StreamSink::start(const StreamConfig &cfg, std::string *error)
{
    stop();
    if (!m_encoder) {
        if (error) *error = "no hardware encoder on this platform yet";
        return false;
    }
    if (cfg.url.empty() || cfg.width == 0 || cfg.height == 0 || (cfg.width & 1) || (cfg.height & 1)) {
        if (error) *error = "stream needs a URL and an even raster";
        return false;
    }
    m_cfg = cfg;
    if (!m_encoder->prepareSurfaces(cfg.width, cfg.height, error)) return false;
    m_framesIn = 0; m_framesRepeated = 0; m_framesSkipped = 0; m_framesDroppedQueue = 0;
    m_fps = 0.0; m_kbps = 0.0;
    m_stopping.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    setState("waiting for the first frame");
    m_mux   = std::thread([this] { muxThread(); });
    m_paced = std::thread([this] { pacedThread(); });
    return true;
}

void StreamSink::stop()
{
    if (!m_running.load(std::memory_order_acquire) && !m_paced.joinable() && !m_mux.joinable()) return;
    // Order (plan A, "Threading"): the renderer's target is already gone
    // from the pass (the bus pushed the targets first) — surfaces still
    // out are GPU completions in flight.
    m_running.store(false, std::memory_order_release);
    m_latestCv.notify_all();
    m_muxer.interrupt();
    if (m_paced.joinable()) m_paced.join();      // closes the encoder: its callbacks drain into the queue
    m_stopping.store(true, std::memory_order_release);   // nothing arrives after this
    m_auCv.notify_all();
    if (m_mux.joinable()) m_mux.join();          // writes what is queued, then the trailer
    // Surfaces the renderer still holds come back through submit()/release()
    // once their command buffers complete; give them a moment.
    const auto deadline = Clock::now() + std::chrono::milliseconds(1500);
    while (m_inFlight.load(std::memory_order_acquire) > 0 && Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (m_inFlight.load() > 0) {
        qWarning("StreamSink: %d surface(s) never came back from the renderer; the pool stays allocated",
                 m_inFlight.load());
    } else {
        void *latest = nullptr;
        { std::lock_guard<std::mutex> lk(m_latestMutex); latest = m_latest; m_latest = nullptr; }
        if (latest) m_encoder->releaseSurface(latest);
        m_encoder->releaseSurfaces();
    }
    { std::lock_guard<std::mutex> lk(m_auMutex); m_aus.clear(); }
    setState("stopped");
}

// ---- IGpuFrameSink -----------------------------------------------------------------

void *StreamSink::acquireSurface(uint32_t w, uint32_t h)
{
    if (!m_running.load(std::memory_order_acquire)) return nullptr;
    if (w != m_cfg.width || h != m_cfg.height) return nullptr;
    void *s = m_encoder->acquireSurface();
    if (s) m_inFlight.fetch_add(1, std::memory_order_acq_rel);
    return s;
}

void StreamSink::submit(void *surface, const GpuFrameDesc &desc)
{
    if (!surface) return;
    m_inFlight.fetch_sub(1, std::memory_order_acq_rel);
    if (!m_running.load(std::memory_order_acquire)) { m_encoder->releaseSurface(surface); return; }
    void *replaced = nullptr;
    {
        std::lock_guard<std::mutex> lk(m_latestMutex);
        replaced = m_latest;
        m_latest = surface;
        m_latestDesc = desc;
    }
    m_framesIn.fetch_add(1, std::memory_order_relaxed);
    if (replaced) {
        // Two pictures inside one clock period: the newer wins.
        m_encoder->releaseSurface(replaced);
        m_framesSkipped.fetch_add(1, std::memory_order_relaxed);
    }
    m_latestCv.notify_one();
}

void StreamSink::release(void *surface)
{
    if (!surface) return;
    m_inFlight.fetch_sub(1, std::memory_order_acq_rel);
    m_encoder->releaseSurface(surface);
}

// ---- the paced clock -----------------------------------------------------------------

void StreamSink::pacedThread()
{
    // Wait for the first picture: the clock starts with it, at the source
    // rate the bus has told us by then.
    {
        std::unique_lock<std::mutex> lk(m_latestMutex);
        m_latestCv.wait(lk, [this] { return m_latest != nullptr || !m_running.load(std::memory_order_acquire); });
    }
    if (!m_running.load(std::memory_order_acquire)) return;

    uint32_t num = m_cfg.fpsNum, den = m_cfg.fpsDen;
    if (!num || !den) { num = m_srcFpsNum.load(); den = m_srcFpsDen.load(); }
    if (!num || !den) { num = 30; den = 1; }
    const double fps = double(num) / double(den);
    m_fps.store(fps, std::memory_order_relaxed);

    EncoderSpec spec;
    spec.width = m_cfg.width;  spec.height = m_cfg.height;
    spec.fpsNum = num;  spec.fpsDen = den;
    spec.bitrateKbps = m_cfg.bitrateKbps;
    spec.gopFrames   = static_cast<uint32_t>(std::lround(fps));
    spec.profile     = m_cfg.profile;
    std::string err;
    setState("opening the encoder");
    const bool encOk = m_encoder->open(spec, [this](EncodedUnit &&au) {
        std::lock_guard<std::mutex> lk(m_auMutex);
        if (m_aus.size() >= kMaxQueuedUnits) {
            // The network is behind: drop the oldest non-key unit run.
            m_aus.pop_front();
            m_framesDroppedQueue.fetch_add(1, std::memory_order_relaxed);
        }
        m_aus.push_back(std::move(au));
        m_auCv.notify_one();
    }, &err);
    if (!encOk) { setState("error: " + err); m_running.store(false); return; }

    MuxConfig mc;
    mc.url = m_cfg.url;  mc.width = m_cfg.width;  mc.height = m_cfg.height;
    mc.fpsNum = num;  mc.fpsDen = den;
    mc.srtLatencyMs = m_cfg.srtLatencyMs;
    mc.streamId = m_cfg.streamId;  mc.passphrase = m_cfg.passphrase;
    setState("connecting to " + m_cfg.url);
    if (!m_muxer.open(mc, &err)) {
        setState("error: " + err);
        m_encoder->close();
        m_running.store(false);
        return;
    }
    setState("streaming");

    const int64_t periodNs = static_cast<int64_t>(1000000000.0 * den / num + 0.5);
    const int64_t dur90k   = static_cast<int64_t>(90000.0 * den / num + 0.5);
    const auto t0 = Clock::now();
    uint64_t tick = 0;
    void *last = nullptr;
    GpuFrameDesc lastDesc {};
    bool forceKey = true;
    uint64_t lastBytes = 0;
    auto lastRate = t0;
    uint64_t encodedAtLastLog = 0;

    while (m_running.load(std::memory_order_acquire)) {
        const auto deadline = t0 + std::chrono::nanoseconds(periodNs * static_cast<int64_t>(tick));
        {
            std::unique_lock<std::mutex> lk(m_latestMutex);
            m_latestCv.wait_until(lk, deadline, [this] { return !m_running.load(std::memory_order_acquire); });
            if (!m_running.load(std::memory_order_acquire)) break;
            if (m_latest) {
                if (last) m_encoder->releaseSurface(last);
                last = m_latest;  lastDesc = m_latestDesc;
                m_latest = nullptr;
            } else if (last) {
                m_framesRepeated.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (last) {
            const int64_t pts = static_cast<int64_t>(tick) * dur90k;
            m_encoder->encode(last, pts, dur90k, forceKey);
            forceKey = false;
        }
        // Behind by more than two periods (a stall, a sleep): skip ticks
        // rather than burst — the stream's clock is wall time.
        const auto now = Clock::now();
        const int64_t behind = std::chrono::duration_cast<std::chrono::nanoseconds>(now - deadline).count();
        tick += behind > 2 * periodNs ? static_cast<uint64_t>(behind / periodNs) : 1;

        if (now - lastRate >= std::chrono::seconds(1)) {
            const auto ms = m_muxer.stats();
            const auto es = m_encoder->stats();
            const double secs = std::chrono::duration<double>(now - lastRate).count();
            m_kbps.store((ms.bytes - lastBytes) * 8.0 / 1000.0 / secs, std::memory_order_relaxed);
            lastBytes = ms.bytes;
            lastRate  = now;
            qInfo("StreamSink: %llu in, %llu encoded (+%llu), %llu key, %llu repeated, %llu skipped, %llu dropped, "
                  "%llu pkts, %.0f kbps, %d in flight, mux errors %llu",
                  static_cast<unsigned long long>(m_framesIn.load()),
                  static_cast<unsigned long long>(es.encoded),
                  static_cast<unsigned long long>(es.encoded - encodedAtLastLog),
                  static_cast<unsigned long long>(es.keyframes),
                  static_cast<unsigned long long>(m_framesRepeated.load()),
                  static_cast<unsigned long long>(m_framesSkipped.load()),
                  static_cast<unsigned long long>(m_framesDroppedQueue.load()),
                  static_cast<unsigned long long>(ms.packets), m_kbps.load(), m_inFlight.load(),
                  static_cast<unsigned long long>(ms.errors));
            encodedAtLastLog = es.encoded;
        }
    }
    m_encoder->close();   // drains the callbacks into the queue
    if (last) m_encoder->releaseSurface(last);
}

// ---- the muxer ---------------------------------------------------------------------

void StreamSink::muxThread()
{
    for (;;) {
        EncodedUnit au;
        {
            std::unique_lock<std::mutex> lk(m_auMutex);
            m_auCv.wait(lk, [this] { return !m_aus.empty() || m_stopping.load(std::memory_order_acquire); });
            if (m_aus.empty()) break;            // stopping, and the paced thread has already closed the encoder
            au = std::move(m_aus.front());
            m_aus.pop_front();
        }
        if (m_muxer.isOpen()) m_muxer.writeVideo(au.data.data(), au.data.size(), au.pts90k, au.dur90k, au.keyframe);
    }
    m_muxer.close();
}

StreamSink::Status StreamSink::status() const
{
    Status s;
    { std::lock_guard<std::mutex> lk(m_stateMutex); s.state = m_state; }
    s.encoder = m_encoder ? m_encoder->name() : "none";
    s.framesIn = m_framesIn.load();
    s.framesRepeated = m_framesRepeated.load();
    s.framesSkipped = m_framesSkipped.load();
    if (m_encoder) { const auto es = m_encoder->stats(); s.framesEncoded = es.encoded; s.keyframes = es.keyframes; }
    const auto ms = m_muxer.stats();
    s.packets = ms.packets;  s.bytes = ms.bytes;
    s.fps = m_fps.load();  s.kbps = m_kbps.load();
    return s;
}

} // namespace qcv::output
