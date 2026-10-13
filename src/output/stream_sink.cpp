#include "stream_sink.h"

#include "decode/qcbae/output_ring.h"

#include <QtLogging>

#include <chrono>
#include <cmath>
#include <cstring>

namespace qcv::output {

namespace {
using Clock = std::chrono::steady_clock;
int64_t nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(); }
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
    if (cfg.audioKbps > 0 && !m_aac.open(cfg.audioKbps, error)) {
        m_encoder->releaseSurfaces();
        return false;
    }
    m_framesIn = 0; m_framesRepeated = 0; m_framesSkipped = 0; m_framesDroppedQueue = 0;
    m_audioFrames = 0; m_audioSilent = 0;
    m_fps = 0.0; m_kbps = 0.0;
    m_reconnects = 0;
    m_forceKey.store(false, std::memory_order_release);
    m_waitKey.store(false, std::memory_order_release);
    { std::lock_guard<std::mutex> lk(m_extraMutex); m_videoExtradata.clear(); }
    m_clockT0.store(0, std::memory_order_release);
    m_muxReady.store(false, std::memory_order_release);
    m_audioRing.clear();
    m_failed.store(false, std::memory_order_release);
    m_stopping.store(false, std::memory_order_release);
    m_running.store(true, std::memory_order_release);
    setState("waiting for the first frame");
    m_mux   = std::thread([this] { muxThread(); });
    if (cfg.audioKbps > 0) m_audio = std::thread([this] { audioThread(); });
    m_paced = std::thread([this] { pacedThread(); });
    return true;
}

void StreamSink::stop()
{
    if (!m_running.load(std::memory_order_acquire) && !m_paced.joinable() && !m_mux.joinable() && !m_audio.joinable()) return;
    // Order (plan A, "Threading"): the renderer's target is already gone
    // from the pass (the bus pushed the targets first) — surfaces still
    // out are GPU completions in flight.
    m_running.store(false, std::memory_order_release);
    m_latestCv.notify_all();
    m_muxer.interrupt();
    if (m_paced.joinable()) m_paced.join();      // closes the encoder: its callbacks drain into the queue
    if (m_audio.joinable()) m_audio.join();
    m_stopping.store(true, std::memory_order_release);   // nothing arrives after this
    m_auCv.notify_all();
    if (m_mux.joinable()) m_mux.join();          // writes what is queued, then the trailer
    m_aac.close();
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
    setState(m_failed.load() ? m_state : "stopped");
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

// ---- audio in ------------------------------------------------------------------------

void StreamSink::pushAudio(const float *stereo, uint32_t frames, bool muted)
{
    if (!m_running.load(std::memory_order_acquire) || m_cfg.audioKbps == 0 || frames == 0) return;
    // One producer at a time; a second one (a dual mixer and a player
    // both running for an instant) drops rather than corrupts.
    bool expected = false;
    if (!m_audioBusy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) return;
    if (muted || !stereo) {
        static const float zeros[512 * 2] = {};
        uint32_t done = 0;
        while (done < frames) {
            const uint32_t n = std::min<uint32_t>(512, frames - done);
            m_audioRing.write(zeros, n * 2 * sizeof(float));
            done += n;
        }
    } else {
        m_audioRing.write(stereo, static_cast<size_t>(frames) * 2 * sizeof(float));
    }
    m_audioBusy.store(false, std::memory_order_release);
}

// ---- the queue to the muxer -----------------------------------------------------------

void StreamSink::enqueue(EncodedUnit &&u)
{
    std::lock_guard<std::mutex> lk(m_auMutex);
    if (m_aus.size() >= kMaxQueuedUnits) {
        // The network is behind: drop the oldest.
        m_aus.pop_front();
        m_framesDroppedQueue.fetch_add(1, std::memory_order_relaxed);
    }
    m_aus.push_back(std::move(u));
    m_auCv.notify_one();
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
    const bool encOk = m_encoder->open(spec, [this](EncodedUnit &&au) { enqueue(std::move(au)); }, &err);
    if (!encOk) { setState("error: " + err); m_failed.store(true); m_running.store(false); return; }

    {
        MuxConfig mc;
        mc.url = m_cfg.url;  mc.width = m_cfg.width;  mc.height = m_cfg.height;
        mc.fpsNum = num;  mc.fpsDen = den;
        mc.srtLatencyMs = m_cfg.srtLatencyMs;
        mc.streamId = m_cfg.streamId;  mc.passphrase = m_cfg.passphrase;
        if (m_aac.isOpen()) { mc.audioExtradata = m_aac.extradata(); mc.audioKbps = m_cfg.audioKbps; }
        m_muxCfg = mc;
    }
    // The first connect retries like a reconnect would (a server that is
    // not up yet, a room still opening); Stop ends it.
    if (!connectWithBackoff("connecting")) {
        m_encoder->close();
        return;
    }
    m_muxReady.store(true, std::memory_order_release);
    setState("streaming");

    const int64_t periodNs = static_cast<int64_t>(1000000000.0 * den / num + 0.5);
    const int64_t dur90k   = static_cast<int64_t>(90000.0 * den / num + 0.5);
    const auto t0 = Clock::now();
    m_clockT0.store(std::chrono::duration_cast<std::chrono::nanoseconds>(t0.time_since_epoch()).count(),
                    std::memory_order_release);
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
            const bool key = forceKey || m_forceKey.exchange(false, std::memory_order_acq_rel);
            m_encoder->encode(last, pts, dur90k, key);
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
                  "audio %llu (+%llu silent), %llu pkts, %.0f kbps, %d in flight, mux errors %llu, reconnects %llu",
                  static_cast<unsigned long long>(m_framesIn.load()),
                  static_cast<unsigned long long>(es.encoded),
                  static_cast<unsigned long long>(es.encoded - encodedAtLastLog),
                  static_cast<unsigned long long>(es.keyframes),
                  static_cast<unsigned long long>(m_framesRepeated.load()),
                  static_cast<unsigned long long>(m_framesSkipped.load()),
                  static_cast<unsigned long long>(m_framesDroppedQueue.load()),
                  static_cast<unsigned long long>(m_audioFrames.load()),
                  static_cast<unsigned long long>(m_audioSilent.load()),
                  static_cast<unsigned long long>(ms.packets), m_kbps.load(), m_inFlight.load(),
                  static_cast<unsigned long long>(ms.errors),
                  static_cast<unsigned long long>(m_reconnects.load()));
            encodedAtLastLog = es.encoded;
        }
    }
    m_encoder->close();   // drains the callbacks into the queue
    if (last) m_encoder->releaseSurface(last);
}

