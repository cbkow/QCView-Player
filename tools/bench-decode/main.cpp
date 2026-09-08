// bench-decode — decode-throughput bench for the app's FFmpeg paths.
//
// 2026-09-08 (Phase I.E follow-up). Measures the real cost of the
// three ways QCView can decode a clip, on the same shared Vulkan
// device and the same helpers the app uses, so the numbers are the
// app's numbers minus the compositor:
//
//   sw         software decode (FFmpeg native, slice/frame threads)
//   vk-ffpool  Vulkan hwaccel, FFmpeg-managed frame pool  (pre-I.E)
//   vk-cached  Vulkan hwaccel, app-owned cached pool via
//              acquireSharedVulkanFramesCtx                 (I.E)
//
// Options:
//   --mode M        sw | vk-ffpool | vk-cached          (default vk-cached)
//   --threads N     AVCodecContext::thread_count        (default 0 = auto)
//   --frames N      frames to decode, looping the file  (default 300)
//   --readback      hw frames: av_hwframe_transfer_data to CPU each frame
//                   (models the D3D11VA-style readback path)
//   --reopen K      close + reopen the decoder every K frames, cycling
//                   through the given files (models playlist boundaries)
//   --quiet         drop FFmpeg + app log noise
//
// Usage:  bench-decode [options] file [file ...]
//
// Reports frames, wall time, fps, time-to-first-frame, and (with
// --reopen) the mean open→first-frame latency. Vulkan modes end with
// a device-wide wait so asynchronous GPU work is included in the
// wall time.

#include <QtGlobal>
#include <QtLogging>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#if defined(Q_OS_WIN)
#include "vulkan/vulkan_device_manager.h"
#include "vulkan_hw_device_ctx.h"
#endif

namespace {

enum class Mode { Sw, VkFfPool, VkCached, D3d11va };

struct Options {
    Mode        mode      = Mode::VkCached;
    int         threads   = 0;
    int         frames    = 300;
    bool        readback  = false;
    int         reopen    = 0;
    bool        quiet     = false;
    std::vector<std::string> files;
};

Options g_opt;

using Clock = std::chrono::steady_clock;
double msSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// get_format: prefer Vulkan when offered; in cached mode hand FFmpeg
// our pool exactly as VideoDecoder does.
AVPixelFormat getFormat(AVCodecContext *ctx, const AVPixelFormat *fmts)
{
    if (g_opt.mode == Mode::D3d11va) {
        // The app's inter-codec route (VideoDecoder kOtherPreferred):
        // D3D11 first, always consumed via av_hwframe_transfer_data.
        for (int i = 0; fmts[i] != AV_PIX_FMT_NONE; ++i)
            if (fmts[i] == AV_PIX_FMT_D3D11) return AV_PIX_FMT_D3D11;
        return fmts[0];
    }
    for (int i = 0; fmts[i] != AV_PIX_FMT_NONE; ++i) {
        if (fmts[i] == AV_PIX_FMT_VULKAN) {
#if defined(Q_OS_WIN)
            if (g_opt.mode == Mode::VkCached && !ctx->hw_frames_ctx) {
                ctx->hw_frames_ctx = qcv::acquireSharedVulkanFramesCtx(ctx);
            }
#endif
            return AV_PIX_FMT_VULKAN;
        }
    }
    return fmts[0];
}

struct Decoder {
    AVFormatContext *fmt   = nullptr;
    AVCodecContext  *cctx  = nullptr;
    int              vidx  = -1;

    bool open(const std::string &path, AVBufferRef *hwDev) {
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) return false;
        if (avformat_find_stream_info(fmt, nullptr) < 0) return false;
        const AVCodec *codec = nullptr;
        vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (vidx < 0 || !codec) return false;
        cctx = avcodec_alloc_context3(codec);
        if (!cctx || avcodec_parameters_to_context(cctx, fmt->streams[vidx]->codecpar) < 0)
            return false;
        cctx->pkt_timebase = fmt->streams[vidx]->time_base;
        cctx->thread_count = g_opt.threads;
        cctx->thread_type  = FF_THREAD_FRAME | FF_THREAD_SLICE;
        if (hwDev) {
            cctx->hw_device_ctx = av_buffer_ref(hwDev);
            cctx->get_format    = getFormat;
        }
        return avcodec_open2(cctx, codec, nullptr) >= 0;
    }
    void close() {
        if (cctx) avcodec_free_context(&cctx);
        if (fmt)  avformat_close_input(&fmt);
        vidx = -1;
    }
    void rewind() {
        av_seek_frame(fmt, vidx, 0, AVSEEK_FLAG_BACKWARD);
        avcodec_flush_buffers(cctx);
    }
};

