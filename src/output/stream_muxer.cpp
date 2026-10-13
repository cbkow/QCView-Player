#include "stream_muxer.h"

extern "C" {
#include <libavcodec/packet.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
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

// Offsets of the Annex-B units in [p, p+n): (unit start after its start code, unit length).
void annexBUnits(const uint8_t *p, size_t n, std::vector<std::pair<size_t, size_t>> *units)
{
    units->clear();
    auto findStart = [&](size_t from, size_t *sc) -> size_t {
        for (size_t k = from; k + 3 <= n; ++k) {
            if (p[k] == 0 && p[k + 1] == 0) {
                if (p[k + 2] == 1) { *sc = 3; return k; }
                if (k + 4 <= n && p[k + 2] == 0 && p[k + 3] == 1) { *sc = 4; return k; }
            }
        }
        return n;
    };
    size_t sc = 0;
    size_t s = findStart(0, &sc);
    while (s < n) {
        const size_t unit = s + sc;
        size_t sc2 = 0;
        const size_t next = findStart(unit, &sc2);
        if (unit < n && next > unit) units->emplace_back(unit, next - unit);
        s = next; sc = sc2;
    }
}

} // namespace

bool StreamMuxer::isRtmp(const std::string &url)
{
    return startsWith(url, "rtmp://") || startsWith(url, "rtmps://");
}

