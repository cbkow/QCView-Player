// DualLiveSource — a live stream as one side of dual view.
//
// The dual island is a clock pump over seekable sources; a live feed has no
// timeline at all. This source is the adapter: it owns a LiveSource receiver
// (HostBridgeSource for qcbae:// After Effects / Premiere frames,
// LiveStreamDecoder for srt:// and other FFmpeg URLs), takes its published
// frames into a latest-wins slot, and hands that frame back for ANY master
// frame the controller asks about.
//
// Consequences, all deliberate:
//   - fps() and frameCount() are 0. isLive() is true, and the controller
//     keeps live sides out of the master clock and skips the
//     master-frame→source-frame translation and the past-end test for them
//     (without that the side would render transparent for ever).
//   - setDecodeTarget / seekTo are no-ops. The pump never checks them.
//   - When the feed stops (After Effects loses focus, SRT drops) the last
//     frame stays in the slot, so the side holds its picture instead of
//     going black. The chip shows the receiver's status.
//   - No scrub decoder is built for it (the controller only builds those for
//     DualVideoDecoder), so scrubbing B leaves a live A alone.
//
// One receiver per side: two live sides each own theirs, which is also what
// makes qcbae://ae on one side and srt:// on the other work.

#pragma once

#include "decode/live_source.h"
#include "i_dual_source.h"

#include <QString>

#include <memory>
#include <mutex>

namespace qcv::dual {

class DualLiveSource : public IDualSource, public qcv::LiveFrameSink
{
public:
    DualLiveSource();
    ~DualLiveSource() override;

    // ---- IDualSource ----
    bool open(const QString &path) override;
    void close() override;
    bool isOpen() const override;

    // No-ops: this source has no timeline to seek in.
    void setDecodeTarget(int frameNumber) override;
    void seekTo(int frameNumber) override;

    // Both ignore the frame number and return the latest frame received.
    std::shared_ptr<DualFrame> getBufferedFrame(int frameNumber) const override;
    std::shared_ptr<DualFrame> getClosestFrame(int frameNumber) const override;

    bool hasFrame(int frameNumber) const override;
    int  bufferedAhead() const override;
    int  bufferedBehind() const override;
    int  bufferCount() const override;
    void getBufferedRange(int &startFrame, int &endFrame) const override;

    int    width() const override;
    int    height() const override;
    double fps() const override { return 0.0; }         // free-running
    int    frameCount() const override { return 0; }    // no extent
    QString path() const override { return m_url; }
    QString hwAccelName() const override;
    bool   isLive() const override { return true; }

    void setFrameAvailableCallback(FrameAvailableCallback cb) override;

    // ---- LiveFrameSink ----
    void publishExternalFrame(qcv::FrameHandle handle, int64_t pts) override;

    // The receiver, for status in the UI (never null between open and close).
    qcv::LiveSource *live() const { return m_live.get(); }

private:
    QString m_url;
    std::unique_ptr<qcv::LiveSource> m_live;

    mutable std::mutex         m_frameMutex;
    std::shared_ptr<DualFrame> m_latest;

    mutable std::mutex      m_callbackMutex;
    FrameAvailableCallback  m_onFrameAvailable;
};

} // namespace qcv::dual
