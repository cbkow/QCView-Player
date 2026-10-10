// LiveAudioSource — the QCBridgeAE audio segment as an IAudioSource.
//
// Premiere pushes a copy of what its own audio device plays to the
// QCBridgeAE Transmit device ("Audio Stream" ticked for the device in
// Preferences > Playback): planar float32, the sequence's channels,
// 1024-frame packets stamped with timeline time, into a shared-memory
// segment beside the frame ring (QCBridgeAE DESIGN-NOTES D6, vendored
// in decode/qcbae/audio_ring.*). This class reads that segment on its
// own thread, folds the planes to the stereo the device wants with the
// clip's routing mode (the same computeRoutingMatrix the file decoders
// use), and queues the result in an AudioRingBuffer the render callback
// drains like any other source.
//
// No timeline of its own: seek() is a no-op, duration() is 0, and
// isLive() tells the players to keep their servos off it. It plays what
// arrives when it arrives, behind a small jitter buffer whose depth is a
// user setting (plus the positive part of the live A/V offset). Clock
// drift between Premiere's device and ours is parts per million; when
// the queue nonetheless creeps past the target by two packets, one
// incoming packet is dropped.
//
// Playdown (whether the viewer hears it) is NOT decided here: the
// players zero their output after draining, so meters and taps keep
// running while the output is silent, and the stream never stalls.
//
// Taps: a future consumer (the NDI plugin) registers a callback and
// receives every packet as the host sent it — planar, all channels,
// with its timestamp — on the reader thread, before the fold.

#pragma once

#include "i_audio_source.h"
#include "audio_ring_buffer.h"
#include "decode/qcbae/audio_desc.h"

#include <QString>
#include <QStringList>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace qcv {

class LiveAudioSource : public IAudioSource
{
public:
    // Planar packet as the host sent it: `planes[c]` holds
    // `desc.frames` floats for c < desc.channels.
    using Tap = std::function<void(const qcbae::AudioPacketDesc &desc,
                                   const float *const *planes,
                                   const qcbae::AudioSession &session)>;

    LiveAudioSource();
    ~LiveAudioSource() override;

    // ---- IAudioSource ----
    bool open(const QString &url) override;      // a qcbae:// URL
    void close() override;
    void start() override;
    void stop() override;

    bool   hasAudio()  const override;
    bool   isOpen()    const override { return m_thread.joinable(); }
    bool   isRunning() const override { return m_running.load(std::memory_order_acquire); }
    double duration()  const override { return 0.0; }
    bool   isLive()    const override { return true; }

    void        seek(double) override {}
    std::size_t read(float *output, std::size_t frameCount) override;
    const AudioFormat &format() const override { return m_format; }

    double   secondsSinceLastSeek() const override { return 1e9; }
    bool     seekPending()          const override { return false; }
    uint32_t    flushGeneration()   const override { return m_ring.staleGeneration(); }
    std::size_t staleFrames()       const override { return m_ring.staleBytes() / m_format.bytesPerFrame(); }
    void        discardStale()            override { m_ring.discardStale(); }

    void setRoutingMode(int mode) override;
    int  routingMode() const override { return m_routingMode.load(std::memory_order_acquire); }
    void   setTempo(double) override {}
    double tempo() const override { return 1.0; }

    int         sourceChannels()          const override { return m_channels.load(std::memory_order_acquire); }
    QString     sourceChannelLayoutName() const override;
    std::array<float, 16> peakLevels()    const override;
    QStringList sourceChannelNames()      const override;

    // ---- Live facts (for the players' logs; the UI reads the
    // LiveSource's own copy) ----
    int sampleRate() const { return m_sampleRate.load(std::memory_order_acquire); }

    // ---- Taps ----
    int  addTap(Tap tap);
    void removeTap(int id);

    // ---- Process-wide settings (WindowManager pushes them) ----
    // Jitter-buffer depth and the live A/V offset (its positive part
    // adds depth; the negative part is a video hold, owned by the
    // frame reader). Scrub mute silences packets pushed during a host
    // scrub, the same setting that mutes the shuttle engines.
    static void setGlobalBufferMs(int ms);
    static void setGlobalSyncOffsetMs(int ms);
    static void setGlobalScrubMute(bool muted);

private:
    void readerLoop();
    bool interruptibleSleep(int ms);
    void rebuildFold(int channels);

    AudioFormat              m_format;
    AudioRingBuffer          m_ring;        // interleaved stereo float, ~1 s
    QString                  m_url;
    std::string              m_segmentName;

    std::thread              m_thread;
    std::atomic<bool>        m_stop{false};
    std::atomic<bool>        m_running{false};
    std::mutex               m_sleepMutex;
    std::condition_variable  m_sleepCv;

    std::atomic<bool>        m_hasAudio{false};
    std::atomic<int>         m_channels{0};
    std::atomic<int>         m_sampleRate{0};
    std::atomic<int>         m_routingMode{0};
    std::array<std::atomic<float>, 16> m_peaks{};

    // The 2xN fold, rebuilt on the reader thread when the mode or the
    // channel count changes.
    std::vector<float>       m_fold;         // reader thread only
    int                      m_foldChannels = 0;
    int                      m_foldMode     = -1;

    std::mutex               m_tapMutex;
    std::vector<std::pair<int, Tap>> m_taps;
    int                      m_nextTapId = 1;

    static std::atomic<int>  s_bufferMs;
    static std::atomic<int>  s_syncOffsetMs;
    static std::atomic<bool> s_scrubMute;
};

} // namespace qcv
