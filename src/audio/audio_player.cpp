#include "audio_player.h"
#include "audio_decoder.h"
#include "multi_stream_audio_decoder.h"
#include "live_audio_source.h"
#include "decode/qcbae/host_bridge_url.h"

#if defined(Q_OS_MACOS) || defined(__APPLE__)
#include "coreaudio_device.h"
#define QCV_HAS_AUDIO_DEVICE 1
#elif defined(Q_OS_WIN)
#include "wasapi_audio_device.h"
#define QCV_HAS_AUDIO_DEVICE 1
#else
#define QCV_HAS_AUDIO_DEVICE 0
#endif

extern "C" {
#include <libavformat/avformat.h>
}

#include <QFileInfo>
#include <QTimer>
#include <QtGlobal>
#include <QtLogging>
#include <cmath>
#include <cstring>

namespace qcv {

namespace {

// Quick probe: how many audio streams does this file have? Returns
// 0 if the file can't be opened. AudioPlayer dispatches on the
// result — single-stream sources keep the well-tested AudioDecoder
// path; multi-stream sources go to MultiStreamAudioDecoder. Open
// + find_stream_info is the same expensive call AudioDecoder /
// MultiStreamAudioDecoder will make again on the chosen instance,
// but that's the cost of dispatching at the right layer (and the
// alternative — peeking inside the constructed decoder — would
// require both classes to expose half-open intermediate state).
int probeAudioStreamCount(const QString &path)
{
    // TRACE_AUDIO_PROBE — a third concurrent avformat_open_input on
    // the same file when MediaItem has no hint yet (cold File >
    // Open). Visible alongside FFmpegMetadataExtractor + the actual
    // audio/video decoders to confirm interleaving.
    const QString trimmedName = QFileInfo(path).fileName();
    qInfo("probeAudioStreamCount: begin '%s'", qPrintable(trimmedName));
    AVFormatContext *ctx = nullptr;
    const QByteArray pathUtf8 = path.toUtf8();
    if (avformat_open_input(&ctx, pathUtf8.constData(),
                              nullptr, nullptr) < 0) {
        qInfo("probeAudioStreamCount: end (open failed) '%s'",
              qPrintable(trimmedName));
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
    qInfo("probeAudioStreamCount: end '%s' (count=%d)",
          qPrintable(trimmedName), count);
    return count;
}

} // namespace

AudioPlayer::AudioPlayer(QObject *parent)
    : QObject(parent)
{
    // m_decoder is constructed lazily in open() once we know whether
    // the file needs the single- or multi-stream class.
#if defined(Q_OS_MACOS) || defined(__APPLE__)
    m_device = std::make_unique<CoreAudioDevice>();
#elif defined(Q_OS_WIN)
    m_device = std::make_unique<WasapiAudioDevice>();
#endif
}

AudioPlayer::~AudioPlayer() { shutdown(); }

bool AudioPlayer::initialize()
{
    if (m_initialized) return true;
#if defined(Q_OS_MACOS) || defined(__APPLE__)
    CoreAudioDeviceConfig cfg;
    cfg.dataCallback = &AudioPlayer::dataCallback;
    cfg.userData     = this;
    cfg.sampleRate   = 48000;
    cfg.channels     = 2;
    cfg.bufferSizeMs = 10;
    if (!m_device || !m_device->initialize(cfg)) {
        qWarning("AudioPlayer: device init failed");
        return false;
    }
#elif defined(Q_OS_WIN)
    WasapiAudioDeviceConfig cfg;
    cfg.dataCallback = &AudioPlayer::dataCallback;
    cfg.userData     = this;
    cfg.sampleRate   = 48000;
    cfg.channels     = 2;
    cfg.bufferSizeMs = 10;
    if (!m_device || !m_device->initialize(cfg)) {
        qWarning("AudioPlayer: device init failed");
        return false;
    }
#endif
#if QCV_HAS_AUDIO_DEVICE
    // Servo scratch: sized for the device's real worst-case callback
    // (WASAPI's first fill can request the whole buffer) at the max
    // consumption ratio, plus interpolation margin. Never resized in
    // the render callback.
    if (m_device) {
        const std::size_t maxFrames =
            static_cast<std::size_t>(m_device->bufferFrameCount()) + 16;
        m_servoScratch.assign(maxFrames * 2 * 2, 0.0f);
    }
#endif
    m_initialized = true;
    return true;
}

void AudioPlayer::shutdown()
{
    if (!m_initialized) return;
    stop();
    close();
#if QCV_HAS_AUDIO_DEVICE
    if (m_device) m_device->shutdown();
#endif
    m_initialized = false;
}

bool AudioPlayer::open(const QString &path, int audioStreamCountHint)
{
    if (!m_initialized) {
        qWarning("AudioPlayer::open called before initialize");
        return false;
    }
    close();
    // A QCBridgeAE live item: the audio segment beside its frame ring.
    // Opens whether or not the host is pushing yet (the segment comes
    // and goes with the host's play state); hasAudio() follows it.
    if (hostbridge::isUrl(path)) {
        m_decoder = std::make_unique<LiveAudioSource>();
        if (!m_decoder->open(path)) {
            m_decoder.reset();
            emit hasAudioChanged();
            return false;
        }
        m_decoder->start();
        emit hasAudioChanged();
        return true;
    }
    // Pick the right decoder shape for this file's audio layout:
    //   - 0 or 1 audio streams → AudioDecoder (today's well-tested
    //     single-stream path; covers stereo MOVs, AAC files, etc.)
    //   - 2+ audio streams → MultiStreamAudioDecoder (libavfilter
    //     graph; covers broadcast multi-mono-track deliverables).
    // Dispatching here means AudioPlayer's transport / drift code
    // doesn't care which shape is in use.
    //
    // Prefer the caller-supplied hint over an in-process probe.
    // The metadata extractor already ran avformat_find_stream_info
    // on every bin item; reusing its result avoids a second
    // expensive index scan per playlist boundary cross on broadcast
    // multi-stream files. Probe is the cold-load fallback.
    //
    // `>= 1` not `>= 0` — a hint of 0 means "I don't know" (the
    // default-initialized value on MediaItem.video.audioStreamCount
    // before the extractor populates it, or on projects saved before
    // the field existed). Treating 0 as a confident "no audio"
    // dispatches every multi-stream broadcast master into single-
    // stream AudioDecoder, which picks ONE 2-channel bounce stream
    // via av_find_best_stream — losing the 5.1 / 7.1 layout and
    // collapsing the meter row to 2 unlabeled bars.
    const int streamCount = (audioStreamCountHint >= 1)
                            ? audioStreamCountHint
                            : probeAudioStreamCount(path);
    if (streamCount >= 2) {
        m_decoder = std::make_unique<MultiStreamAudioDecoder>(this);
    } else {
        m_decoder = std::make_unique<AudioDecoder>(this);
    }
    if (!m_decoder->open(path)) {
        emit hasAudioChanged();
        return false;
    }
    m_decoder->start();
    emit hasAudioChanged();
    return true;
}

void AudioPlayer::close()
{
    if (m_isPlaying.load()) stop();
#if QCV_HAS_AUDIO_DEVICE
    // Synchronous stop: open() replaces m_decoder next and the
    // callback must not be mid-read when it does. (pause() alone
    // defers the stop by a couple of blocks for its fade-out.)
    if (m_device && !shuttleActive()) m_device->stop();
#endif
    if (m_decoder) m_decoder->close();
    emit hasAudioChanged();
}

void AudioPlayer::play()
{
    // A live source starts the device before any audio has arrived:
    // the callback drains silence until the host pushes.
    if (!m_decoder || (!m_decoder->hasAudio() && !m_decoder->isLive())) return;
    if (m_isPlaying.exchange(true)) return;
#if QCV_HAS_AUDIO_DEVICE
    // No re-anchoring here: the playout-position anchor is owned by
    // seek() alone (consumption simply pauses with the device while
    // stopped, so the estimate stays valid across pause/play), and
    // WindowManager seeks before resuming playback anyway.
    //
    // Playout continuity restarts: the drain fades the first block
    // in and drops any stale pre-seek audio silently rather than
    // "fading out" something the listener never heard.
    ++m_pauseGeneration;                 // cancels a deferred stop
    m_drainReq.restart.store(true, std::memory_order_release);
    if (m_device) m_device->start();
#endif
    emit isPlayingChanged();
}

void AudioPlayer::pause()
{
    if (!m_isPlaying.exchange(false)) return;
    // A pause while held must not leave the hold armed for the next
    // play (which re-arms it explicitly when it wants one).
    m_drainReq.hold.store(false, std::memory_order_release);
    m_drainReq.skipFrames.store(0, std::memory_order_release);
#if QCV_HAS_AUDIO_DEVICE
    // The callback sees !m_isPlaying, fades the current block out and
    // then outputs silence; stop the device once that has played.
    stopDeviceDeferred();
#endif
    emit isPlayingChanged();
}

void AudioPlayer::stopDeviceDeferred()
{
#if QCV_HAS_AUDIO_DEVICE
    const int gen = ++m_pauseGeneration;
    QTimer::singleShot(30, this, [this, gen] {
        if (gen != m_pauseGeneration) return;         // play() came back
        if (m_isPlaying.load() || shuttleActive()) return;
        if (m_device) m_device->stop();
    });
#endif
}

void AudioPlayer::setHold(bool on)
{
    m_drainReq.hold.store(on, std::memory_order_release);
    if (on) m_lastServoUpdateValid = false;
}

void AudioPlayer::releaseHold(double masterSeconds)
{
    if (!m_drainReq.hold.load(std::memory_order_acquire)) return;
    if (!m_decoder || !m_decoder->hasAudio() || m_decoder->isLive()) {
        m_drainReq.hold.store(false, std::memory_order_release);
        return;
    }
#if QCV_HAS_AUDIO_DEVICE
    // Where did the master clock actually land relative to the audio
    // anchor? Forward by a little: drop that many ring frames (the
    // decode thread has ~100 ms queued, no seek, no refill gap).
    // Backward, or forward by a lot: re-seek (sample-accurate now).
    constexpr double kSkipMaxSeconds = 0.400;
    constexpr double kDeadbandSeconds = 0.004;
    const double offsetSec =
        m_syncOffsetMs.load(std::memory_order_relaxed) / 1000.0;
    const double tempo = m_decoder->tempo();
    const int sampleRate = m_device ? m_device->sampleRate() : 48000;
    const double consumedSec =
        static_cast<double>(m_srcFramesConsumed.load(std::memory_order_relaxed))
        / static_cast<double>(sampleRate) * tempo;
    const double delta = (masterSeconds - offsetSec)
                       - (m_anchorSrcSec + consumedSec);
    if (delta > kDeadbandSeconds && delta <= kSkipMaxSeconds) {
        m_drainReq.skipFrames.store(
            static_cast<uint32_t>(std::lround(delta / tempo * sampleRate)),
            std::memory_order_release);
        qInfo("AudioPlayer: hold released, skipping %+.0f ms to the "
              "landed frame", delta * 1000.0);
    } else if (delta < -kDeadbandSeconds || delta > kSkipMaxSeconds) {
        qInfo("AudioPlayer: hold released, re-seeking (%+.0f ms)",
              delta * 1000.0);
        seek(masterSeconds);
    } else {
        qInfo("AudioPlayer: hold released in place (%+.1f ms)",
              delta * 1000.0);
    }
#else
    Q_UNUSED(masterSeconds);
#endif
    m_drainReq.hold.store(false, std::memory_order_release);
    m_lastServoUpdateValid = false;
}

void AudioPlayer::stop() { pause(); }

void AudioPlayer::seek(double seconds)
{
    if (!m_decoder || !m_decoder->hasAudio()) return;
    if (m_decoder->isLive()) return;   // no timeline to seek in

    // Apply the user's A/V-sync offset at the decoder boundary: fetch
    // samples for `seconds - offset` so the audio CONTENT lags the
    // video clock by offsetMs — the right direction to compensate for
    // video display + pipeline lag. The anchor lands in the same
    // shifted (source-seconds) domain; update() compares against
    // `videoPos - offset` so both sides of the drift subtraction stay
    // in one clock domain.
    //
    // Order matters: the decoder seek FIRST (it raises seekPending,
    // which makes the render callback stop consuming pre-flush
    // frames), THEN the anchor/counter reset. A callback already
    // in flight when this runs can attribute at most one buffer
    // (~10 ms) of pre-seek consumption to the new anchor — the servo
    // absorbs that.
    const double offsetSec =
        m_syncOffsetMs.load(std::memory_order_relaxed) / 1000.0;
    m_decoder->seek(seconds - offsetSec);

    m_anchorSrcSec = seconds - offsetSec;
    m_srcFramesConsumed.store(0, std::memory_order_relaxed);
    m_drainReq.skipFrames.store(0, std::memory_order_release);  // superseded
    m_servo.reset();
    m_servoRatio.store(1.0f, std::memory_order_relaxed);
    m_lastServoUpdateValid = false;
    m_outOfBandTicks = 0;
}

void AudioPlayer::setSyncOffsetMs(int ms)
{
    if (ms < -100) ms = -100;
    if (ms >  100) ms =  100;
    m_syncOffsetMs.store(ms, std::memory_order_relaxed);
    // Caller (WindowManager) decides whether to immediately re-seek;
    // we don't pull a position out of thin air here.
}

void AudioPlayer::setVolume(float v)
{
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    if (qFuzzyCompare(m_volume.load(), v)) return;
    m_volume.store(v);
    emit volumeChanged();
}

void AudioPlayer::setMuted(bool m)
{
    if (m_muted.exchange(m) == m) return;
    emit mutedChanged();
}

void AudioPlayer::setRoutingMode(int mode)
{
    if (m_decoder) m_decoder->setRoutingMode(mode);
}

void AudioPlayer::setPlaybackTempo(double tempo)
{
    if (m_decoder) m_decoder->setTempo(tempo);
}

double AudioPlayer::playbackTempo() const
{
    return m_decoder ? m_decoder->tempo() : 1.0;
}

int AudioPlayer::routingMode() const
{
    return m_decoder ? m_decoder->routingMode() : 0;
}

void AudioPlayer::beginShuttle(const QString &path, double srcSec,
                               double signedSpeed, int routingMode)
{
    if (!m_initialized) return;
    if (!m_shuttle) m_shuttle = std::make_unique<ShuttleAudioEngine>();
    m_shuttle->begin(path, srcSec, signedSpeed, routingMode);
#if QCV_HAS_AUDIO_DEVICE
    // Normal playback is paused during the gesture (device stopped);
    // the grain ring needs the callback running.
    if (m_device) m_device->start();
#endif
}

void AudioPlayer::shuttleTarget(const QString &path, double srcSec,
                                double signedSpeed)
{
    if (m_shuttle) m_shuttle->updateTarget(path, srcSec, signedSpeed);
}

void AudioPlayer::endShuttle()
{
    if (m_shuttle) m_shuttle->end();
#if QCV_HAS_AUDIO_DEVICE
    // Keep the device only if normal playback is running (the commit
    // seek + play() at gesture release restarts it otherwise).
    if (m_device && !m_isPlaying.load()) m_device->stop();
#endif
}

bool AudioPlayer::shuttleActive() const
{
    return m_shuttle && m_shuttle->active();
}

QVariantList AudioPlayer::audioChannelPeaks() const
{
    QVariantList out;
    if (!m_decoder) return out;
    const int nb = m_decoder->sourceChannels();
    if (nb <= 0) return out;
    const auto peaks = m_decoder->peakLevels();
    out.reserve(nb);
    for (int i = 0; i < nb && i < 16; ++i) out.append(peaks[i]);
    return out;
}

QStringList AudioPlayer::audioChannelNames() const
{
    return m_decoder ? m_decoder->sourceChannelNames() : QStringList();
}

void AudioPlayer::update(double videoPositionSeconds)
{
    // Continuous sync servo, called per video frame (video mode) or
    // from the ~30 Hz pump (other modes). Three tiers by |drift|:
    //
    //   <= 100 ms servo band — trim the render callback's consumption
    //             ratio via the PI controller (±0.2 % inside 20 ms,
    //             opening to ±1.5 % at the band edge — see
    //             AudioSyncServo); drift converges with no seeks.
    //             The band is > 2 frame periods at 24p: one late
    //             video publish (one frame = 41.7 ms) must stay a
    //             servo matter, never a cut.
    //   band..1 s soft re-seek, only once the drift has stayed out of
    //             band for several consecutive ticks AND the 1 s
    //             cooldown has elapsed. The cut itself is faded by
    //             the drain stage and lands sample-accurately.
    //   > 1 s     discontinuity (loop wrap, external scrub): re-seek
    //             immediately, cooldown bypassed. Letting audio trail
    //             a full clip behind is the gap the user hears as
    //             "pause until the audio catches up."
    constexpr double kServoBandSeconds     = 0.100;
    constexpr double kSeekCooldownSeconds  = 1.0;
    constexpr double kDiscontinuitySeconds = 1.0;
    constexpr int    kOutOfBandTicksToSeek = 4;

    if (!m_decoder || !m_decoder->hasAudio()
        || !m_isPlaying.load()) return;
    // A live source plays what arrives; there is no position to servo.
    if (m_decoder->isLive()) return;
    // Held (post-seek, waiting for the picture): consumption is
    // frozen, so the estimate below is meaningless until release.
    if (m_drainReq.hold.load(std::memory_order_acquire)) return;

#if QCV_HAS_AUDIO_DEVICE
    if (!m_device) return;

    // A decoder seek is still in flight — the consumption counter is
    // frozen (callback outputs silence) and the estimate below would
    // be garbage. Wait for the flush to complete.
    if (m_decoder->seekPending()) {
        m_lastServoUpdateValid = false;
        return;
    }

    const double offsetSec =
        m_syncOffsetMs.load(std::memory_order_relaxed) / 1000.0;
    const double target = videoPositionSeconds - offsetSec;

    // EOF tail: past the end of the audio stream there is nothing to
    // consume — the position estimate freezes while the video clock
    // runs on, and correcting would just thrash seeks against EOF.
    // Freeze the servo instead; the next real seek (loop wrap, user
    // action) re-engages it.
    const double dur = m_decoder->duration();
    if (dur > 0.0 && target > dur - 0.050) {
        m_servoRatio.store(1.0f, std::memory_order_relaxed);
        m_lastServoUpdateValid = false;
        return;
    }

    // Playout position in source seconds: consumption at the ring
    // drain, minus what the device has buffered but not yet played.
    // Ring frames are output-domain; ×tempo maps them back to source
    // seconds (the tempo stage consumes `tempo` source seconds per
    // output second). Tempo changes re-anchor via seek, so the value
    // is constant within an anchor epoch.
    const int sampleRate = m_device->sampleRate();
    const double tempo = m_decoder->tempo();
    const double ratioNow =
        static_cast<double>(m_servoRatio.load(std::memory_order_relaxed));
    const double srcSecConsumed =
        static_cast<double>(m_srcFramesConsumed.load(std::memory_order_relaxed))
        / static_cast<double>(sampleRate) * tempo;
    const double latencySec =
        m_device->bufferLatencySeconds() * ratioNow * tempo;
    const double audioSrcPos = m_anchorSrcSec + srcSecConsumed - latencySec;

    const double drift    = target - audioSrcPos;
    const double absDrift = std::abs(drift);

    if (absDrift > kServoBandSeconds) {
        const bool discontinuity = absDrift > kDiscontinuitySeconds;
        const bool sustained = ++m_outOfBandTicks >= kOutOfBandTicksToSeek;
        if (discontinuity
            || (sustained
                && m_decoder->secondsSinceLastSeek() > kSeekCooldownSeconds)) {
            qInfo("AudioPlayer: drift %+0.0f ms — re-seeking audio to "
                  "%.2fs%s",
                  drift * 1000.0, videoPositionSeconds,
                  discontinuity ? " (discontinuity, cooldown bypassed)" : "");
            seek(videoPositionSeconds);
        }
        return;
    }
    m_outOfBandTicks = 0;

    // Servo band: dt-aware PI update, ratio published to the render
    // callback.
    const auto now = std::chrono::steady_clock::now();
    double dt = 0.0;
    if (m_lastServoUpdateValid) {
        dt = std::chrono::duration<double>(now - m_lastServoUpdate).count();
    }
    m_lastServoUpdate      = now;
    m_lastServoUpdateValid = true;

    const double ratio = m_servo.update(drift, dt);
    m_servoRatio.store(static_cast<float>(ratio), std::memory_order_relaxed);

    // Convergence trace, ~every 10 s at the 30 Hz pump cadence.
    // Expected steady state: |drift| < 5 ms, ratio within ±0.0005 of
    // 1. Cheap enough to keep in release builds for field diagnosis.
    if (++m_servoLogCounter >= 300) {
        m_servoLogCounter = 0;
        qInfo("AudioPlayer: servo drift %+.1f ms  ratio %.5f",
              drift * 1000.0, ratio);
    }
#else
    Q_UNUSED(videoPositionSeconds);
#endif
}

bool AudioPlayer::hasAudio() const
{
    return m_decoder && m_decoder->hasAudio();
}

double AudioPlayer::duration() const
{
    return m_decoder ? m_decoder->duration() : 0.0;
}

void AudioPlayer::dataCallback(void * /*device*/, float *output,
                                uint32_t frameCount, void *userData)
{
    auto *self = static_cast<AudioPlayer *>(userData);
    self->processAudio(output, frameCount);
}

void AudioPlayer::processAudio(float *output, uint32_t frameCount)
{
    const size_t outBytes = static_cast<size_t>(frameCount) * 2
                            * sizeof(float);
    // Shuttle mode preempts normal playback (which the gesture
    // paused): drain the grain ring, then fall through to the shared
    // mute/volume/soft-limit tail below.
    if (m_shuttle && m_shuttle->active()) {
        m_shuttle->read(output, frameCount);
        if (m_muted.load()) {
            std::memset(output, 0, outBytes);
            return;
        }
        const float vol = m_volume.load();
        const size_t n = static_cast<size_t>(frameCount) * 2;
        for (size_t i = 0; i < n; ++i) output[i] *= vol;
        return;
    }
    if (!m_decoder || !m_decoder->hasAudio()) {
        std::memset(output, 0, outBytes);
        return;
    }

    // Drain through the stage at the ratio update() published. The
    // stage owns the resampler, the seek-pending / stale-tail logic,
    // every fade, the hold and the skip request, and adds only REAL
    // post-seek frames to the consumption counter (silence padding
    // and pre-seek audio don't advance the stream, so they must not
    // advance the position estimate). Not playing = hold: the block
    // fades out, then silence until pause()'s deferred device stop.
    const double ratio =
        static_cast<double>(m_servoRatio.load(std::memory_order_relaxed));
    const bool live = m_decoder->isLive();
    m_drain.process(*m_decoder, output, frameCount, ratio,
                    m_servoScratch.data(), m_servoScratch.size() / 2,
                    m_srcFramesConsumed, m_drainReq,
                    /*extraHold=*/!m_isPlaying.load() && !live);

    // Live playdown off: drained (meters and taps ran), not heard.
    if (live && !m_livePlaydown.load(std::memory_order_acquire)) {
        std::memset(output, 0, outBytes);
        return;
    }

    // Muted AFTER consuming: the stream keeps advancing with the
    // clock, so unmute plays current audio, not a stale buffer.
    if (m_muted.load()) {
        std::memset(output, 0, outBytes);
        return;
    }

    // Soft limit: keep small signals linear, smoothly compress
    // anything above ±0.8 toward ±1.0 asymptote so multi-track
    // sums (Phase 5.x) and >1.0 user volumes don't hard-clip.
    // Same shape as the old app's AudioMixer / AudioPlayer.
    const float vol = m_volume.load();
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
    const size_t samples = static_cast<size_t>(frameCount) * 2; // stereo
    for (size_t i = 0; i < samples; ++i) {
        output[i] = softLimit(output[i] * vol);
    }
}

} // namespace qcv
