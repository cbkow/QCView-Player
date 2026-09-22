#include "read_ahead.h"

#include <QFile>
#include <QFileInfo>
#include <QtLogging>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_map>

extern "C" {
#include <libavformat/avformat.h>
}

#if defined(__APPLE__)
#include <pthread/qos.h>
#include <sys/resource.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

namespace qcv {

namespace {

constexpr int64_t kChunk   = 2 << 20;   // matches the SMB client's I/O size
constexpr int64_t kGapFill = 4 << 20;   // interleaved audio between video samples
constexpr double  kDefaultSeconds = 2.0;
constexpr int64_t kDefaultCapMB   = 512;
constexpr uint32_t kNoGen = UINT32_MAX;

double secondsFromEnv()
{
    bool ok = false;
    const double v = qEnvironmentVariable("QCV_READAHEAD_SECONDS").toDouble(&ok);
    return ok ? std::max(0.0, v) : kDefaultSeconds;
}

int64_t capBytesFromEnv()
{
    bool ok = false;
    const int v = qEnvironmentVariableIntValue("QCV_READAHEAD_MB", &ok);
    return (ok && v > 0 ? int64_t(v) : kDefaultCapMB) << 20;
}

using Clock = std::chrono::steady_clock;

double secondsSince(Clock::time_point t)
{
    return std::chrono::duration<double>(Clock::now() - t).count();
}

} // namespace

struct Client {
    ReadAhead::Id id = 0;
    QString path;
    QString name;
    std::vector<FrameSpan> spans;
    int maxFrames = 0;   // the time window in this file's frames

    // Written by the decoders (any thread).
    std::atomic<int>      position{0};
    std::atomic<uint32_t> gen{0};       // bumped on a jump
    std::atomic<bool>     detached{false};

    // Worker thread only.
    uint32_t seenGen  = kNoGen;
    int      frontier = 0;
    bool     failed   = false;
    std::unique_ptr<QFile> file;
    Clock::time_point windowStart;
    int64_t  windowBytes  = 0;
    bool     windowLogged = true;
    int      windowLogs   = 0;
    int64_t  bytes = 0, lastLogBytes = 0;
    double   readSecs = 0.0;
    int      restarts = 0;
};

struct ReadAhead::Impl {
    const double  seconds  = secondsFromEnv();
    const int64_t capBytes = capBytesFromEnv();

    std::mutex mutex;
    std::condition_variable cv;
    std::unordered_map<Id, std::shared_ptr<Client>> clients;
    Id   nextId = 1;
    bool workPending = false;
    bool stop = false;
    std::thread thread;

