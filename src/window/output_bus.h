// OutputBus — QCView's end of the output bus (QCBridgeN/PLAN.md P2).
//
// Owns what the renderer must not know about: the installed sink plugins
// (manifests), which of them the user enabled, the rings each one gets
// (`/qcview-out-<id>` and its `-audio` twin), the plugin process and the
// O13 lifecycle, the request/reply handshake, the sink's status line, the
// feed mute and the included layers. It hands the renderer one
// OutputTarget per enabled output and updates its spec and active flag as
// the sink's request and presence change (polled at 4 Hz on the GUI
// thread — a request is a one-off, presence has a 1 s window).
//
// Audio: both players call audioTap() on their device thread with the
// mixed stereo before Listen, mute and volume. The tap splits to planes
// and pushes into every enabled audio ring, zeros with the muted flag
// when the master mute or the feed mute is on. Lock-free; the rings were
// built for a host's audio thread.

#pragma once

#include "decode/qcbae/audio_ring.h"
#include "decode/qcbae/output_ring.h"
#include "output/stream_sink.h"
#include "render/output_target.h"

#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantList>

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace qcv {

class IPlayerRenderer;

class OutputBus : public QObject
{
    Q_OBJECT
    // One map per installed output: id, name, installed, enabled, present,
    // accepted, shape, status, plugin, published, seen.
    Q_PROPERTY(QVariantList outputs READ outputs NOTIFY outputsChanged)
    Q_PROPERTY(int enabledCount READ enabledCount NOTIFY outputsChanged)
    Q_PROPERTY(bool feedMuted READ feedMuted WRITE setFeedMuted NOTIFY feedMutedChanged)
    Q_PROPERTY(bool includeGuides READ includeGuides WRITE setIncludeGuides NOTIFY layersChanged)
    Q_PROPERTY(bool includeAnnotations READ includeAnnotations WRITE setIncludeAnnotations NOTIFY layersChanged)

public:
    explicit OutputBus(QObject *parent = nullptr);
    ~OutputBus() override;

    // WindowManager gives the bus a way to reach the live renderer (it
    // changes with the player window) and the source frame rate.
    void setRendererAccessor(std::function<IPlayerRenderer *()> fn) { m_renderer = std::move(fn); }
    void setFpsAccessor(std::function<double()> fn) { m_fps = std::move(fn); }
    // Call once the renderer exists: restores the outputs enabled last
    // time and pushes the targets.
    void rendererReady();
    // Before the renderer goes away.
    void rendererGone();

    QVariantList outputs() const;
    int  enabledCount() const;
    bool feedMuted() const { return m_feedMuted.load(std::memory_order_acquire); }
    void setFeedMuted(bool on);
    bool includeGuides() const { return m_includeGuides; }
    void setIncludeGuides(bool on);
    bool includeAnnotations() const { return m_includeAnnotations; }
    void setIncludeAnnotations(bool on);

    Q_INVOKABLE void setEnabled(const QString &id, bool on);
    Q_INVOKABLE bool isEnabled(const QString &id) const;
    Q_INVOKABLE void rescan();

    // The audio players' tap (AudioPlayer::OutputTap signature). `ctx`
    // is the bus. Device thread.
    static void audioTap(void *ctx, const float *stereo, uint32_t frames, bool muted);

signals:
    void outputsChanged();
    void feedMutedChanged();
    void layersChanged();

private:
    struct Output {
        // manifest
        QString     id, name, exec;
        QStringList args;
        int         protocol = 1;
        bool        installed = false;   // exec exists
        // state
        bool        enabled = false;
        std::unique_ptr<qcbae::OutputRing>     ring;
        std::unique_ptr<qcbae::AudioRing>      audio;
        std::unique_ptr<output::OutputTarget>  target;
        QProcess   *proc = nullptr;
        bool        launchedByUs = false;
        int         restarts = 0;
        qint64      firstRestartMs = 0;
        bool        gaveUp = false;
        uint64_t    reqGen = 0, statusGen = 0;
        qcbae::OutputRequest req {};
        bool        accepted = false;
        bool        present = false;
        QString     shape, status, plugin;
        int         audioSlot = -1;
    };

    void scanManifests();
    void loadManifest(const QString &path);
    Output *find(const QString &id);
    void enable(Output &o);
    void disable(Output &o);
    void launch(Output &o);
    void onProcessFinished(Output *o, int code, QProcess::ExitStatus st);
    void poll();
    void answer(Output &o);
    void pushTargets();
    void restoreEnabled();
    void tap(const float *stereo, uint32_t frames, bool muted);
    // The stream output (plan A0: env-driven spike behind QCV_STREAM_SPIKE;
    // A1 makes it a setting with UI). One GPU-sink target beside the ring
    // targets; the sink outlives the target's removal from the renderer.
    void startSpikeStream();
    void stopStream();

    std::vector<std::unique_ptr<Output>> m_outputs;
    std::unique_ptr<output::StreamSink>   m_stream;
    std::unique_ptr<output::OutputTarget> m_streamTarget;
    std::function<IPlayerRenderer *()> m_renderer;
    std::function<double()>            m_fps;
    QTimer m_poll;
    bool   m_rendererReady = false;

    static constexpr int kMaxAudio = 4;
    std::atomic<qcbae::AudioRing *> m_audioSlots[kMaxAudio];
    std::atomic<bool> m_feedMuted{false};
    bool m_includeGuides = true;
    bool m_includeAnnotations = true;
};

} // namespace qcv
