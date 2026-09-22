// ReadAhead — warm the bytes of the next few video frames before the
// decoder asks for them.
//
// On a network volume (LucidLink, SMB shares) an uncached frame costs a
// round trip per read, and a decoder reads one frame at a time: 8K ProRes
// arrived at about one frame a second, while the same file played in real
// time once cached. This touches the next frames' exact byte ranges, from
// the container's index: one byte per volume cache page (LucidLink's page is
// 1 MiB; QCV_READAHEAD_PAGE_KB). A byte read anywhere in a page makes the
// volume download and keep the whole page, so the decoder's own read is then
// served from the volume's local cache (measured: LucidLink's on-disk cache
// grew by the file's size). Nothing is held in the app: a table of frame
// offsets. A plain SMB share has no such page cache, so there a touch leaves
// nothing behind; LucidLink is the case this is for.
//
// It runs for every video file. Telling a network volume from a local one
// isn't reliable (symlinks, firmlinks, SMB loopback gateways, File Provider
// mounts), so instead it is kept cheap: one background thread for the whole
// app, utility QoS and utility I/O priority (the decoder's reads win), no
// locks shared with decode or render, asleep when there is nothing to warm.
// On a local disk a touch costs one small page read per MiB, next to
// nothing.
//
// Driven by read position, not the playhead: each decoder reports the first
// frame it hasn't read yet (after a decoded frame, or on a seek), and the
// window runs from there, where the decoder reads next. A jump (backwards,
// or past the window) restarts the window, so at most one 2 MB chunk is
// spent on frames nobody will read.
//
// The window is media time with a byte cap, not a frame count: frames range
// from ~100 KB (HD H.264) to ~20 MB (8K ProRes), so 15 frames was either
// nothing or 300 MB. QCV_READAHEAD_SECONDS (default 2; 0 turns read-ahead
// off) and QCV_READAHEAD_MB (default 512). The dual decoder buffers ~8
// frames ahead on its own, so the window has to reach past that to help.
// When the link is slower than the file's bitrate (uncached 8K), no window
// keeps playback real-time; a larger one only helps paused viewing,
// stepping, and building the cache. The defaults are placeholders until
// measured on a large uncached file.

#pragma once

#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

struct AVFormatContext;

namespace qcv {

// One frame's bytes in the file: the frame's sample plus any interleaved
// data (audio) up to the next video sample, when that gap is small.
struct FrameSpan {
    int64_t pos = -1;   // -1 = unknown, skipped
    int64_t len = 0;
};

class ReadAhead
{
public:
    using Id = std::uint64_t;   // 0 = not attached

    static ReadAhead &instance();

    // The table of frame spans from the demuxer's index. tsToFrame maps an
    // index timestamp to the caller's own frame number, so positions reported
    // later line up. Empty when the container has no usable index: MOV/MP4
    // list every sample, but Matroska indexes only its cue points, so an MKV
    // gets no read-ahead (a byte-position fallback could cover it later).
    static std::vector<FrameSpan> spansFromIndex(
        AVFormatContext *fmt, int streamIdx,
        const std::function<int(int64_t)> &tsToFrame, int frameCount);

    // Returns 0 (and does nothing further) when read-ahead is off or there
    // are no spans. fps converts the time window to frames. Any thread.
    Id attach(const QString &path, std::vector<FrameSpan> spans, double fps);
    void detach(Id id);

    // The first frame the caller has not read yet. Cheap; any thread;
    // unknown or detached ids are ignored.
    void setPosition(Id id, int nextFrame);

    // A loop range (inclusive frames) to hydrate whole, after the playhead
    // window is warm: the part of the file the user is reviewing. first < 0
    // clears it. Capped by QCV_READAHEAD_LOOP_GB (default 16) so an in/out
    // spanning a whole feature can't flood the volume's cache. Any thread.
    void setRange(Id id, int firstFrame, int lastFrame);

    ~ReadAhead();

private:
    ReadAhead();
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace qcv
