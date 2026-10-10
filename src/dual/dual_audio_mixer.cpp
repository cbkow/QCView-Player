#include "dual_audio_mixer.h"

#include "audio/audio_decoder.h"
#include "audio/i_audio_source.h"
#include "audio/multi_stream_audio_decoder.h"
#include "audio/live_audio_source.h"
#include "decode/qcbae/host_bridge_url.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <QtGlobal>

#if defined(Q_OS_MACOS) || defined(__APPLE__)
#include "audio/coreaudio_device.h"
#define QCV_DUAL_HAS_AUDIO_DEVICE 1
#elif defined(Q_OS_WIN)
#include "audio/wasapi_audio_device.h"
#define QCV_DUAL_HAS_AUDIO_DEVICE 1
#else
#define QCV_DUAL_HAS_AUDIO_DEVICE 0
#endif

#include <QTimer>
#include <QtLogging>

#include <cmath>
#include <cstring>
#include <vector>

namespace qcv::dual {

namespace {
// Same probe as AudioPlayer's. Tiny duplication; factoring out into
// a shared helper would mean dragging libavformat into a header that
// many TUs include. Cheaper to keep two copies.
int probeAudioStreamCount(const QString &path)
{
    AVFormatContext *ctx = nullptr;
    const QByteArray pathUtf8 = path.toUtf8();
    if (avformat_open_input(&ctx, pathUtf8.constData(),
                              nullptr, nullptr) < 0) {
        return 0;
    }
    int count = 0;
    if (avformat_find_stream_info(ctx, nullptr) >= 0) {
        for (unsigned i = 0; i < ctx->nb_streams; ++i) {
            const AVStream *s = ctx->streams[i];
            if (s && s->codecpar
                && s->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                ++count;
            }
        }
    }
    avformat_close_input(&ctx);
    return count;
}

// Caller-supplied hint (>=1) bypasses the file-open probe; anything
// less falls back to probing. Same intent as AudioPlayer::open's
// hint param — reuse the metadata extractor's already-completed
// find_stream_info pass so we don't pay a second one per playlist
// boundary cross.
//
// `>= 1` not `>= 0` — a hint of 0 means "I don't know" (the
// default-initialized value on MediaItem.video.audioStreamCount
// before the extractor populates it). Treating 0 as a confident
// "no audio" dispatches every multi-stream broadcast master into
// single-stream AudioDecoder, which collapses the meter row to
// 2 unlabeled bars. See AudioPlayer::open for the same fix.
std::unique_ptr<IAudioSource> makeDecoderForPath(const QString &path,
                                                   QObject *parent,
                                                   int streamCountHint)
{
    if (path.isEmpty()) return nullptr;
    // A QCBridgeAE live side reads the audio segment beside its frame
    // ring; other live URLs (srt://) carry no audio here.
    if (hostbridge::isUrl(path)) return std::make_unique<LiveAudioSource>();
    const int streamCount = (streamCountHint >= 1)
                            ? streamCountHint
                            : probeAudioStreamCount(path);
    if (streamCount >= 2) {
        return std::make_unique<MultiStreamAudioDecoder>(parent);
    }
    return std::make_unique<AudioDecoder>(parent);
}
} // namespace

DualAudioMixer::DualAudioMixer(QObject *parent)
    : QObject(parent)
{
    // Decoders constructed lazily in open() once we know each side's
    // stream count (single-stream vs multi-stream pick is per-side).
#if defined(Q_OS_MACOS) || defined(__APPLE__)
    m_device = std::make_unique<CoreAudioDevice>();
#elif defined(Q_OS_WIN)
    m_device = std::make_unique<WasapiAudioDevice>();
#endif
}

DualAudioMixer::~DualAudioMixer() { shutdown(); }

bool DualAudioMixer::initialize()
{
    if (m_initialized) return true;
#if defined(Q_OS_MACOS) || defined(__APPLE__)
    CoreAudioDeviceConfig cfg;
    cfg.dataCallback = &DualAudioMixer::dataCallback;
    cfg.userData     = this;
    cfg.sampleRate   = 48000;
    cfg.channels     = 2;
    cfg.bufferSizeMs = 10;
    if (!m_device || !m_device->initialize(cfg)) {
        qWarning("DualAudioMixer: device init failed");
        return false;
    }
#elif defined(Q_OS_WIN)
    WasapiAudioDeviceConfig cfg;
    cfg.dataCallback = &DualAudioMixer::dataCallback;
    cfg.userData     = this;
    cfg.sampleRate   = 48000;
    cfg.channels     = 2;
    cfg.bufferSizeMs = 10;
    if (!m_device || !m_device->initialize(cfg)) {
        qWarning("DualAudioMixer: device init failed");
        return false;
    }
#endif
    m_initialized = true;
    return true;
}

void DualAudioMixer::shutdown()
{
    if (!m_initialized) return;
    pause();
    close();
#if QCV_DUAL_HAS_AUDIO_DEVICE
    if (m_device) m_device->shutdown();
#endif
    m_initialized = false;
}

bool DualAudioMixer::open(const QString &pathA, const QString &pathB,
                            int audioStreamCountHintA,
                            int audioStreamCountHintB)
{
    if (!m_initialized) {
        qWarning("DualAudioMixer::open called before initialize");
        return false;
    }
    close();

    // Pick the right decoder shape per-side. A could be a single-
    // stream stereo file while B is an 8-stream broadcast master
    // (or vice versa) — they're independent.
    m_decoderA = makeDecoderForPath(pathA, this, audioStreamCountHintA);
    m_decoderB = makeDecoderForPath(pathB, this, audioStreamCountHintB);
    // Remembered for the shuttle engines' grain readers.
    m_pathA = pathA;
    m_pathB = pathB;

    bool any = false;
    if (m_decoderA && !pathA.isEmpty() && m_decoderA->open(pathA)) {
        m_decoderA->start();
        any = true;
    }
    if (m_decoderB && !pathB.isEmpty() && m_decoderB->open(pathB)) {
        m_decoderB->start();
        any = true;
    }
    emit hasAudioChanged();

#if QCV_DUAL_HAS_AUDIO_DEVICE
    // A live side plays whether or not the transport does: its picture
    // is free-running, so its audio runs with it. Start the device now
    // and keep it running (pause() leaves it alone while a side is live).
    if (anyLive() && m_device) {
        m_syncA.req.restart.store(true, std::memory_order_release);
        m_syncB.req.restart.store(true, std::memory_order_release);
        m_device->start();
    }
#endif

    qInfo("DualAudioMixer: opened — A=%s, B=%s",
          (m_decoderA && m_decoderA->hasAudio() ? "yes" : "no"),
          (m_decoderB && m_decoderB->hasAudio() ? "yes" : "no"));
    // Degenerate success: both paths empty / both lacking audio
    // streams is fine — the mixer renders silence.
    return any || (pathA.isEmpty() && pathB.isEmpty());
}

void DualAudioMixer::close()
{
    if (m_playing.load()) pause();
#if QCV_DUAL_HAS_AUDIO_DEVICE
    // Synchronous stop: the decoders are closed next and the callback
    // must not be mid-read when they are.
    if (m_device && !m_shuttleActive.load()) m_device->stop();
#endif
    endShuttle();
    if (m_decoderA) m_decoderA->close();
    if (m_decoderB) m_decoderB->close();
    m_pathA.clear();
    m_pathB.clear();
    emit hasAudioChanged();
}

bool DualAudioMixer::anyLive() const
{
    return (m_decoderA && m_decoderA->isLive()) || (m_decoderB && m_decoderB->isLive());
}

void DualAudioMixer::play()
{
    if (!hasAudioA() && !hasAudioB() && !anyLive()) return;
    if (m_playing.exchange(true)) return;
#if QCV_DUAL_HAS_AUDIO_DEVICE
    ++m_pauseGeneration;                 // cancels a deferred stop
    m_syncA.req.restart.store(true, std::memory_order_release);
    m_syncB.req.restart.store(true, std::memory_order_release);
    if (m_device) m_device->start();
#endif
}

void DualAudioMixer::pause()
{
    if (!m_playing.exchange(false)) return;
    m_hold.store(false, std::memory_order_release);
    m_syncA.req.skipFrames.store(0, std::memory_order_release);
    m_syncB.req.skipFrames.store(0, std::memory_order_release);
#if QCV_DUAL_HAS_AUDIO_DEVICE
    // The callback sees !m_playing, fades the block out, then silence;
    // stop the device once that has played.
    stopDeviceDeferred();
#endif
}

void DualAudioMixer::stopDeviceDeferred()
{
#if QCV_DUAL_HAS_AUDIO_DEVICE
    const int gen = ++m_pauseGeneration;
    QTimer::singleShot(30, this, [this, gen] {
        if (gen != m_pauseGeneration) return;
        if (m_playing.load() || m_shuttleActive.load()) return;
        if (anyLive()) return;   // a live side keeps the device running
        if (m_device) m_device->stop();
    });
#endif
}

void DualAudioMixer::setHold(bool on)
{
    m_hold.store(on, std::memory_order_release);
    if (on) {
        m_syncA.resetPending.store(true, std::memory_order_release);
        m_syncB.resetPending.store(true, std::memory_order_release);
    }
}

void DualAudioMixer::seek(double seconds)
{
    seekPerSide(seconds, seconds);
}

void DualAudioMixer::seekPerSide(double sourceSecondsA,
                                   double sourceSecondsB)
{
    // Negative = "in a timeline gap"; mute that side and skip the
    // codec seek (the audio decoder has nowhere meaningful to go).
    // Sync offset shifts the audio decoder BEHIND the video clock by
    // offsetMs — positive offset → audio plays later, the direction
    // that compensates for video display + pipeline lag. The
    // m_lastSeekPos{A,B} atomics keep the un-offset source time so
    // the gap-edge re-seek logic in updatePerSide compares apples to
    // apples with the master clock the controller passes in.
    const double offsetSec =
        m_syncOffsetMs.load(std::memory_order_relaxed) / 1000.0;

    if (m_decoderA && m_decoderA->hasAudio()) {
        if (sourceSecondsA >= 0.0) {
            m_decoderA->seek(sourceSecondsA - offsetSec);
            reanchorSide(m_syncA, sourceSecondsA - offsetSec);
            m_lastSeekPosA.store(sourceSecondsA);
        }
        m_inGapA.store(sourceSecondsA < 0.0);
    } else {
        m_inGapA.store(true);
    }
    if (m_decoderB && m_decoderB->hasAudio()) {
        if (sourceSecondsB >= 0.0) {
            m_decoderB->seek(sourceSecondsB - offsetSec);
            reanchorSide(m_syncB, sourceSecondsB - offsetSec);
            m_lastSeekPosB.store(sourceSecondsB);
        }
        m_inGapB.store(sourceSecondsB < 0.0);
    } else {
        m_inGapB.store(true);
    }
}

void DualAudioMixer::reanchorSide(SideSync &sync, double anchorSrcSec)
{
    // Order matters like AudioPlayer::seek: the decoder seek was
    // issued by the caller first (seekPending is up, so the render
    // callback stops consuming), then the anchor/counter reset here.
    sync.anchorSrcSec.store(anchorSrcSec, std::memory_order_relaxed);
    sync.srcFramesConsumed.store(0, std::memory_order_relaxed);
    sync.ratio.store(1.0f, std::memory_order_relaxed);
    sync.req.skipFrames.store(0, std::memory_order_release);
    sync.resetPending.store(true, std::memory_order_release);
}

void DualAudioMixer::setSyncOffsetMs(int ms)
{
    if (ms < -100) ms = -100;
    if (ms >  100) ms =  100;
    m_syncOffsetMs.store(ms, std::memory_order_relaxed);
}

void DualAudioMixer::updatePerSide(double sourceSecondsA,
                                     double sourceSecondsB)
{
    // Edit-aware sync. Called per pump tick by the controller.
    // Three responsibilities:
    //   1. Update gap flags so processAudio skips the right side
    //      when master is outside that track's clip range.
    //   2. Re-seek a side when it transitions gap → clip, so audio
    //      resumes at the new clip's source-in offset rather than
    //      where the gap left it.
    //   3. Run the per-side drift servo (see header) while inside
    //      a clip — this is dual's first continuous drift control;
    //      before it, sides relied on sharing a wall clock.
    constexpr double kSeekCooldownSeconds = 1.0;

    const double offsetSec =
        m_syncOffsetMs.load(std::memory_order_relaxed) / 1000.0;
    auto handle = [&](const char *tag, IAudioSource *dec, double newPos,
                      std::atomic<bool> &inGap,
                      std::atomic<double> &lastSeekPos,
                      SideSync &sync) {
        // A live side has no timeline: never a gap (the controller
        // passes -1 for it), never a seek, never a servo.
        if (dec && dec->isLive()) {
            inGap.store(!dec->hasAudio());
            return;
        }
        if (!dec || !dec->hasAudio()) {
            inGap.store(true);
            return;
        }
        const bool wasInGap = inGap.load();
        const bool nowInGap = (newPos < 0.0);
        if (nowInGap != wasInGap) inGap.store(nowInGap);
        if (nowInGap) {
            // No consumption while gapped — the position estimate is
            // frozen, so make the servo start fresh at the next clip.
            sync.resetPending.store(true, std::memory_order_release);
            return;
        }
        if (wasInGap) {
            // Gap → clip transition. Seek the decoder to the new
            // clip's source position so playback resumes at the
            // right offset. Cooldown is short (we don't expect
            // back-to-back gap crossings).
            if (dec->secondsSinceLastSeek() > kSeekCooldownSeconds) {
                dec->seek(newPos - offsetSec);
                reanchorSide(sync, newPos - offsetSec);
                lastSeekPos.store(newPos);
            }
            return;   // let the seek settle before servoing
        }
        servoSide(tag, dec, sync, newPos - offsetSec);
    };
    handle("A", m_decoderA.get(), sourceSecondsA, m_inGapA,
           m_lastSeekPosA, m_syncA);
    handle("B", m_decoderB.get(), sourceSecondsB, m_inGapB,
           m_lastSeekPosB, m_syncB);
}

void DualAudioMixer::servoSide(const char *tag, IAudioSource *dec,
                                 SideSync &sync, double targetSrcSec)
{
    // Per-side mirror of AudioPlayer::update's tiers — see that
    // function for the rationale on each constant. `targetSrcSec`
    // arrives already shifted into the decoder's (offset-applied)
    // source domain, matching the anchors reanchorSide stores.
    constexpr double kServoBandSeconds     = 0.100;
    constexpr double kSeekCooldownSeconds  = 1.0;
    constexpr double kDiscontinuitySeconds = 1.0;
    constexpr int    kOutOfBandTicksToSeek = 6;   // 60 Hz pump: ~100 ms

#if QCV_DUAL_HAS_AUDIO_DEVICE
    if (!m_device) return;

    // Held: consumption is frozen; the estimate is meaningless until
    // release, and the servo restarts clean then.
    if (m_hold.load(std::memory_order_acquire)) {
        sync.resetPending.store(true, std::memory_order_release);
        return;
    }
    if (sync.resetPending.exchange(false, std::memory_order_acquire)) {
        sync.servo.reset();
        sync.lastUpdateValid = false;
        sync.outOfBandTicks  = 0;
        sync.ratio.store(1.0f, std::memory_order_relaxed);
    }
    if (dec->seekPending()) {
        sync.lastUpdateValid = false;
        return;
    }
    // EOF tail: nothing to consume past stream end; freeze rather
    // than thrash seeks against EOF (e.g. B shorter than A).
    const double dur = dec->duration();
    if (dur > 0.0 && targetSrcSec > dur - 0.050) {
        sync.ratio.store(1.0f, std::memory_order_relaxed);
        sync.lastUpdateValid = false;
        return;
    }

    // Ring frames are output-domain; ×tempo maps back to source
    // seconds (constant within an anchor epoch — tempo changes
    // re-seek both sides).
    const double tempo = dec->tempo();
    const double ratioNow = static_cast<double>(
        sync.ratio.load(std::memory_order_relaxed));
    const double srcSecConsumed =
        static_cast<double>(
            sync.srcFramesConsumed.load(std::memory_order_relaxed))
        / static_cast<double>(m_device->sampleRate()) * tempo;
    const double latencySec =
        m_device->bufferLatencySeconds() * ratioNow * tempo;
    const double audioSrcPos =
        sync.anchorSrcSec.load(std::memory_order_relaxed)
        + srcSecConsumed - latencySec;

    const double drift    = targetSrcSec - audioSrcPos;
    const double absDrift = std::abs(drift);

    if (absDrift > kServoBandSeconds) {
        const bool discontinuity = absDrift > kDiscontinuitySeconds;
        const bool sustained = ++sync.outOfBandTicks >= kOutOfBandTicksToSeek;
        if (discontinuity
            || (sustained
                && dec->secondsSinceLastSeek() > kSeekCooldownSeconds)) {
            qInfo("DualAudioMixer[%s]: drift %+0.0f ms — re-seeking to "
                  "%.2fs%s",
                  tag, drift * 1000.0, targetSrcSec,
                  discontinuity ? " (discontinuity, cooldown bypassed)"
                                : "");
            dec->seek(targetSrcSec);
            reanchorSide(sync, targetSrcSec);
        }
        return;
    }
    sync.outOfBandTicks = 0;

    const auto now = std::chrono::steady_clock::now();
    double dt = 0.0;
    if (sync.lastUpdateValid) {
        dt = std::chrono::duration<double>(now - sync.lastUpdate).count();
    }
    sync.lastUpdate      = now;
    sync.lastUpdateValid = true;

    const double ratio = sync.servo.update(drift, dt);
    sync.ratio.store(static_cast<float>(ratio), std::memory_order_relaxed);

    // Convergence trace ~every 10 s at the 60 Hz pump cadence.
    if (++sync.logCounter >= 600) {
        sync.logCounter = 0;
        qInfo("DualAudioMixer[%s]: servo drift %+.1f ms  ratio %.5f",
              tag, drift * 1000.0, ratio);
    }
#else
    Q_UNUSED(tag); Q_UNUSED(dec); Q_UNUSED(sync); Q_UNUSED(targetSrcSec);
#endif
}

bool DualAudioMixer::hasAudioA() const
{
    return m_decoderA && m_decoderA->hasAudio();
}

bool DualAudioMixer::hasAudioB() const
{
    return m_decoderB && m_decoderB->hasAudio();
}

void DualAudioMixer::setMutedA(bool m)
{
    if (m_mutedA.exchange(m) == m) return;
    emit mutedAChanged();
}

void DualAudioMixer::setMutedB(bool m)
{
    if (m_mutedB.exchange(m) == m) return;
    emit mutedBChanged();
}

void DualAudioMixer::setMasterVolume(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 2.0f) v = 2.0f;
    m_masterVolume.store(v, std::memory_order_relaxed);
}

