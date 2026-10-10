#include "live_audio_source.h"
#include "audio_routing_matrix.h"
#include "decode/qcbae/audio_ring.h"
#include "decode/qcbae/host_bridge_url.h"
#include "decode/qcbae/shared_ring.h"   // process_alive

#include <QtGlobal>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#if !defined(_WIN32)
#  include <sys/mman.h>
#endif

namespace qcv {

namespace {
constexpr int kWaitPollMs  = 250;   // segment absent or producer gone
constexpr int kPacketPollMs = 2;    // segment healthy: how often to look
constexpr int kIdlePollsBeforeMeterClear = 100 / kPacketPollMs;   // no packet for 100 ms: meters fall
// Room for about a second of stereo float at 48 kHz: far more than any
// depth target, so the only thing that ever trims is the target itself.
constexpr size_t kRingBytes = 48000 * 2 * sizeof(float);
}

std::atomic<int>  LiveAudioSource::s_bufferMs{60};
std::atomic<int>  LiveAudioSource::s_syncOffsetMs{0};
std::atomic<bool> LiveAudioSource::s_scrubMute{false};

void LiveAudioSource::setGlobalBufferMs(int ms)     { s_bufferMs.store(std::clamp(ms, 20, 200), std::memory_order_release); }
void LiveAudioSource::setGlobalSyncOffsetMs(int ms) { s_syncOffsetMs.store(std::clamp(ms, -200, 200), std::memory_order_release); }
void LiveAudioSource::setGlobalScrubMute(bool m)    { s_scrubMute.store(m, std::memory_order_release); }

LiveAudioSource::LiveAudioSource()
    : m_ring(kRingBytes)
{
    for (auto &p : m_peaks) p.store(0.0f, std::memory_order_relaxed);
}

LiveAudioSource::~LiveAudioSource()
{
    close();
}

bool LiveAudioSource::open(const QString &url)
{
    close();
    const QString ring = hostbridge::ringName(url);
    if (ring.isEmpty()) {
        qWarning("LiveAudioSource: '%s' is not a qcbae:// source", qPrintable(url));
        return false;
    }
    m_url = url;
    m_segmentName = (ring + QString::fromLatin1(qcbae::kAudioRingSuffix)).toStdString();
    m_stop.store(false, std::memory_order_release);
    m_hasAudio.store(false, std::memory_order_release);
    m_channels.store(0, std::memory_order_release);
    m_sampleRate.store(0, std::memory_order_release);
    m_ring.clear();   // no consumer yet: the device starts after open()
    m_thread = std::thread([this] { readerLoop(); });
    qInfo("LiveAudioSource: watching %s", m_segmentName.c_str());
    return true;
}

void LiveAudioSource::close()
{
    if (!m_thread.joinable()) return;
    m_stop.store(true, std::memory_order_release);
    { std::lock_guard<std::mutex> lk(m_sleepMutex); }
    m_sleepCv.notify_all();
    m_thread.join();
    m_running.store(false, std::memory_order_release);
    m_hasAudio.store(false, std::memory_order_release);
    for (auto &p : m_peaks) p.store(0.0f, std::memory_order_relaxed);
    qInfo("LiveAudioSource: closed (%s)", m_segmentName.c_str());
}

void LiveAudioSource::start() { m_running.store(true, std::memory_order_release); }
void LiveAudioSource::stop()  { m_running.store(false, std::memory_order_release); }

bool LiveAudioSource::hasAudio() const
{
    return m_hasAudio.load(std::memory_order_acquire);
}

std::size_t LiveAudioSource::read(float *output, std::size_t frameCount)
{
    const size_t want = frameCount * m_format.bytesPerFrame();
    const size_t got  = m_ring.read(output, want);
    if (got < want) std::memset(reinterpret_cast<uint8_t *>(output) + got, 0, want - got);
    return got / m_format.bytesPerFrame();
}

bool LiveAudioSource::interruptibleSleep(int ms)
{
    std::unique_lock<std::mutex> lk(m_sleepMutex);
    return !m_sleepCv.wait_for(lk, std::chrono::milliseconds(ms), [this] {
        return m_stop.load(std::memory_order_acquire);
    });
}

void LiveAudioSource::setRoutingMode(int mode)
{
    m_routingMode.store(mode, std::memory_order_release);   // the reader rebuilds its fold
}

// 2 x N, row-major [L row..., R row...]. computeRoutingMatrix answers
// nullopt for the layouts FFmpeg folds by default (1, 2 and 6 channels in
// Auto); there is no FFmpeg here, so those three are spelled out: mono
// to both sides, stereo through, 5.1 by the same BS.775 coefficients the
// Downmix5_1 mode uses (LFE dropped, as there).
void LiveAudioSource::rebuildFold(int channels)
{
    const int mode = m_routingMode.load(std::memory_order_acquire);
    if (channels == m_foldChannels && mode == m_foldMode) return;
    m_foldChannels = channels;
    m_foldMode     = mode;
    m_fold.assign(static_cast<size_t>(2 * channels), 0.0f);
    if (channels <= 0) return;
    if (const auto m = computeRoutingMatrix(mode, channels)) {
        for (size_t i = 0; i < m_fold.size() && i < m->size(); ++i)
            m_fold[i] = static_cast<float>((*m)[i]);
        return;
    }
    const size_t R = static_cast<size_t>(channels);   // start of the R row
    if (channels == 1) {
        m_fold[0] = 1.0f; m_fold[R] = 1.0f;
    } else if (channels == 2) {
        m_fold[0] = 1.0f; m_fold[R + 1] = 1.0f;
    } else if (channels == 6) {
        m_fold[0] = 1.0f; m_fold[2] = 0.707f; m_fold[4] = 0.707f;            // L = L + .707C + .707Ls
        m_fold[R + 1] = 1.0f; m_fold[R + 2] = 0.707f; m_fold[R + 5] = 0.707f; // R = R + .707C + .707Rs
    } else {
        m_fold[0] = 1.0f; m_fold[R + 1] = 1.0f;
    }
}

void LiveAudioSource::readerLoop()
{
    qcbae::AudioRing ring;
    uint64_t last = 0, dropped = 0, droppedForDepth = 0, packets = 0, generation = 0;
    int idlePolls = 0;
    qcbae::AudioSession session {};
    bool haveSession = false;
    uint32_t loggedPid = 0;

    std::vector<float> planes;        // max_channels * slot_frames, sized at open
    std::vector<float> stereo;        // interleaved fold output per packet
    const float *planePtrs[qcbae::kMaxAudioChannels] = {};

    while (!m_stop.load(std::memory_order_acquire)) {
        if (!ring.valid()) {
            if (!ring.open(m_segmentName)) {
                m_hasAudio.store(false, std::memory_order_release);
                if (!interruptibleSleep(kWaitPollMs)) break;
                continue;
            }
            const auto *h = ring.header();
            planes.assign(static_cast<size_t>(h->max_channels) * h->slot_frames, 0.0f);
            stereo.assign(static_cast<size_t>(h->slot_frames) * 2, 0.0f);
            ring.skip_to_latest(&last);   // live: history is not for playing
            generation = 0; haveSession = false;
            if (h->producer_pid != loggedPid) {
                loggedPid = h->producer_pid;
                qInfo("LiveAudioSource: segment %s open (producer pid %u, %u slots x %u frames)",
                      m_segmentName.c_str(), loggedPid, h->slot_count, h->slot_frames);
            }
        }

        if (!qcbae::process_alive(ring.header()->producer_pid)) {
            ring = qcbae::AudioRing();
#if !defined(_WIN32)
            // Same housekeeping as the frame ring: a dead producer's
            // segment stays in the kernel until someone unlinks it.
            if (::shm_unlink(m_segmentName.c_str()) == 0)
                qInfo("LiveAudioSource: removed the segment a dead producer left (%s)", m_segmentName.c_str());
#endif
            m_hasAudio.store(false, std::memory_order_release);
            if (!interruptibleSleep(kWaitPollMs)) break;
            continue;
        }
        if (ring.state() == qcbae::AudioState::Retired) {
            ring = qcbae::AudioRing();
            continue;
        }

        // A new session (every play, scrub and speed change on the host):
        // read its format, drop whatever was queued from the previous one.
        if (ring.read_session(&session, &generation)) {
            haveSession = true;
            m_channels.store(static_cast<int>(session.channels), std::memory_order_release);
            m_sampleRate.store(static_cast<int>(session.sample_rate), std::memory_order_release);
            m_hasAudio.store(session.channels > 0, std::memory_order_release);
            m_ring.markStale();
        }

        const int targetBytesPerPacket = static_cast<int>(ring.header()->slot_frames) * 8;
        const int depthMs = s_bufferMs.load(std::memory_order_acquire)
                          + std::max(0, s_syncOffsetMs.load(std::memory_order_acquire));
        const size_t targetBytes = static_cast<size_t>(depthMs) * 48000 / 1000 * 8;

        bool any = false;
        for (int n = 0; n < 64; ++n) {   // bounded: never starve the sleep
            qcbae::AudioPacketDesc d {};
            const auto r = ring.next_packet(&last, &d, planes.data(), &dropped);
            if (r == qcbae::AudioRing::Next::None) break;
            if (r == qcbae::AudioRing::Next::Resynced) { m_ring.markStale(); continue; }
            any = true;
            ++packets;
            const uint32_t slotFrames = ring.header()->slot_frames;
            const int ch = static_cast<int>(d.channels);

            // Peaks per source channel, pre-fold.
            for (int c = 0; c < 16; ++c) {
                float peak = 0.0f;
                if (c < ch) {
                    const float *p = planes.data() + static_cast<size_t>(c) * slotFrames;
                    for (uint32_t i = 0; i < d.frames; ++i) peak = std::max(peak, std::fabs(p[i]));
                }
                m_peaks[static_cast<size_t>(c)].store(peak, std::memory_order_relaxed);
            }
            if (ch != m_channels.load(std::memory_order_relaxed)) {
                m_channels.store(ch, std::memory_order_release);
                m_hasAudio.store(ch > 0, std::memory_order_release);
            }
            if (static_cast<int>(d.sample_rate) != m_sampleRate.load(std::memory_order_relaxed))
                m_sampleRate.store(static_cast<int>(d.sample_rate), std::memory_order_release);

            // Taps see the packet as the host sent it.
            {
                std::lock_guard<std::mutex> lk(m_tapMutex);
                if (!m_taps.empty()) {
                    for (int c = 0; c < ch; ++c) planePtrs[c] = planes.data() + static_cast<size_t>(c) * slotFrames;
                    for (auto &t : m_taps) t.second(d, planePtrs, session);
                }
            }

            // Depth control: past the target by two packets, this one is
            // not queued. (Drift is ppm; this fires rarely.)
            if (m_ring.freshBytes() > targetBytes + 2u * static_cast<size_t>(targetBytesPerPacket)) {
                ++droppedForDepth;
                continue;
            }

            rebuildFold(ch);
            const bool silent = (d.flags & qcbae::kAudioPacketScrubbing) && s_scrubMute.load(std::memory_order_acquire);
            if (silent || ch <= 0) {
                std::memset(stereo.data(), 0, static_cast<size_t>(d.frames) * 2 * sizeof(float));
            } else {
                const size_t R = static_cast<size_t>(ch);
                for (uint32_t i = 0; i < d.frames; ++i) {
                    float l = 0.0f, rr = 0.0f;
                    for (int c = 0; c < ch; ++c) {
                        const float v = planes[static_cast<size_t>(c) * slotFrames + i];
                        l  += m_fold[static_cast<size_t>(c)] * v;
                        rr += m_fold[R + static_cast<size_t>(c)] * v;
                    }
                    stereo[i * 2]     = l;
                    stereo[i * 2 + 1] = rr;
                }
            }
            // The device's format is 48 kHz. A host at another rate would
            // need a resampler here; Premiere's sequences are 48 kHz in
            // practice and the mismatch is logged once below.
            m_ring.write(stereo.data(), static_cast<size_t>(d.frames) * 2 * sizeof(float));
        }
        if (packets == 1 && any) {
            qInfo("LiveAudioSource: LIVE — %d ch %d Hz, depth target %d ms%s",
                  m_channels.load(), m_sampleRate.load(), depthMs,
                  m_sampleRate.load() != 48000 ? " (NOT 48 kHz: played at the device rate)" : "");
        }
        if (any && (packets % 2000) == 0) {
            qInfo("LiveAudioSource: %llu packets, resync-dropped %llu, depth-dropped %llu, queued %.0f ms",
                  (unsigned long long)packets, (unsigned long long)dropped, (unsigned long long)droppedForDepth,
                  1000.0 * static_cast<double>(m_ring.freshBytes()) / (48000.0 * 8.0));
        }
        // Meters: the host stopped (or paused between scrub pushes). The
        // peaks are per packet, so without this they would hold the last
        // packet's level for ever.
        if (any) {
            idlePolls = 0;
        } else if (idlePolls < kIdlePollsBeforeMeterClear && ++idlePolls == kIdlePollsBeforeMeterClear) {
            for (auto &pk : m_peaks) pk.store(0.0f, std::memory_order_relaxed);
        }
        if (!interruptibleSleep(kPacketPollMs)) break;
    }
}

QString LiveAudioSource::sourceChannelLayoutName() const
{
    switch (m_channels.load(std::memory_order_acquire)) {
        case 1:  return QStringLiteral("mono");
        case 2:  return QStringLiteral("stereo");
        case 6:  return QStringLiteral("5.1");
        case 8:  return QStringLiteral("7.1 / 5.1+stereo");
        default: return {};
    }
}

std::array<float, 16> LiveAudioSource::peakLevels() const
{
    std::array<float, 16> out {};
    for (size_t i = 0; i < 16; ++i) out[i] = m_peaks[i].load(std::memory_order_relaxed);
    return out;
}

QStringList LiveAudioSource::sourceChannelNames() const
{
    const int ch = m_channels.load(std::memory_order_acquire);
    QStringList names;
    if (ch == 1) return {QStringLiteral("M")};
    if (ch == 2) return {QStringLiteral("L"), QStringLiteral("R")};
    if (ch == 6) return {QStringLiteral("L"), QStringLiteral("R"), QStringLiteral("C"),
                         QStringLiteral("LFE"), QStringLiteral("Ls"), QStringLiteral("Rs")};
    for (int i = 1; i <= ch; ++i) names << QString::number(i);
    return names;
}

int LiveAudioSource::addTap(Tap tap)
{
    std::lock_guard<std::mutex> lk(m_tapMutex);
    const int id = m_nextTapId++;
    m_taps.emplace_back(id, std::move(tap));
    return id;
}

void LiveAudioSource::removeTap(int id)
{
    std::lock_guard<std::mutex> lk(m_tapMutex);
    m_taps.erase(std::remove_if(m_taps.begin(), m_taps.end(),
                                [id](const auto &t) { return t.first == id; }),
                 m_taps.end());
}

} // namespace qcv