bool StreamMuxer::parameterSets(const uint8_t *data, size_t size, std::vector<uint8_t> *out)
{
    std::vector<std::pair<size_t, size_t>> units;
    annexBUnits(data, size, &units);
    static const uint8_t kStart[4] = {0, 0, 0, 1};
    std::vector<uint8_t> sps, pps;
    for (const auto &[off, n] : units) {
        const int t = data[off] & 0x1f;
        if (t == 7)      sps.assign(data + off, data + off + n);
        else if (t == 8) pps.assign(data + off, data + off + n);
    }
    if (sps.empty() || pps.empty()) return false;
    out->clear();
    out->insert(out->end(), kStart, kStart + 4);
    out->insert(out->end(), sps.begin(), sps.end());
    out->insert(out->end(), kStart, kStart + 4);
    out->insert(out->end(), pps.begin(), pps.end());
    return true;
}

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
    m_flv = isRtmp(cfg.url);
    if (!srt && !m_flv) return fail("the URL must start with srt://, rtmp:// or rtmps://: " + cfg.url);

    int rc = avformat_alloc_output_context2(&m_ctx, nullptr, m_flv ? "flv" : "mpegts", cfg.url.c_str());
    if (rc < 0 || !m_ctx) return fail(std::string(m_flv ? "flv" : "mpegts") + " muxer: " + avErr(rc));
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
    // TS: Annex-B with in-band parameter sets, no extradata (mpegtsenc adds
    // the AUD). FLV: the SPS/PPS go into the header as avcC — given now, or
    // taken from the first keyframe in writeVideo.
    if (m_flv && !cfg.videoExtradata.empty()) {
        par->extradata = static_cast<uint8_t *>(av_mallocz(cfg.videoExtradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!par->extradata) return fail("could not allocate the video extradata");
        std::memcpy(par->extradata, cfg.videoExtradata.data(), cfg.videoExtradata.size());
        par->extradata_size = static_cast<int>(cfg.videoExtradata.size());
    }

    if (!cfg.audioExtradata.empty()) {
        m_audio = avformat_new_stream(m_ctx, nullptr);
        if (!m_audio) return fail("could not add the audio stream");
        m_audio->id = 1;
        m_audio->time_base = AVRational{1, 48000};
        AVCodecParameters *ap = m_audio->codecpar;
        ap->codec_type  = AVMEDIA_TYPE_AUDIO;
        ap->codec_id    = AV_CODEC_ID_AAC;
        ap->sample_rate = 48000;
        ap->format      = AV_SAMPLE_FMT_FLTP;
        ap->bit_rate    = static_cast<int64_t>(cfg.audioKbps) * 1000;
        ap->frame_size  = 1024;
        av_channel_layout_default(&ap->ch_layout, 2);
        ap->extradata = static_cast<uint8_t *>(av_mallocz(cfg.audioExtradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!ap->extradata) return fail("could not allocate the audio extradata");
        std::memcpy(ap->extradata, cfg.audioExtradata.data(), cfg.audioExtradata.size());
        ap->extradata_size = static_cast<int>(cfg.audioExtradata.size());
    }

    // Protocol options. libavformat's srt protocol reads these as
    // AVOptions on the URLContext; the URL's own query keeps working too.
    AVDictionary *popts = nullptr;
    if (m_flv) {
        av_dict_set(&popts, "rtmp_live", "live", 0);
    } else {
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
    }
    rc = avio_open2(&m_ctx->pb, cfg.url.c_str(), AVIO_FLAG_WRITE, &m_ctx->interrupt_callback, &popts);
    if (popts) {
        const AVDictionaryEntry *e = nullptr;
        while ((e = av_dict_iterate(popts, e))) qInfo("StreamMuxer: protocol option not consumed: %s", e->key);
        av_dict_free(&popts);
    }
    if (rc < 0) return fail("connect " + cfg.url + ": " + avErr(rc));
    m_connected = true;

    if (m_flv && par->extradata_size == 0) {
        qInfo("StreamMuxer: connected to %s; the FLV header waits for the first keyframe", cfg.url.c_str());
        return true;
    }
    if (!writeHeader(error)) return false;
    if (m_flv) qInfo("StreamMuxer: streaming %ux%u H.264%s in FLV to %s",
                     cfg.width, cfg.height, m_audio ? " + AAC" : "", cfg.url.c_str());
    else       qInfo("StreamMuxer: streaming %ux%u H.264%s in MPEG-TS to %s (SRT latency %d ms)",
                     cfg.width, cfg.height, m_audio ? " + AAC" : "", cfg.url.c_str(), cfg.srtLatencyMs);
    return true;
}

bool StreamMuxer::writeHeader(std::string *error)
{
    // Muxer options: a live stream, no PAT/PMT period games (TS), no
    // duration / file size placeholders (FLV).
    AVDictionary *mopts = nullptr;
    if (m_flv) av_dict_set(&mopts, "flvflags", "no_duration_filesize", 0);
    else       av_dict_set(&mopts, "mpegts_flags", "+resend_headers+initial_discontinuity", 0);
    const int rc = avformat_write_header(m_ctx, &mopts);
    if (mopts) av_dict_free(&mopts);
    if (rc < 0) {
        m_lastError = "write_header: " + avErr(rc);
        if (error) *error = m_lastError;
        qWarning("StreamMuxer: %s", m_lastError.c_str());
        return false;
    }
    m_headerWritten = true;
    return true;
}

bool StreamMuxer::writeVideo(const uint8_t *data, size_t size, int64_t pts90k, int64_t dur90k, bool keyframe)
{
    if (!isOpen() || !data || size == 0) return false;
    if (!m_headerWritten) {
        // FLV without parameter sets up front: the first keyframe brings them.
        if (!keyframe) return true;
        std::vector<uint8_t> ps;
        if (!parameterSets(data, size, &ps)) {
            m_errors.fetch_add(1, std::memory_order_relaxed);
            m_lastError = "the keyframe carries no SPS/PPS for the FLV header";
            if (m_errors.load() <= 3) qWarning("StreamMuxer: %s", m_lastError.c_str());
            return false;
        }
        AVCodecParameters *par = m_video->codecpar;
        av_freep(&par->extradata);
        par->extradata = static_cast<uint8_t *>(av_mallocz(ps.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!par->extradata) return false;
        std::memcpy(par->extradata, ps.data(), ps.size());
        par->extradata_size = static_cast<int>(ps.size());
        std::string err;
        if (!writeHeader(&err)) { m_errors.fetch_add(1, std::memory_order_relaxed); return false; }
        qInfo("StreamMuxer: FLV header written from the first keyframe (%zu bytes of SPS/PPS)", ps.size());
    }
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return false;
    if (av_new_packet(pkt, static_cast<int>(size)) < 0) { av_packet_free(&pkt); return false; }
    std::memcpy(pkt->data, data, size);
    pkt->stream_index = m_video->index;
    pkt->pts = pts90k;
    pkt->dts = pts90k;
    pkt->duration = dur90k;
    if (keyframe) pkt->flags |= AV_PKT_FLAG_KEY;
    av_packet_rescale_ts(pkt, AVRational{1, 90000}, m_video->time_base);
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

bool StreamMuxer::writeAudio(const uint8_t *data, size_t size, int64_t ptsSamples, int64_t durSamples)
{
    if (!isOpen() || !m_audio || !data || size == 0) return false;
    if (!m_headerWritten) return true;   // FLV: nothing before the video header
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) return false;
    if (av_new_packet(pkt, static_cast<int>(size)) < 0) { av_packet_free(&pkt); return false; }
    std::memcpy(pkt->data, data, size);
    pkt->stream_index = m_audio->index;
    pkt->pts = ptsSamples;
    pkt->dts = ptsSamples;
    pkt->duration = durSamples;
    pkt->flags |= AV_PKT_FLAG_KEY;
    av_packet_rescale_ts(pkt, AVRational{1, 48000}, m_audio->time_base);
    const int rc = av_interleaved_write_frame(m_ctx, pkt);
    av_packet_free(&pkt);
    if (rc < 0) {
        m_errors.fetch_add(1, std::memory_order_relaxed);
        m_lastError = "write audio: " + avErr(rc);
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
    m_audio = nullptr;
    m_headerWritten = false;
    m_connected = false;
}

} // namespace qcv::output