void DualAudioMixer::setMasterMuted(bool m)
{
    m_masterMuted.store(m, std::memory_order_relaxed);
}

void DualAudioMixer::setTempoBoth(double tempo)
{
    if (m_decoderA) m_decoderA->setTempo(tempo);
    if (m_decoderB) m_decoderB->setTempo(tempo);
}

void DualAudioMixer::beginShuttle(double srcSecA, double srcSecB,
                                    double signedSpeed)
{
    if (!m_initialized) return;
    if (!m_shuttleA) m_shuttleA = std::make_unique<ShuttleAudioEngine>();
    if (!m_shuttleB) m_shuttleB = std::make_unique<ShuttleAudioEngine>();

    auto beginSide = [&](ShuttleAudioEngine &e, IAudioSource *dec,
                         const QString &path, double srcSec) {
        const bool usable = dec && dec->hasAudio() && !path.isEmpty();
        e.begin(usable ? path : QString(),
                std::max(0.0, srcSec), signedSpeed,
                dec ? dec->routingMode() : 0);
        e.setInGap(srcSec < 0.0);
    };
    beginSide(*m_shuttleA, m_decoderA.get(), m_pathA, srcSecA);
    beginSide(*m_shuttleB, m_decoderB.get(), m_pathB, srcSecB);
    m_shuttleActive.store(true);
#if QCV_DUAL_HAS_AUDIO_DEVICE
    // Dual pause() at gesture start stopped the device; the grain
    // rings need the callback running.
    if (m_device) m_device->start();
#endif
}