// ---- audio: continuous AAC from the tap, silence where it is idle ------------------------

void StreamSink::audioThread()
{
    constexpr uint32_t N = AacEncoder::kFrameSamples;
    constexpr size_t   frameBytes = N * 2 * sizeof(float);
    // The clock starts with the first picture; audio before it is stale.
    while (m_running.load(std::memory_order_acquire) && m_clockT0.load(std::memory_order_acquire) == 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!m_running.load(std::memory_order_acquire)) return;
    m_audioRing.discardAll();
    const int64_t t0 = m_clockT0.load(std::memory_order_acquire);
    // A picture waits up to one clock period for its tick, audio does not:
    // stamp audio one period later so the two meet in the middle (measured
    // one frame of audio lead at 25p without it).
    const double fps = m_fps.load(std::memory_order_relaxed);
    const int64_t ptsOffset = fps > 0.0 ? static_cast<int64_t>(48000.0 / fps + 0.5) : 0;

    float interleaved[N * 2];
    float left[N], right[N];
    int64_t emitted = 0;                       // samples sent, = the next pts
    // The tap runs ~22 ms ahead of what the device plays; a frame of
    // slack keeps the real audio flowing without inserting silence
    // between device callbacks.
    const int64_t slackSamples = 48000 * 80 / 1000;
    std::vector<EncodedUnit> out;
    while (m_running.load(std::memory_order_acquire)) {
        const int64_t elapsedSamples = (nowNs() - t0) * 48000 / 1000000000LL;
        const size_t avail = m_audioRing.availableRead();
        bool silent = false;
        if (avail >= frameBytes) {
            // The device ran ahead of wall time (a burst): keep ~150 ms,
            // drop the rest so the stream does not lag.
            if (avail > frameBytes * 8) {
                size_t drop = avail - frameBytes * 7;
                drop -= drop % frameBytes;
                char junk[4096];
                while (drop > 0) { const size_t n = std::min(drop, sizeof junk); m_audioRing.read(junk, n); drop -= n; }
            }
            m_audioRing.read(interleaved, frameBytes);
        } else if (emitted + static_cast<int64_t>(N) <= elapsedSamples - slackSamples) {
            // Nothing from the tap and the stream is due: silence keeps
            // the audio continuous (the interleaver waits for it).
            std::memset(interleaved, 0, sizeof interleaved);
            silent = true;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(3));
            continue;
        }
        for (uint32_t i = 0; i < N; ++i) { left[i] = interleaved[i * 2]; right[i] = interleaved[i * 2 + 1]; }
        out.clear();
        if (m_aac.encode(left, right, emitted + ptsOffset, &out)) {
            if (m_muxReady.load(std::memory_order_acquire)) {
                for (auto &u : out) enqueue(std::move(u));
            }
            m_audioFrames.fetch_add(1, std::memory_order_relaxed);
            if (silent) m_audioSilent.fetch_add(1, std::memory_order_relaxed);
        }
        emitted += N;
    }
}