const char *modeName(Mode m) {
    switch (m) {
    case Mode::Sw:       return "sw";
    case Mode::VkFfPool: return "vk-ffpool";
    case Mode::VkCached: return "vk-cached";
    case Mode::D3d11va:  return "d3d11va";
    }
    return "?";
}

bool parseArgs(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char *what) -> const char * {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", what); std::exit(2); }
            return argv[++i];
        };
        if      (a == "--mode") {
            std::string m = next("--mode");
            if      (m == "sw")        g_opt.mode = Mode::Sw;
            else if (m == "vk-ffpool") g_opt.mode = Mode::VkFfPool;
            else if (m == "vk-cached") g_opt.mode = Mode::VkCached;
            else if (m == "d3d11va")   { g_opt.mode = Mode::D3d11va; g_opt.readback = true; }
            else { std::fprintf(stderr, "unknown mode %s\n", m.c_str()); return false; }
        }
        else if (a == "--threads")  g_opt.threads  = std::atoi(next("--threads"));
        else if (a == "--frames")   g_opt.frames   = std::atoi(next("--frames"));
        else if (a == "--reopen")   g_opt.reopen   = std::atoi(next("--reopen"));
        else if (a == "--readback") g_opt.readback = true;
        else if (a == "--quiet")    g_opt.quiet    = true;
        else if (!a.empty() && a[0] == '-') { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return false; }
        else g_opt.files.push_back(a);
    }
    return !g_opt.files.empty();
}

} // namespace