void DualAudioMixer::shuttleTargetPerSide(double srcSecA, double srcSecB,
                                            double signedSpeed)
{
    if (!m_shuttleActive.load()) return;
    auto tick = [&](ShuttleAudioEngine *e, const QString &path,
                    double srcSec) {
        if (!e) return;
        if (srcSec < 0.0) {
            // Gap: silence but keep the last real target so clip
            // re-entry snaps from something sensible.
            e->setInGap(true);
            return;
        }
        e->setInGap(false);
        e->updateTarget(path, srcSec, signedSpeed);
    };
    tick(m_shuttleA.get(), m_pathA, srcSecA);
    tick(m_shuttleB.get(), m_pathB, srcSecB);
}

void DualAudioMixer::endShuttle()
{
    if (!m_shuttleActive.exchange(false)) return;
    if (m_shuttleA) m_shuttleA->end();
    if (m_shuttleB) m_shuttleB->end();
#if QCV_DUAL_HAS_AUDIO_DEVICE
    if (m_device && !m_playing.load()) m_device->stop();
#endif
}

void DualAudioMixer::setRoutingModeA(int mode)
{
    if (m_decoderA) m_decoderA->setRoutingMode(mode);
}

void DualAudioMixer::setRoutingModeB(int mode)
{
    if (m_decoderB) m_decoderB->setRoutingMode(mode);
}

