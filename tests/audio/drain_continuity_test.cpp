// Standalone (not wired into CMake). Build + run on macOS:
//   QT=~/Qt/6.11.1/macos; clang++ -std=c++20 -O1 -Isrc -F$QT/lib \
//     -I$QT/lib/QtCore.framework/Headers -framework QtCore -Wl,-rpath,$QT/lib \
//     tests/audio/drain_continuity_test.cpp -o /tmp/drain_test && /tmp/drain_test
// Exit 0 = every scripted event stayed within the clean-sine jump bound.

// Continuity harness for AudioDrainStage: a fake IAudioSource backed by
// the real AudioRingBuffer, producing a 1 kHz sine; scripted seek /
// underrun / hold / skip events; measures the largest sample-to-sample
// jump in the output. A clean sine at 48 kHz has |delta| <= 0.131
// (amplitude 1). Fades keep every event under that; a hard cut shows
// up as a jump near the signal amplitude.
#include "audio/audio_drain_stage.h"
#include "audio/audio_ring_buffer.h"
#include <cmath>
#include <cstdio>
#include <vector>

using namespace qcv;

struct FakeSource : IAudioSource {
    AudioRingBuffer ring{192000};
    AudioFormat fmt;
    bool pending = false;
    double phase = 0.0;
    // Producer: write n frames of sine.
    void produce(std::size_t n) {
        std::vector<float> buf(n * 2);
        for (std::size_t i = 0; i < n; ++i) {
            const float v = std::sin(phase);
            phase += 2.0 * M_PI * 1000.0 / 48000.0;
            buf[i*2] = v; buf[i*2+1] = v;
        }
        ring.write(buf.data(), n * 2 * sizeof(float));
    }
    void requestSeek() { pending = true; }
    void serviceSeek() { ring.markStale(); phase += 1.7; pending = false; }  // phase jump = new content
    bool open(const QString &) override { return true; }
    void close() override {}
    void start() override {}
    void stop() override {}
    bool hasAudio() const override { return true; }
    bool isOpen() const override { return true; }
    bool isRunning() const override { return true; }
    double duration() const override { return 1e9; }
    void seek(double) override {}
    std::size_t read(float *out, std::size_t frames) override {
        const std::size_t got = ring.read(out, frames * 8) / 8;
        if (got < frames) std::memset(out + got*2, 0, (frames-got)*8);
        return got;
    }
    const AudioFormat &format() const override { return fmt; }
    double secondsSinceLastSeek() const override { return 10; }
    bool seekPending() const override { return pending; }
    uint32_t flushGeneration() const override { return ring.staleGeneration(); }
    std::size_t staleFrames() const override { return ring.staleBytes() / 8; }
    void discardStale() override { ring.discardStale(); }
    void setRoutingMode(int) override {}
    int routingMode() const override { return 0; }
    void setTempo(double) override {}
    double tempo() const override { return 1.0; }
    int sourceChannels() const override { return 2; }
    QString sourceChannelLayoutName() const override { return {}; }
    std::array<float,16> peakLevels() const override { return {}; }
    QStringList sourceChannelNames() const override { return {}; }
};

int main() {
    FakeSource src;
    AudioDrainStage stage;
    AudioDrainStage::Request req;
    std::atomic<uint64_t> consumed{0};
    const std::size_t block = 480;
    std::vector<float> out(block*2), scratch(block*4);
    std::vector<float> all;
    float prev = 0.0f; double maxJump = 0; int worstBlock = -1;
    bool produce = true;
    auto run = [&](int nBlocks, double ratio, bool extraHold, const char *label) {
        double localMax = 0;
        for (int b = 0; b < nBlocks; ++b) {
            if (produce && src.ring.freshBytes() < 38400) src.produce(2048);   // ~100 ms depth
            stage.process(src, out.data(), block, ratio, scratch.data(), block*2, consumed, req, extraHold);
            for (std::size_t i = 0; i < block; ++i) {
                const double j = std::fabs(out[i*2] - prev);
                if (j > localMax) localMax = j;
                prev = out[i*2];
                all.push_back(out[i*2]);
            }
        }
        if (localMax > maxJump) { maxJump = localMax; }
        std::printf("%-34s blocks=%3d maxJump=%.3f%s\n", label, nBlocks, localMax, localMax > 0.2 ? "  <-- CLICK" : "");
        return localMax;
    };
    req.restart = true;
    run(20, 1.0, false, "steady");
    run(20, 1.015, false, "steady ratio 1.015");
    run(20, 0.985, false, "steady ratio 0.985");
    // --- seek: request, play stale for a block, service, then continue
    src.requestSeek();
    run(1, 1.0, false, "seek pending (stale rolling)");
    src.serviceSeek();
    run(1, 1.0, false, "seek landed (stale fade-out)");
    run(10, 1.0, false, "post-seek (fade-in)");
    // --- seek serviced between callbacks
    src.requestSeek(); src.serviceSeek();
    run(12, 1.0, false, "seek serviced between blocks");
    // --- underrun: drain the ring dry then refill
    produce = false;
    run(12, 1.0, false, "producer stalls (drains dry)");
    run(2, 1.0, false, "underrun (empty blocks)");
    produce = true;
    run(10, 1.0, false, "recovery (fade-in)");
    // --- hold / release
    req.hold = true;
    run(3, 1.0, false, "hold (fade-out then silence)");
    req.hold = false;
    run(10, 1.0, false, "release (fade-in)");
    // --- skip request
    req.skipFrames = 1000;
    run(6, 1.0, false, "skip 1000 frames");
    // --- pause via extraHold then restart
    run(3, 1.0, true, "pause (extraHold)");
    req.restart = true;
    run(10, 1.0, false, "play again (restart)");
    std::printf("overall maxJump=%.3f  (clean sine bound 0.131)  consumed=%llu\n", maxJump, (unsigned long long)consumed.load());
    return maxJump > 0.2 ? 1 : 0;
}