int main(int argc, char **argv)
{
    if (!parseArgs(argc, argv)) {
        std::fprintf(stderr,
            "usage: bench-decode [--mode sw|vk-ffpool|vk-cached] [--threads N] "
            "[--frames N] [--readback] [--reopen K] [--quiet] file [file ...]\n");
        return 2;
    }
    if (g_opt.quiet) {
        av_log_set_level(AV_LOG_ERROR);
        qInstallMessageHandler([](QtMsgType, const QMessageLogContext &, const QString &) {});
    }

    AVBufferRef *hwDev = nullptr;
#if defined(Q_OS_WIN)
    if (g_opt.mode == Mode::D3d11va) {
        // Exactly what VideoDecoder does for non-ProRes codecs: FFmpeg
        // creates its own D3D11 device + texture-array pool; frames come
        // back to the CPU via av_hwframe_transfer_data (readback forced).
        if (av_hwdevice_ctx_create(&hwDev, AV_HWDEVICE_TYPE_D3D11VA, nullptr, nullptr, 0) < 0) {
            std::fprintf(stderr, "av_hwdevice_ctx_create(d3d11va) failed\n");
            return 1;
        }
    } else if (g_opt.mode != Mode::Sw) {
        if (!qcv::VulkanDeviceManager::instance().initialize()) {
            std::fprintf(stderr, "VulkanDeviceManager init failed\n");
            return 1;
        }
        hwDev = qcv::createSharedVulkanHwDeviceCtx();
        if (!hwDev) { std::fprintf(stderr, "createSharedVulkanHwDeviceCtx failed\n"); return 1; }
    }
#else
    if (g_opt.mode != Mode::Sw) {
        std::fprintf(stderr, "Vulkan modes are Windows-only in this tool\n");
        return 2;
    }
#endif

    AVPacket *pkt     = av_packet_alloc();
    AVFrame  *frame   = av_frame_alloc();
    AVFrame  *swFrame = av_frame_alloc();

    Decoder dec;
    size_t fileIdx = 0;
    const auto tStart = Clock::now();
    auto tOpen = Clock::now();
    if (!dec.open(g_opt.files[fileIdx], hwDev)) {
        std::fprintf(stderr, "open failed: %s\n", g_opt.files[fileIdx].c_str());
        return 1;
    }

    int    frames        = 0;
    int    sinceOpen     = 0;
    int    opens         = 1;
    double firstFrameMs  = -1.0;
    double openLatencySum = 0.0;   // open→first frame, per open
    int    openLatencyN  = 0;
    bool   awaitingFirst = true;
    std::string firstPixFmt, firstSwFmt;
    int    w = 0, h = 0;

    while (frames < g_opt.frames) {
        const int rc = av_read_frame(dec.fmt, pkt);
        if (rc == AVERROR_EOF) {
            // Drain, then loop the file.
            avcodec_send_packet(dec.cctx, nullptr);
            while (avcodec_receive_frame(dec.cctx, frame) >= 0) {
                if (g_opt.readback && frame->hw_frames_ctx) {
                    av_hwframe_transfer_data(swFrame, frame, 0);
                    av_frame_unref(swFrame);
                }
                av_frame_unref(frame);
                ++frames; ++sinceOpen;
            }
            dec.rewind();
            continue;
        }
        if (rc < 0) { std::fprintf(stderr, "read error\n"); break; }
        if (pkt->stream_index != dec.vidx) { av_packet_unref(pkt); continue; }
        if (avcodec_send_packet(dec.cctx, pkt) < 0) { av_packet_unref(pkt); break; }
        av_packet_unref(pkt);
        while (avcodec_receive_frame(dec.cctx, frame) >= 0) {
            if (awaitingFirst) {
                const double ms = msSince(tOpen);
                if (firstFrameMs < 0) {
                    firstFrameMs = ms;
                    firstPixFmt  = av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format));
                    if (frame->hw_frames_ctx) {
                        auto *fc = reinterpret_cast<AVHWFramesContext *>(frame->hw_frames_ctx->data);
                        firstSwFmt = av_get_pix_fmt_name(fc->sw_format);
                    }
                    w = frame->width; h = frame->height;
                }
                openLatencySum += ms; ++openLatencyN;
                awaitingFirst = false;
            }
            if (g_opt.readback && frame->hw_frames_ctx) {
                if (av_hwframe_transfer_data(swFrame, frame, 0) < 0) {
                    std::fprintf(stderr, "readback failed\n");
                }
                av_frame_unref(swFrame);
            }
            av_frame_unref(frame);
            ++frames; ++sinceOpen;
            if (g_opt.reopen > 0 && sinceOpen >= g_opt.reopen && frames < g_opt.frames) {
                dec.close();
                fileIdx = (fileIdx + 1) % g_opt.files.size();
                tOpen = Clock::now();
                if (!dec.open(g_opt.files[fileIdx], hwDev)) {
                    std::fprintf(stderr, "reopen failed: %s\n", g_opt.files[fileIdx].c_str());
                    return 1;
                }
                ++opens; sinceOpen = 0; awaitingFirst = true;
                break;   // packet loop restarts on the new decoder
            }
        }
    }

#if defined(Q_OS_WIN)
    if (hwDev && g_opt.mode != Mode::D3d11va)
        qcv::VulkanDeviceManager::instance().waitForGpu();
#endif
    const double wallMs = msSince(tStart);

    std::printf("mode=%s threads=%d readback=%d reopen=%d files=%zu\n",
                modeName(g_opt.mode), g_opt.threads, g_opt.readback ? 1 : 0,
                g_opt.reopen, g_opt.files.size());
    std::printf("first frame: %dx%d fmt=%s%s%s  ttff=%.1f ms\n", w, h,
                firstPixFmt.c_str(), firstSwFmt.empty() ? "" : " sw=",
                firstSwFmt.c_str(), firstFrameMs);
    std::printf("frames=%d wall=%.0f ms  fps=%.1f  opens=%d  open->first=%.1f ms avg\n",
                frames, wallMs, frames * 1000.0 / wallMs, opens,
                openLatencyN ? openLatencySum / openLatencyN : 0.0);

    dec.close();
    av_frame_free(&swFrame);
    av_frame_free(&frame);
    av_packet_free(&pkt);
#if defined(Q_OS_WIN)
    if (hwDev) {
        const bool vulkan = (g_opt.mode != Mode::D3d11va);
        av_buffer_unref(&hwDev);
        if (vulkan) {
            qcv::releaseSharedVulkanFramesCache();
            qcv::VulkanDeviceManager::instance().shutdown();
        }
    }
#endif
    return 0;
}
