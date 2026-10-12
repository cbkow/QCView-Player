#include "aac_encoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
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
}

AacEncoder::AacEncoder() = default;
AacEncoder::~AacEncoder() { close(); }

bool AacEncoder::open(uint32_t bitrateKbps, std::string *error)
{
    close();
    auto fail = [&](const std::string &msg) {
        if (error) *error = msg;
        qWarning("AacEncoder: %s", msg.c_str());
        close();
        return false;
    };
    const AVCodec *codec = avcodec_find_encoder_by_name("aac");
    if (!codec) return fail("FFmpeg has no 'aac' encoder");
    m_ctx = avcodec_alloc_context3(codec);
    if (!m_ctx) return fail("could not allocate the AAC context");
    m_ctx->sample_rate = kSampleRate;
    m_ctx->sample_fmt  = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&m_ctx->ch_layout, kChannels);
    m_ctx->bit_rate    = static_cast<int64_t>(bitrateKbps) * 1000;
    m_ctx->time_base   = AVRational{1, static_cast<int>(kSampleRate)};
    m_ctx->flags      |= AV_CODEC_FLAG_GLOBAL_HEADER;   // AudioSpecificConfig in extradata
    int rc = avcodec_open2(m_ctx, codec, nullptr);
    if (rc < 0) return fail("avcodec_open2(aac): " + avErr(rc));
    if (m_ctx->frame_size != static_cast<int>(kFrameSamples)) {
        return fail("aac frame size is " + std::to_string(m_ctx->frame_size) + ", expected 1024");
    }
    m_extradata.assign(m_ctx->extradata, m_ctx->extradata + m_ctx->extradata_size);
    m_frame = av_frame_alloc();
    m_pkt   = av_packet_alloc();
    if (!m_frame || !m_pkt) return fail("could not allocate the AAC frame/packet");
    m_frame->format = AV_SAMPLE_FMT_FLTP;
    m_frame->nb_samples = static_cast<int>(kFrameSamples);
    m_frame->sample_rate = kSampleRate;
    av_channel_layout_copy(&m_frame->ch_layout, &m_ctx->ch_layout);
    rc = av_frame_get_buffer(m_frame, 0);
    if (rc < 0) return fail("av_frame_get_buffer: " + avErr(rc));
    m_frames = 0; m_bytes = 0;
    qInfo("AacEncoder: AAC-LC 48 kHz stereo, %u kbps, %zu-byte config", bitrateKbps, m_extradata.size());
    return true;
}

void AacEncoder::close()
{
    if (m_pkt)   av_packet_free(&m_pkt);
    if (m_frame) av_frame_free(&m_frame);
    if (m_ctx)   avcodec_free_context(&m_ctx);
    m_extradata.clear();
}

bool AacEncoder::encode(const float *left, const float *right, int64_t ptsSamples, std::vector<EncodedUnit> *out)
{
    if (!m_ctx || !m_frame || !m_pkt) return false;
    int rc = av_frame_make_writable(m_frame);
    if (rc < 0) return false;
    std::memcpy(m_frame->data[0], left,  kFrameSamples * sizeof(float));
    std::memcpy(m_frame->data[1], right, kFrameSamples * sizeof(float));
    m_frame->pts = ptsSamples;
    rc = avcodec_send_frame(m_ctx, m_frame);
    if (rc < 0) {
        qWarning("AacEncoder: send_frame: %s", avErr(rc).c_str());
        return false;
    }
    for (;;) {
        rc = avcodec_receive_packet(m_ctx, m_pkt);
        if (rc == AVERROR(EAGAIN) || rc == AVERROR_EOF) break;
        if (rc < 0) { qWarning("AacEncoder: receive_packet: %s", avErr(rc).c_str()); return false; }
        EncodedUnit u;
        u.audio = true;
        u.data.assign(m_pkt->data, m_pkt->data + m_pkt->size);
        u.pts   = m_pkt->pts;
        u.dur   = m_pkt->duration > 0 ? m_pkt->duration : kFrameSamples;
        u.keyframe = true;
        ++m_frames;
        m_bytes += u.data.size();
        if (out) out->push_back(std::move(u));
        av_packet_unref(m_pkt);
    }
    return true;
}

} // namespace qcv::output
