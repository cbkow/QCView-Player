// LiveAudioSource over a fake QCBridgeAE audio segment: the fold for 2, 6
// and 8 channels against computeRoutingMatrix, continuity of what read()
// delivers (a counter in the samples), the flush on a new session, the
// scrub mute, the depth trim, and recovery after an overrun. Standalone,
// like drain_continuity_test.cpp:
//
//   QT=~/Qt/6.11.1/macos; clang++ -std=c++20 -O1 -Isrc -F$QT/lib \
//     -I$QT/lib/QtCore.framework/Headers -framework QtCore -Wl,-rpath,$QT/lib \
//     tests/audio/live_audio_source_test.cpp src/audio/live_audio_source.cpp \
//     src/decode/qcbae/audio_ring.cpp src/decode/qcbae/shared_ring.cpp \
//     -o /tmp/live_audio_test && /tmp/live_audio_test

#include "audio/live_audio_source.h"
#include "audio/audio_routing_matrix.h"
#include "decode/qcbae/audio_ring.h"

#include <QCoreApplication>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

using namespace qcv;

namespace {
int failures = 0;
void check(bool cond, const char *what)
{
    std::printf("%-64s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond) ++failures;
}
constexpr int64_t kTicks = 254016000000ll;

// The segment a Premiere device would create, for qcbae://probe.
struct Producer {
    qcbae::AudioRing ring;
    uint32_t channels = 2;
    int64_t  time = 0;
    uint64_t frameCounter = 0;
    bool create() { return ring.create("/qcbae-probe-audio", kTicks, 32, 256, 16); }
    void session(uint32_t ch, bool scrubbing = false) {
        channels = ch;
        qcbae::AudioSession s {};
        s.sample_rate = 48000; s.channels = ch; s.push_frames = 256; s.speed = 1.0f;
        s.flags = scrubbing ? qcbae::kAudioSessionScrubbing : 0u;
        ring.begin_session(s);
        ring.set_host_audio(qcbae::HostAudio::On);
    }
    // Each channel c carries (counter + c * 1000) per frame so the fold
    // can be checked exactly; `scale` sets the amplitude.
    void push(uint32_t frames, float scale = 1e-6f, uint32_t flags = 0) {
        std::vector<std::vector<float>> planes(channels, std::vector<float>(frames));
        std::vector<const float *> ptrs;
        for (uint32_t c = 0; c < channels; ++c) {
            for (uint32_t i = 0; i < frames; ++i)
                planes[c][i] = (static_cast<float>(frameCounter + i) + c * 1000.0f) * scale;
            ptrs.push_back(planes[c].data());
        }
        ring.push(ptrs.data(), channels, 48000, frames, time, flags);
        frameCounter += frames;
        time += static_cast<int64_t>(frames) * kTicks / 48000;
    }
};

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);

    Producer p;
    check(p.create(), "fake segment created");

