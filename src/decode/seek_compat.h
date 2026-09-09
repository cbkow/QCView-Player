#pragma once

// Seeking for demuxers whose av_seek_frame() leaves them unable to
// deliver another packet (2026-09-09).
//
// FFmpeg 9.0's animated-WebP demuxer (webp_anim) accepts a seek and
// then returns end-of-stream for every subsequent read — reproducible
// with `ffmpeg -ss 0 -i anim.webp` (0 frames) versus `ffmpeg -i
// anim.webp` (all frames). Every decoder here seeks to frame 0 right
// after opening, so an animated WebP opened, counted its frames and
// then never showed one. GIF and APNG (AVFMT_GENERIC_INDEX) seek fine.
//
// seekStream() is a drop-in for av_seek_frame() that, for such
// demuxers, reopens the container at the start instead. Callers
// already decode forward until they reach the target pts, which is
// exactly right for these small intra-only files. The old context is
// only released once the replacement is open, so a failed reopen
// leaves the caller's pointer valid.

#include <cstring>
#include <string>

extern "C" {
#include <libavformat/avformat.h>
}

namespace qcv {

inline bool demuxerNeedsReopenToSeek(const AVFormatContext *fmt)
{
    return fmt && fmt->iformat && fmt->iformat->name
        && std::strcmp(fmt->iformat->name, "webp_anim") == 0;
}

// Returns >= 0 on success (positioned at or before `pts` for normal
// demuxers, at the START for reopen-to-seek demuxers), < 0 on failure.
// `*fmt` may be replaced by a fresh context.
inline int seekStream(AVFormatContext **fmt, int streamIdx, int64_t pts, int flags)
{
    if (!fmt || !*fmt) return -1;
    if (!demuxerNeedsReopenToSeek(*fmt)) {
        return av_seek_frame(*fmt, streamIdx, pts, flags);
    }
    const std::string url = (*fmt)->url ? (*fmt)->url : "";
    if (url.empty()) return -1;
    AVFormatContext *fresh = nullptr;
    if (avformat_open_input(&fresh, url.c_str(), nullptr, nullptr) < 0) return -1;
    if (avformat_find_stream_info(fresh, nullptr) < 0
        || streamIdx < 0 || streamIdx >= static_cast<int>(fresh->nb_streams)) {
        avformat_close_input(&fresh);
        return -1;
    }
    avformat_close_input(fmt);
    *fmt = fresh;
    return 0;
}

} // namespace qcv