    void run();
    bool prepare(Client &c);
    void warmOne(Client &c, std::vector<char> &buf);
    static void logStats(const Client &c, const char *when);
};

ReadAhead &ReadAhead::instance()
{
    static ReadAhead ra;
    return ra;
}

ReadAhead::ReadAhead() : d(std::make_unique<Impl>())
{
    if (d->seconds > 0.0) {
        qInfo("ReadAhead: %.1f s window, %lld MB cap "
              "(QCV_READAHEAD_SECONDS / QCV_READAHEAD_MB)",
              d->seconds, static_cast<long long>(d->capBytes >> 20));
        d->thread = std::thread([this] { d->run(); });
    } else {
        qInfo("ReadAhead: off (QCV_READAHEAD_SECONDS=0)");
    }
}

ReadAhead::~ReadAhead()
{
    {
        std::lock_guard<std::mutex> lk(d->mutex);
        d->stop = true;
    }
    d->cv.notify_all();
    if (d->thread.joinable()) d->thread.join();
}

std::vector<FrameSpan> ReadAhead::spansFromIndex(
    AVFormatContext *fmt, int streamIdx,
    const std::function<int(int64_t)> &tsToFrame, int frameCount)
{
    if (!fmt || streamIdx < 0 || streamIdx >= static_cast<int>(fmt->nb_streams)
        || frameCount <= 0) {
        return {};
    }
    AVStream *st = fmt->streams[streamIdx];
    const int n = avformat_index_get_entries_count(st);
    if (n <= 0) return {};

    struct Entry { int64_t pos, end; int frame; };
    std::vector<Entry> entries;
    entries.reserve(n);
    for (int i = 0; i < n; ++i) {
        const AVIndexEntry *e = avformat_index_get_entry(st, i);
        if (!e || e->pos < 0 || e->size <= 0) continue;
        const int f = tsToFrame(e->timestamp);
        if (f < 0 || f >= frameCount) continue;
        entries.push_back({e->pos, e->pos + e->size, f});
    }
    if (entries.empty()) return {};

    // In file order, extend each sample over a small gap to the next video
    // sample: in an interleaved file that gap is the audio for the frame.
    std::sort(entries.begin(), entries.end(),
              [](const Entry &a, const Entry &b) { return a.pos < b.pos; });
    std::vector<FrameSpan> spans(static_cast<size_t>(frameCount));
    for (size_t k = 0; k < entries.size(); ++k) {
        int64_t end = entries[k].end;
        if (k + 1 < entries.size()) {
            const int64_t next = entries[k + 1].pos;
            if (next > end && next - end <= kGapFill) end = next;
        }
        FrameSpan &s = spans[static_cast<size_t>(entries[k].frame)];
        if (s.pos < 0) {
            s.pos = entries[k].pos;
            s.len = end - entries[k].pos;
        }
    }
    return spans;
}

ReadAhead::Id ReadAhead::attach(const QString &path, std::vector<FrameSpan> spans,
                                double fps)
{
    if (d->seconds <= 0.0 || spans.empty()) return 0;
    auto c = std::make_shared<Client>();
    c->path  = path;
    c->name  = QFileInfo(path).fileName();
    c->spans = std::move(spans);
    c->maxFrames = std::max(1, static_cast<int>(
        std::ceil(d->seconds * (fps > 0.0 ? fps : 24.0))));
    {
        std::lock_guard<std::mutex> lk(d->mutex);
        c->id = d->nextId++;
        d->clients.emplace(c->id, c);
        d->workPending = true;
    }
    d->cv.notify_one();
    return c->id;
}

void ReadAhead::detach(Id id)
{
    if (id == 0) return;
    std::lock_guard<std::mutex> lk(d->mutex);
    auto it = d->clients.find(id);
    if (it == d->clients.end()) return;
    // The worker may be mid-read on it; it holds its own reference and
    // checks this flag between chunks.
    it->second->detached.store(true, std::memory_order_release);
    d->clients.erase(it);
}

void ReadAhead::setPosition(Id id, int nextFrame)
{
    if (id == 0) return;
    {
        std::lock_guard<std::mutex> lk(d->mutex);
        auto it = d->clients.find(id);
        if (it == d->clients.end()) return;
        Client &c = *it->second;
        const int prev = c.position.exchange(nextFrame, std::memory_order_acq_rel);
        // Playback moves one frame at a time. A step back, or a step past
        // the window, is a jump: restart the window from the new position.
        if (nextFrame < prev || nextFrame > prev + c.maxFrames)
            c.gen.fetch_add(1, std::memory_order_acq_rel);
        d->workPending = true;
    }
    d->cv.notify_one();
}

void ReadAhead::Impl::run()
{
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    setiopolicy_np(IOPOL_TYPE_DISK, IOPOL_SCOPE_THREAD, IOPOL_UTILITY);
#elif defined(_WIN32)
    // Background mode lowers both CPU and I/O priority for this thread.
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
#endif
    std::vector<char> buf(static_cast<size_t>(kChunk));

    std::unique_lock<std::mutex> lk(mutex);
    while (!stop) {
        workPending = false;
        std::vector<std::shared_ptr<Client>> snapshot;
        snapshot.reserve(clients.size());
        for (auto &kv : clients) snapshot.push_back(kv.second);
        lk.unlock();

        // Most urgent first: the client whose next unwarmed frame is
        // closest to where its decoder is reading.
        Client *best = nullptr;
        int bestLead = INT_MAX;
        for (auto &c : snapshot) {
            if (!prepare(*c)) continue;
            const int lead = c->frontier - c->position.load(std::memory_order_acquire);
            if (lead < bestLead) { bestLead = lead; best = c.get(); }
        }
        if (best) warmOne(*best, buf);

        lk.lock();
        if (!best) {
            cv.wait(lk, [this] { return stop || workPending; });
        }
    }
}

// Worker only. Applies a pending restart and says whether there is a frame
// to warm.
bool ReadAhead::Impl::prepare(Client &c)
{
    if (c.failed || c.detached.load(std::memory_order_acquire)) return false;
    const uint32_t g  = c.gen.load(std::memory_order_acquire);
    const int     pos = c.position.load(std::memory_order_acquire);
    if (g != c.seenGen) {
        if (c.seenGen != kNoGen) ++c.restarts;
        c.seenGen      = g;
        c.frontier     = pos;
        c.windowStart  = Clock::now();
        c.windowBytes  = 0;
        c.windowLogged = false;
    }
    if (c.frontier < pos) c.frontier = pos;   // the decoder got there first

    // Has the window [pos, pos + seconds) been warmed, short of the byte
    // cap? Walked from pos each time: at most maxFrames spans.
    const int last = std::min(pos + c.maxFrames, static_cast<int>(c.spans.size()));
    int64_t bytes = 0;
    int end = pos;
    while (end < last) {
        const int64_t len = std::max<int64_t>(0, c.spans[static_cast<size_t>(end)].len);
        if (end > pos && bytes + len > capBytes) break;   // always at least one
        bytes += len;
        ++end;
    }
    if (c.frontier < end) return true;

    // Window complete. Log how long a fresh window took, a few times per
    // file: the number that says whether this keeps up.
    if (!c.windowLogged && c.windowBytes > 0 && c.windowLogs < 4) {
        c.windowLogged = true;
        ++c.windowLogs;
        const double s = secondsSince(c.windowStart);
        qInfo("ReadAhead: %s — window warm, %.1f MB in %.2f s (%.1f MB/s)",
              qPrintable(c.name), c.windowBytes / 1e6, s,
              s > 0 ? c.windowBytes / 1e6 / s : 0.0);
    }
    c.windowLogged = true;
    return false;
}

// Worker only. Reads one frame's span in chunks, abandoning it on a jump or
// a detach (the frontier then stays put and prepare() restarts it).
void ReadAhead::Impl::warmOne(Client &c, std::vector<char> &buf)
{
    if (!c.file) {
        c.file = std::make_unique<QFile>(c.path);
        if (!c.file->open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
            qWarning("ReadAhead: %s — cannot open (%s); read-ahead off for it",
                     qPrintable(c.name), qPrintable(c.file->errorString()));
            c.failed = true;
            return;
        }
    }
    const FrameSpan span = c.spans[static_cast<size_t>(c.frontier)];
    const uint32_t g = c.seenGen;
    int64_t off = span.pos;
    int64_t remaining = span.pos >= 0 ? span.len : 0;
    while (remaining > 0) {
        if (c.detached.load(std::memory_order_acquire)
            || c.gen.load(std::memory_order_acquire) != g) {
            return;
        }
        const int64_t n = std::min(remaining, kChunk);
        const auto t = Clock::now();
        if (!c.file->seek(off)) break;
        const qint64 r = c.file->read(buf.data(), n);
        c.readSecs += secondsSince(t);
        if (r <= 0) break;
        off += r;
        remaining -= r;
        c.bytes += r;
        c.windowBytes += r;
    }
    ++c.frontier;

    if (c.bytes - c.lastLogBytes >= (512LL << 20)) {
        c.lastLogBytes = c.bytes;
        logStats(c, "so far");
    }
}

void ReadAhead::Impl::logStats(const Client &c, const char *when)
{
    qInfo("ReadAhead: %s — %s %.0f MB warmed, %.1f MB/s while reading, %d restarts",
          qPrintable(c.name), when, c.bytes / 1e6,
          c.readSecs > 0 ? c.bytes / 1e6 / c.readSecs : 0.0, c.restarts);
}

} // namespace qcv