    LiveAudioSource src;
    check(src.open(QStringLiteral("qcbae://probe")), "opens for a qcbae:// URL");
    src.start();
    check(src.isLive() && src.duration() == 0.0 && !src.seekPending(), "live: no timeline");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));   // the reader opens the segment
    check(!src.hasAudio(), "no audio before a session");

    // --- stereo pass-through and continuity ------------------------------
    p.session(2);
    for (int i = 0; i < 8; ++i) p.push(256);
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    check(src.hasAudio() && src.sourceChannels() == 2 && src.sampleRate() == 48000, "session facts arrive");
    {
        std::vector<float> out(2048 * 2);
        src.discardStale();
        const size_t got = src.read(out.data(), 2048);
        check(got == 2048, "read delivers the queued frames");
        bool contiguous = true, folded = true;
        const float first = out[0] / 1e-6f;
        for (size_t i = 0; i < got; ++i) {
            const float l = out[i * 2] / 1e-6f, r = out[i * 2 + 1] / 1e-6f;
            if (std::fabs(l - (first + i)) > 0.5f) contiguous = false;
            if (std::fabs(r - (first + i + 1000.0f)) > 0.5f) folded = false;
        }
        check(contiguous, "stereo frames are contiguous");
        check(folded, "stereo passes L and R through");
        const size_t more = src.read(out.data(), 256);
        check(more == 0, "an empty queue delivers nothing (padded with silence)");
    }

    // --- a new session flushes what was queued -----------------------------
    for (int i = 0; i < 4; ++i) p.push(256);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    const uint32_t genBefore = src.flushGeneration();
    p.session(2);
    p.push(256);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    check(src.flushGeneration() != genBefore && src.staleFrames() == 1024, "a new session marks the old queue stale");
    src.discardStale();
    {
        std::vector<float> out(256 * 2);
        check(src.read(out.data(), 256) == 256, "post-session audio is readable after discard");
    }

    // --- 6 channels: Auto folds with the BS.775 coefficients ------------------
    p.session(6);
    p.push(256);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    src.discardStale();
    {
        std::vector<float> out(256 * 2);
        const size_t got = src.read(out.data(), 256);
        check(got == 256 && src.sourceChannels() == 6, "6-channel session reaches the fold");
        // frame i, channel c = (n + i + c*1000) * scale; L = ch0 + .707 ch2 + .707 ch4
        const float n = (out[0] / 1e-6f) / (1.0f + 0.707f + 0.707f) - (0.707f * 2000.0f + 0.707f * 4000.0f) / (1.0f + 0.707f + 0.707f);
        bool ok = true;
        for (size_t i = 0; i < got; ++i) {
            const float base = n + i;
            const float L = base + 0.707f * (base + 2000.0f) + 0.707f * (base + 4000.0f);
            const float R = (base + 1000.0f) + 0.707f * (base + 2000.0f) + 0.707f * (base + 5000.0f);
            if (std::fabs(out[i * 2] / 1e-6f - L) > 1.0f || std::fabs(out[i * 2 + 1] / 1e-6f - R) > 1.0f) { ok = false; break; }
        }
        check(ok, "Auto on 6 channels is the BS.775 fold (LFE dropped)");
        const auto names = src.sourceChannelNames();
        check(names.size() == 6 && names[3] == QStringLiteral("LFE"), "6-channel names are L R C LFE Ls Rs");
    }

    // --- 8 channels: Auto picks the 7-8 bounce; Stereo7_8 the same; Downmix5_1 the fold
    p.session(8);
    p.push(256);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    src.discardStale();
    {
        std::vector<float> out(256 * 2);
        const size_t got = src.read(out.data(), 256);
        const auto m = computeRoutingMatrix(0, 8);
        check(got == 256 && m.has_value(), "8-channel Auto uses computeRoutingMatrix");
        bool ok = true;
        for (size_t i = 0; i < got && ok; ++i) {
            const float base = (out[i * 2] / 1e-6f) - 6000.0f;   // ch6 carries base + 6000
            if (std::fabs(out[i * 2 + 1] / 1e-6f - (base + 7000.0f)) > 1.0f) ok = false;
        }
        check(ok, "8-channel Auto bounces channels 7-8 to L/R");
        src.setRoutingMode(1);   // Downmix5_1
        p.push(256);
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        src.discardStale();
        const size_t got2 = src.read(out.data(), 256);
        ok = got2 == 256;
        for (size_t i = 0; i < got2 && ok; ++i) {
            // L = ch0 + .707 ch2 + .707 ch4 with ch_c = base + c*1000: ch0 recovered from L
            const float L = out[i * 2] / 1e-6f;
            const float base = (L - 0.707f * 2000.0f - 0.707f * 4000.0f) / (1.0f + 0.707f + 0.707f);
            const float R = (base + 1000.0f) + 0.707f * (base + 2000.0f) + 0.707f * (base + 5000.0f);
            if (std::fabs(out[i * 2 + 1] / 1e-6f - R) > 1.0f) ok = false;
        }
        check(ok, "Downmix5_1 on 8 channels folds channels 1-6");
        src.setRoutingMode(0);
    }

    // --- scrub mute -------------------------------------------------------------
    LiveAudioSource::setGlobalScrubMute(true);
    p.session(2, /*scrubbing=*/true);
    p.push(256, 1e-6f, qcbae::kAudioPacketScrubbing);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    src.discardStale();
    {
        std::vector<float> out(256 * 2, 1.0f);
        const size_t got = src.read(out.data(), 256);
        bool silent = got == 256;
        for (size_t i = 0; i < got * 2; ++i) if (out[i] != 0.0f) silent = false;
        check(silent, "scrub packets are silent while scrub audio is muted");
    }
    LiveAudioSource::setGlobalScrubMute(false);

    // --- depth trim: far more than the target queued, the excess is dropped -------
    LiveAudioSource::setGlobalBufferMs(40);
    p.session(2);
    for (int i = 0; i < 60; ++i) { p.push(256); std::this_thread::sleep_for(std::chrono::milliseconds(1)); }
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    src.discardStale();
    {
        std::vector<float> out(48000 * 2);
        const size_t got = src.read(out.data(), 48000);
        // 60 packets = 15360 frames offered; target 40 ms = 1920 frames + 2 packets.
        check(got > 0 && got <= 1920 + 3 * 256, "the queue is held near the depth target");
    }

    // --- overrun: the reader lapped, then recovers --------------------------------
    for (int i = 0; i < 200; ++i) p.push(256);   // 32 slots: the reader cannot keep up
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    src.discardStale();
    {
        std::vector<float> out(4096 * 2);
        const size_t got = src.read(out.data(), 4096);
        check(got > 0, "after an overrun the reader resyncs and delivers again");
        p.push(256);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        check(src.read(out.data(), 256) == 256, "and keeps up afterwards");
    }

    // --- taps see the planar packet --------------------------------------------------
    {
        int seen = 0; uint32_t ch = 0;
        const int id = src.addTap([&](const qcbae::AudioPacketDesc &d, const float *const *planes,
                                      const qcbae::AudioSession &) {
            ++seen; ch = d.channels; (void)planes;
        });
        p.push(256);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        src.removeTap(id);
        p.push(256);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        check(seen == 1 && ch == 2, "a tap receives each packet until removed");
    }

    // --- the producer goes away ---------------------------------------------------------
    p.ring.set_state(qcbae::AudioState::Retired);
    p.ring = qcbae::AudioRing();   // unlinks
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    check(!src.hasAudio(), "a retired, unlinked segment reads as no audio");

    src.close();
    check(!src.isOpen(), "closes");
    std::printf("\n%s (%d failure%s)\n", failures == 0 ? "PASS" : "FAIL", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
