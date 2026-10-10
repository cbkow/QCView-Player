// LiveSource — the surface every live source shows the UI.
//
// A live source feeds a latest-wins publish slot via publishExternalFrame()
// (see live_stream_decoder.h for why) and exposes
// connection state and a few facts for LiveStrip and the LeftRail dot.
// WindowManager owns exactly one at a time and hands QML this base type,
// so QML is written once for every kind:
//   - LiveStreamDecoder  — srt:// and other FFmpeg network URLs
//   - HostBridgeSource   — qcbae:// After Effects / Premiere shared-memory
//                          frames from the QCBridgeAE Transmit device
//
// QML compares `status` numerically; the values are a wire contract with
// LiveStrip.qml and LeftRail.qml — append, never renumber.

#pragma once

#include "frame_handle.h"

#include <QObject>
#include <QString>
#include <cstdint>
#include <functional>

namespace qcv {

// Where a live source publishes its frames. The single view's VideoDecoder
// is one (its slot feeds the renderer); the dual island's DualLiveSource is
// the other. Both are latest-wins: a consumer that falls behind skips frames.
class LiveFrameSink
{
public:
    virtual ~LiveFrameSink() = default;
    virtual void publishExternalFrame(FrameHandle handle, int64_t pts) = 0;
};

class LiveSource : public QObject
{
    Q_OBJECT
    Q_PROPERTY(Status status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString statusDetail READ statusDetail NOTIFY statusChanged)
    Q_PROPERTY(QString url READ url CONSTANT)
    Q_PROPERTY(int width READ width NOTIFY metadataChanged)
    Q_PROPERTY(int height READ height NOTIFY metadataChanged)
    Q_PROPERTY(QString codecName READ codecName NOTIFY metadataChanged)
    Q_PROPERTY(QString pixelFormatName READ pixelFormatName NOTIFY metadataChanged)
    Q_PROPERTY(bool hasAudio READ hasAudio NOTIFY metadataChanged)
    Q_PROPERTY(int reconnectCount READ reconnectCount NOTIFY statusChanged)
    Q_PROPERTY(bool nonFinite READ nonFinite NOTIFY nonFiniteChanged)
    Q_PROPERTY(bool sharedMemory READ sharedMemory CONSTANT)
    // The feed's audio, as the QCBridgeAE audio segment reports it
    // (QCBridgeAE DESIGN-NOTES D6). Zero / NoAudioSegment for every
    // source that has none. Change with audioChanged.
    Q_PROPERTY(int audioChannels READ audioChannels NOTIFY audioChanged)
    Q_PROPERTY(int audioSampleRate READ audioSampleRate NOTIFY audioChanged)
    Q_PROPERTY(AudioState audioState READ audioState NOTIFY audioChanged)

public:
    enum Status : int {
        Idle         = 0,   // constructed / closed
        Connecting   = 1,   // first connection attempt in progress
        Live         = 2,   // frames flowing
        Reconnecting = 3,   // source gone; retrying
        Paused       = 4,   // source present but its host stopped sending
                            // (e.g. After Effects lost focus); last frame held
    };
    Q_ENUM(Status)

    // QML compares these numerically too — append, never renumber.
    enum AudioState : int {
        NoAudioSegment = 0,   // the host publishes no audio (After Effects, SRT, or not running)
        HostAudioOff   = 1,   // Premiere plays without pushing: "Audio Stream" is unticked for the device
        AudioIdle      = 2,   // segment present, host not playing
        AudioPushing   = 3,   // packets flowing
        AudioScrubbing = 4,   // packets flowing from a scrub
    };
    Q_ENUM(AudioState)

    using QObject::QObject;
    ~LiveSource() override = default;

    // Must be set before open(). The sink must outlive this object's
    // close() — the owner (WindowManager for single view, DualLiveSource in
    // dual) owns both and tears down in that order.
    virtual void setSink(LiveFrameSink *sink) = 0;

    // Invoked from the worker thread after every published frame, so the
    // D3D11 render-on-demand loop wakes (Metal free-runs).
    virtual void setFrameCallback(std::function<void()> cb) = 0;

    virtual bool open(const QString &url) = 0;
    virtual void close() = 0;   // joins the worker; nothing publishes after it returns

    virtual Status  status() const = 0;
    // One human line qualifying the status ("Waiting for After Effects").
    // Empty when the status says it all.
    virtual QString statusDetail() const { return {}; }
    virtual QString url() const = 0;
    virtual int     width() const = 0;
    virtual int     height() const = 0;
    virtual QString codecName() const = 0;
    virtual QString pixelFormatName() const = 0;
    virtual bool    hasAudio() const { return false; }
    virtual int        audioChannels()   const { return 0; }
    virtual int        audioSampleRate() const { return 0; }
    virtual AudioState audioState()      const { return NoAudioSegment; }
    // Delay every published frame by `ms` (0 = none): the negative half
    // of the live A/V offset, since audio can only be delayed, never
    // advanced, relative to its real-time arrival.
    virtual void setVideoHoldMs(int ms) { Q_UNUSED(ms); }
    // The stream's colour tags as FFmpeg names ("smpte2084", "bt2020",
    // "bt2020nc"), empty when the source has none to give — a network
    // stream's codec parameters carry them; a QCBridge feed is the host's
    // working space, untagged by nature. Change with metadataChanged.
    virtual QString colorTransfer() const { return {}; }
    virtual QString colorPrimaries() const { return {}; }
    virtual QString colorMatrix() const { return {}; }
    virtual int     reconnectCount() const = 0;
    // The latest frame carried inf or NaN (never clamped upstream).
    virtual bool    nonFinite() const { return false; }
    // Same-machine shared memory rather than a network stream: LiveStrip
    // shows throughput differently and drops network wording.
    virtual bool    sharedMemory() const { return false; }

    // Polled by LiveStrip's 1 s timer. Q_INVOKABLE, not Q_PROPERTY:
    // per-frame NOTIFY signals would be pure churn.
    Q_INVOKABLE virtual double statFramesReceived() const = 0;
    Q_INVOKABLE virtual double statBytesReceived() const = 0;
    Q_INVOKABLE virtual double statFramesConflated() const { return 0.0; }
    Q_INVOKABLE virtual int    statLiveSeconds() const = 0;   // 0 when not live

signals:
    void statusChanged();
    void metadataChanged();
    void nonFiniteChanged();
    void audioChanged();
};

} // namespace qcv