// ---- the muxer ---------------------------------------------------------------------

bool StreamSink::sleepUnlessStopping(int ms)
{
    for (int t = 0; t < ms; t += 50) {
        if (!m_running.load(std::memory_order_acquire) || m_stopping.load(std::memory_order_acquire)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return true;
}

bool StreamSink::connectWithBackoff(const char *why)
{
    int attempt = 0;
    int delayMs = 1000;
    for (;;) {
        if (!m_running.load(std::memory_order_acquire) || m_stopping.load(std::memory_order_acquire)) return false;
        ++attempt;
        setState(attempt == 1 ? std::string(why) + " to " + m_cfg.url
                              : std::string(why) + " (attempt " + std::to_string(attempt) + ")");
        MuxConfig mc = m_muxCfg;
        { std::lock_guard<std::mutex> lk(m_extraMutex); mc.videoExtradata = m_videoExtradata; }
        std::string err;
        if (m_muxer.open(mc, &err)) return true;
        if (!m_running.load(std::memory_order_acquire)) return false;
        setState(std::string(why) + " failed: " + err + " — retrying in " + std::to_string(delayMs / 1000) + " s");
        if (!sleepUnlessStopping(delayMs)) return false;
        delayMs = std::min(delayMs * 2, 10000);
    }
}

void StreamSink::muxThread()
{
    for (;;) {
        EncodedUnit au;
        {
            std::unique_lock<std::mutex> lk(m_auMutex);
            m_auCv.wait(lk, [this] { return !m_aus.empty() || m_stopping.load(std::memory_order_acquire); });
            if (m_aus.empty()) break;            // stopping, and the encoders are already closed
            au = std::move(m_aus.front());
            m_aus.pop_front();
        }
        if (!m_muxer.isOpen()) continue;
        if (!au.audio && au.keyframe) {
            // The newest parameter sets: an FLV reconnect writes its header from them.
            std::vector<uint8_t> ps;
            if (StreamMuxer::parameterSets(au.data.data(), au.data.size(), &ps)) {
                std::lock_guard<std::mutex> lk(m_extraMutex);
                m_videoExtradata = std::move(ps);
            }
        }
        if (m_waitKey.load(std::memory_order_acquire)) {
            // After a reconnect the receiver starts at a keyframe.
            if (au.audio || !au.keyframe) continue;
            m_waitKey.store(false, std::memory_order_release);
        }
        const bool ok = au.audio
            ? m_muxer.writeAudio(au.data.data(), au.data.size(), au.pts, au.dur)
            : m_muxer.writeVideo(au.data.data(), au.data.size(), au.pts, au.dur, au.keyframe);
        if (ok || !m_running.load(std::memory_order_acquire)) continue;

        // A write failed: the connection is gone. Reconnect with backoff;
        // units arriving meanwhile queue (bounded) and are dropped until
        // the keyframe the paced thread is asked for.
        qWarning("StreamSink: send failed (%s) — reconnecting", m_muxer.lastError().c_str());
        m_muxer.close();
        { std::lock_guard<std::mutex> lk(m_auMutex); m_aus.clear(); }
        setState("reconnecting (" + std::to_string(m_reconnects.load() + 1) + ")");
        if (!sleepUnlessStopping(1000)) break;
        if (!connectWithBackoff("reconnecting")) break;
        m_reconnects.fetch_add(1, std::memory_order_relaxed);
        m_waitKey.store(true, std::memory_order_release);
        m_forceKey.store(true, std::memory_order_release);
        { std::lock_guard<std::mutex> lk(m_auMutex); m_aus.clear(); }
        setState("streaming");
        qInfo("StreamSink: reconnected (%llu)", static_cast<unsigned long long>(m_reconnects.load()));
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
    s.framesDropped = m_framesDroppedQueue.load();
    if (m_encoder) { const auto es = m_encoder->stats(); s.framesEncoded = es.encoded; s.keyframes = es.keyframes; }
    s.audioFrames = m_audioFrames.load();
    s.audioSilent = m_audioSilent.load();
    const auto ms = m_muxer.stats();
    s.packets = ms.packets;  s.bytes = ms.bytes;  s.muxErrors = ms.errors;
    s.fps = m_fps.load();  s.kbps = m_kbps.load();
    s.reconnects = m_reconnects.load();
    return s;
}

} // namespace qcv::output
