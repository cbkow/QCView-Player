#include "stream_muxer.h"

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

#include <QtLogging>

#include <cstring>

namespace qcv::output {

namespace {

std::string avErr(int rc)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(rc, buf, sizeof buf);
    return buf;
}

bool startsWith(const std::string &s, const char *p) { return s.rfind(p, 0) == 0; }

} // namespace

StreamMuxer::StreamMuxer() = default;
StreamMuxer::~StreamMuxer() { close(); }

int StreamMuxer::interruptCb(void *opaque)
{
    return static_cast<StreamMuxer *>(opaque)->m_interrupt.load(std::memory_order_acquire) ? 1 : 0;
}

bool StreamMuxer::open(const MuxConfig &cfg, std::string *error)
{
    close();
    m_interrupt.store(false, std::memory_order_release);
    auto fail = [&](const std::string &msg) {
        m_lastError = msg;
        if (error) *error = msg;
        qWarning("StreamMuxer: %s", msg.c_str());
        close();
        return false;
    };

    const bool srt = startsWith(cfg.url, "srt://");
    if (!srt) return fail("only srt:// is wired up in this build (rtmp comes with A3): " + cfg.url);

    int rc = avformat_alloc_output_context2(&m_ctx, nullptr, "mpegts", cfg.url.c_str());
    if (rc < 0 || !m_ctx) return fail("mpegts muxer: " + avErr(rc));
    m_ctx->interrupt_callback.callback = &StreamMuxer::interruptCb;
    m_ctx->interrupt_callback.opaque   = this;

    m_video = avformat_new_stream(m_ctx, nullptr);
    if (!m_video) return fail("could not add the video stream");
    m_video->id = 0;
    m_video->time_base = AVRational{1, 90000};
    m_video->avg_frame_rate = AVRational{static_cast<int>(cfg.fpsNum), static_cast<int>(cfg.fpsDen)};
    AVCodecParameters *par = m_video->codecpar;
    par->codec_type = AVMEDIA_TYPE_VIDEO;
    par->codec_id   = AV_CODEC_ID_H264;
    par->width      = static_cast<int>(cfg.width);
    par->height     = static_cast<int>(cfg.height);
    par->format     = AV_PIX_FMT_NV12;
    par->color_primaries = AVCOL_PRI_BT709;
    par->color_trc       = AVCOL_TRC_BT709;
    par->color_space     = AVCOL_SPC_BT709;
    par->color_range     = AVCOL_RANGE_MPEG;
    // Annex-B with in-band parameter sets: no extradata; mpegtsenc adds
    // the AUD when the access unit has none.

    // Protocol options. libavformat's srt protocol reads these as
    // AVOptions on the URLContext; the URL's own query keeps working too.
    AVDictionary *popts = nullptr;
    av_dict_set(&popts, "mode", "caller", 0);
    av_dict_set_int(&popts, "latency", static_cast<int64_t>(cfg.srtLatencyMs) * 1000, 0);
    av_dict_set_int(&popts, "connect_timeout", cfg.connectTimeoutMs, 0);
    av_dict_set_int(&popts, "pkt_size", 1316, 0);
    av_dict_set(&popts, "transtype", "live", 0);
    if (!cfg.streamId.empty())   av_dict_set(&popts, "streamid", cfg.streamId.c_str(), 0);
    if (!cfg.passphrase.empty()) {
        av_dict_set(&popts, "passphrase", cfg.passphrase.c_str(), 0);
        av_dict_set_int(&popts, "pbkeylen", cfg.pbKeyLen, 0);
    }
    rc = avio_open2(&m_ctx->pb, cfg.url.c_str(), AVIO_FLAG_WRITE, &m_ctx->interrupt_callback, &popts);
    if (popts) {
        const AVDictionaryEntry *e = nullptr;
        while ((e = av_dict_iterate(popts, e))) qInfo("StreamMuxer: protocol option not consumed: %s", e->key);
        av_dict_free(&popts);
    }
    if (rc < 0) return fail("connect " + cfg.url + ": " + avErr(rc));

    // Muxer options: a live stream, no PAT/PMT period games.
    AVDictionary *mopts = nullptr;
    av_dict_set(&mopts, "mpegts_flags", "+resend_headers+initial_discontinuity", 0);
    rc = avformat_write_header(m_ctx, &mopts);
    if (mopts) av_dict_free(&mopts);
    if (rc < 0) return fail("write_header: " + avErr(rc));
    m_headerWritten = true;
    qInfo("StreamMuxer: streaming %ux%u H.264 in MPEG-TS to %s (SRT latency %d ms)",
          cfg.width, cfg.height, cfg.url.c_str(), cfg.srtLatencyMs);
    return true;
}

bool StreamMuxer::writeVideo(const uint8_t *data, size_t size, int64_t pts90k, int64_t dur90k, bool keyframe)
{
    if (!isOpen() || !data || size == 0) return false;
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return false;
    if (av_new_packet(pkt, static_cast<int>(size)) < 0) { av_packet_free(&pkt); return false; }
    std::memcpy(pkt->data, data, size);
    pkt->stream_index = m_video->index;
    pkt->pts = pts90k;
    pkt->dts = pts90k;
    pkt->duration = dur90k;
    if (keyframe) pkt->flags |= AV_PKT_FLAG_KEY;
    const int rc = av_interleaved_write_frame(m_ctx, pkt);
    av_packet_free(&pkt);
    if (rc < 0) {
        m_errors.fetch_add(1, std::memory_order_relaxed);
        m_lastError = "write: " + avErr(rc);
        if (m_errors.load() <= 3) qWarning("StreamMuxer: %s", m_lastError.c_str());
        return false;
    }
    m_packets.fetch_add(1, std::memory_order_relaxed);
    m_bytes.fetch_add(size, std::memory_order_relaxed);
    return true;
}

void StreamMuxer::close()
{
    if (m_ctx) {
        if (m_headerWritten) av_write_trailer(m_ctx);
        if (m_ctx->pb) avio_closep(&m_ctx->pb);
        avformat_free_context(m_ctx);
        m_ctx = nullptr;
    }
    m_video = nullptr;
    m_headerWritten = false;
}

} // namespace qcv::output
