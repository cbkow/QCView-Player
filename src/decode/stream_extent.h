#pragma once

// Duration / frame count for containers that don't declare one.
//
// Animated WebP (FFmpeg 9.0's webp_anim demuxer) — and GIF / APNG in
// the same family — report neither a stream nor a format duration and
// no frame count, so every consumer that sizes a clip from those
// (VideoDecoder::frameCount, the bin's VideoMetadata, the thumbnail
// loader's duration hint) came out at 0 and the clip never got a
// timeline or a first frame. These files are small, so a synchronous
// packet walk is the honest fix: count the video packets and take the
// end of the last one as the duration. Gated by file size so a huge
// duration-less transport stream can't stall an open.

#include <QFileInfo>
#include <QString>
#include <QtLogging>

#include <algorithm>
#include <cstdint>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
#include <libavutil/rational.h>
}

namespace qcv {

struct StreamExtent {
    bool    ok         = false;
    int     frames     = 0;
    int64_t durationUs = 0;   // end of the last packet, microseconds
};

inline StreamExtent scanStreamExtent(const QString &path, int streamIdx,
                                     qint64 maxBytes = qint64(512) << 20)
{
    StreamExtent out;
    const QFileInfo fi(path);
    if (!fi.exists() || fi.size() > maxBytes) return out;

    AVFormatContext *fmt = nullptr;
    const QByteArray p = path.toUtf8();
    if (avformat_open_input(&fmt, p.constData(), nullptr, nullptr) < 0) return out;
    if (avformat_find_stream_info(fmt, nullptr) < 0 || streamIdx < 0
        || streamIdx >= static_cast<int>(fmt->nb_streams)) {
        avformat_close_input(&fmt);
        return out;
    }
    const AVRational tb = fmt->streams[streamIdx]->time_base;
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) { avformat_close_input(&fmt); return out; }

    int64_t maxEnd = AV_NOPTS_VALUE;
    int64_t lastDuration = 0;
    while (av_read_frame(fmt, pkt) >= 0) {
        if (pkt->stream_index == streamIdx) {
            ++out.frames;
            const int64_t ts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
            if (pkt->duration > 0) lastDuration = pkt->duration;
            if (ts != AV_NOPTS_VALUE) {
                const int64_t end = ts + (pkt->duration > 0 ? pkt->duration : lastDuration);
                maxEnd = (maxEnd == AV_NOPTS_VALUE) ? end : std::max(maxEnd, end);
            }
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    avformat_close_input(&fmt);

    if (out.frames > 0) {
        out.ok = true;
        if (maxEnd != AV_NOPTS_VALUE && tb.num > 0 && tb.den > 0) {
            out.durationUs = av_rescale_q(maxEnd, tb, AVRational{ 1, 1000000 });
        }
        qInfo("scanStreamExtent: '%s' declares no duration — counted %d frames, %.3f s",
              qPrintable(fi.fileName()), out.frames, out.durationUs / 1e6);
    }
    return out;
}

} // namespace qcv