namespace {
QVariantList peaksFromDecoder(const IAudioSource *dec)
{
    QVariantList out;
    if (!dec) return out;
    const int nb = dec->sourceChannels();
    if (nb <= 0) return out;
    const auto peaks = dec->peakLevels();
    out.reserve(nb);
    for (int i = 0; i < nb && i < 16; ++i) out.append(peaks[i]);
    return out;
}
} // namespace

QVariantList DualAudioMixer::audioChannelPeaksA() const
{ return peaksFromDecoder(m_decoderA.get()); }
QVariantList DualAudioMixer::audioChannelPeaksB() const
{ return peaksFromDecoder(m_decoderB.get()); }
QStringList DualAudioMixer::audioChannelNamesA() const
{ return m_decoderA ? m_decoderA->sourceChannelNames() : QStringList(); }
QStringList DualAudioMixer::audioChannelNamesB() const
{ return m_decoderB ? m_decoderB->sourceChannelNames() : QStringList(); }

void DualAudioMixer::dataCallback(void * /*device*/, float *output,
                                    uint32_t frameCount, void *userData)
{
    auto *self = static_cast<DualAudioMixer *>(userData);
    self->processAudio(output, frameCount);
}

void DualAudioMixer::processAudio(float *output, uint32_t frameCount)
{
    const size_t samples = static_cast<size_t>(frameCount) * 2; // stereo
    std::memset(output, 0, samples * sizeof(float));

    // Shuttle preempts normal playback (which the gesture paused):
    // mix the grain engines, honoring per-side mutes, through the
    // same soft-limit shape as the normal path below.
    if (m_shuttleActive.load()) {
        static thread_local std::vector<float> shuttleBuf;
        if (shuttleBuf.size() < samples) shuttleBuf.resize(samples);
        auto mixEngine = [&](ShuttleAudioEngine *e, bool muted) {
            if (!e) return;
            // Muted sides still drain their grain ring — same rule
            // as the normal path's "muted sides still consume"
            // below. Skipping the read let the ring hit the 40 ms
            // back-pressure cap and stall the grain producer, so a
            // mid-shuttle unmute (possible during held-key FF/RW,
            // where the mouse is free to hit the A/B chip mutes)
            // played a stale burst from when the mute began. Drain
            // always; only the accumulate is gated on mute.
            e->read(shuttleBuf.data(), frameCount);
            if (muted) return;
            for (size_t i = 0; i < samples; ++i) {
                output[i] += shuttleBuf[i];
            }
        };
        mixEngine(m_shuttleA.get(), m_mutedA.load());
        mixEngine(m_shuttleB.get(), m_mutedB.load());
        // Master mute/volume — AFTER the engines drained (their
        // rings must keep consuming; see the muted-sides rule).
        if (m_masterMuted.load(std::memory_order_relaxed)) {
            std::memset(output, 0, samples * sizeof(float));
            return;
        }
        const float shuttleVol =
            m_masterVolume.load(std::memory_order_relaxed);
        auto softLimitShuttle = [](float x) noexcept -> float {
            constexpr float threshold = 0.8f;
            if (x >  threshold) {
                const float excess = x - threshold;
                return  threshold
                        + (1.0f - threshold) * (excess / (1.0f + excess));
            }
            if (x < -threshold) {
                const float excess = -x - threshold;
                return -threshold
                        - (1.0f - threshold) * (excess / (1.0f + excess));
            }
            return x;
        };
        for (size_t i = 0; i < samples; ++i) {
            output[i] = softLimitShuttle(output[i] * shuttleVol);
        }
        return;
    }

    // Not playing is a hold, not an early-out: the drain stage fades
    // the block out, then goes silent until pause()'s deferred device
    // stop. Likewise the post-seek hold and a timeline gap on one
    // side — every entry/exit is a fade, never a cut.
    const bool playing = m_playing.load();
    const bool held    = m_hold.load(std::memory_order_acquire);

    // Pull each side through its drain stage into a temp buffer, sum
    // into output. Both sides land at 48k stereo float32 (the
    // decoders resample on their decode threads), so direct
    // accumulation works. Buffers are thread_local and grow-once —
    // same idiom the pre-servo version used for sideBuf.
    static thread_local std::vector<float> sideBuf;
    static thread_local std::vector<float> srcBuf;
    if (sideBuf.size() < samples) sideBuf.resize(samples);
    // Scratch for the stage: ratio ≤ 1.015 plus interpolation margin.
    const size_t scratchFrames = static_cast<size_t>(frameCount) * 2 + 16;
    if (srcBuf.size() < scratchFrames * 2) srcBuf.resize(scratchFrames * 2);

    auto pullAndMix = [&](IAudioSource *dec, SideSync &sync,
                          bool muted, bool inGap) {
        if (!dec || !dec->hasAudio()) return;
        const double ratio = static_cast<double>(
            sync.ratio.load(std::memory_order_relaxed));
        // The stage counts only REAL post-seek frames (read() pads
        // underrun with silence; pre-seek audio is uncounted).
        const bool live = dec->isLive();
        sync.drain.process(*dec, sideBuf.data(), frameCount, ratio,
                           srcBuf.data(), scratchFrames,
                           sync.srcFramesConsumed, sync.req,
                           /*extraHold=*/live ? inGap : (!playing || held || inGap));
        // Muted sides still consume (position keeps tracking the
        // clock) — unmute plays current audio, not a stale buffer.
        if (muted) return;
        if (live && !m_livePlaydown.load(std::memory_order_acquire)) return;
        for (size_t i = 0; i < samples; ++i) {
            output[i] += sideBuf[i];
        }
    };
    pullAndMix(m_decoderA.get(), m_syncA, m_mutedA.load(), m_inGapA.load());
    pullAndMix(m_decoderB.get(), m_syncB, m_mutedB.load(), m_inGapB.load());

    // Master mute — AFTER consuming (mirrors AudioPlayer: the sides
    // keep advancing with the clock so unmute plays current audio).
    if (m_masterMuted.load(std::memory_order_relaxed)) {
        std::memset(output, 0, samples * sizeof(float));
        return;
    }

    // Soft limit on the sum so two unity-gain sources mixing to
    // ±2.0 don't hard-clip the device. Same shape as
    // AudioPlayer::processAudio; master volume applied inside.
    const float vol = m_masterVolume.load(std::memory_order_relaxed);
    auto softLimit = [](float x) noexcept -> float {
        constexpr float threshold = 0.8f;
        if (x >  threshold) {
            const float excess = x - threshold;
            return  threshold + (1.0f - threshold) * (excess / (1.0f + excess));
        }
        if (x < -threshold) {
            const float excess = -x - threshold;
            return -threshold - (1.0f - threshold) * (excess / (1.0f + excess));
        }
        return x;
    };
    for (size_t i = 0; i < samples; ++i) {
        output[i] = softLimit(output[i] * vol);
    }
}

} // namespace qcv::dual
