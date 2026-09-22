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
#include <fcntl.h>
#include <pthread/qos.h>
#include <sys/resource.h>
#elif defined(_WIN32)
// windows.h defines min/max as function-like macros, and this file calls
// std::min/std::max with plain arguments (the std::max<int64_t> ones are
// immune — the <> suppresses expansion, but four others are not). Every
// other file here that includes windows.h alongside such calls guards it
// the same way; this one did not, so the Windows build did not compile.
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#include <windows.h>
#endif

namespace qcv {

namespace {

constexpr int64_t kGapFill = 4 << 20;   // interleaved audio between video samples
constexpr int     kDefaultPageKB  = 1024; // LucidLink's cache page (`lucid3 cache`)
constexpr double  kDefaultSeconds = 2.0;
constexpr int64_t kDefaultCapMB   = 512;
constexpr uint32_t kNoGen = UINT32_MAX;

double secondsFromEnv()
{
    bool ok = false;
    const double v = qEnvironmentVariable("QCV_READAHEAD_SECONDS").toDouble(&ok);
    return ok ? std::max(0.0, v) : kDefaultSeconds;
}

int64_t pageBytesFromEnv()
{
    bool ok = false;
    const int v = qEnvironmentVariableIntValue("QCV_READAHEAD_PAGE_KB", &ok);
    return int64_t(ok && v > 0 ? v : kDefaultPageKB) << 10;
}

int64_t loopCapBytesFromEnv()
{
    bool ok = false;
    const int v = qEnvironmentVariableIntValue("QCV_READAHEAD_LOOP_GB", &ok);
    return int64_t(ok && v > 0 ? v : 16) << 30;
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
    // The loop range to hydrate whole, or first < 0 for none.
    std::atomic<int>      rangeFirst{-1};
    std::atomic<int>      rangeLast{-1};
    std::atomic<uint32_t> rangeGen{0};

    // Worker thread only.
    uint32_t seenGen  = kNoGen;
    int      frontier = 0;
    bool     failed   = false;
    std::unique_ptr<QFile> file;
    Clock::time_point windowStart;
    int64_t  windowBytes  = 0;
    int64_t  lastPage     = -1;   // frames share boundary pages; touch once
    bool     windowLogged = true;
    int      windowLogs   = 0;
    int64_t  bytes = 0, lastLogBytes = 0;
    double   readSecs = 0.0;
    int      restarts = 0;
    // Loop-range pass (worker only).
    uint32_t rangeSeenGen   = kNoGen;
    int      rangeFrontier  = 0;
    int      rangeEnd       = 0;     // one past the last frame, 0 = none
    int64_t  rangeLastPage  = -1;
    int64_t  rangeBytes     = 0;
    Clock::time_point rangeStart;
    bool     rangeLogged    = true;
};

struct ReadAhead::Impl {
    const double  seconds  = secondsFromEnv();
    const int64_t capBytes = capBytesFromEnv();
    const int64_t pageBytes = pageBytesFromEnv();
    const int64_t loopCapBytes = loopCapBytesFromEnv();

    std::mutex mutex;
    std::condition_variable cv;
    std::unordered_map<Id, std::shared_ptr<Client>> clients;
    Id   nextId = 1;
    bool workPending = false;
    bool stop = false;
    std::thread thread;

    void run();
    bool prepare(Client &c);
    bool prepareRange(Client &c);
    bool openFile(Client &c);
    bool touchFrame(Client &c, int frame, int64_t &lastPage,
                    const std::function<bool()> &abandon);
    void warmOne(Client &c);
    void warmRangeOne(Client &c);
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
        qInfo("ReadAhead: %.1f s window, %lld MB cap, 1 byte per %lld KB page "
              "(QCV_READAHEAD_SECONDS / _MB / _PAGE_KB)",
              d->seconds, static_cast<long long>(d->capBytes >> 20),
              static_cast<long long>(d->pageBytes >> 10));
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

void ReadAhead::setRange(Id id, int firstFrame, int lastFrame)
{
    if (id == 0) return;
    {
        std::lock_guard<std::mutex> lk(d->mutex);
        auto it = d->clients.find(id);
        if (it == d->clients.end()) return;
        Client &c = *it->second;
        const bool none = firstFrame < 0 || lastFrame < firstFrame;
        const int f = none ? -1 : firstFrame;
        const int l = none ? -1 : lastFrame;
        if (c.rangeFirst.load() == f && c.rangeLast.load() == l) return;
        c.rangeFirst.store(f, std::memory_order_relaxed);
        c.rangeLast.store(l, std::memory_order_relaxed);
        c.rangeGen.fetch_add(1, std::memory_order_release);
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
        if (best) {
            warmOne(*best);
        } else {
            // Windows are all warm: spend idle time hydrating loop ranges.
            for (auto &c : snapshot) {
                if (!prepareRange(*c)) continue;
                best = c.get();
                warmRangeOne(*best);
                break;
            }
        }

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
        c.lastPage     = -1;
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

// Worker only. Applies a pending loop-range change and says whether any of
// the range is still to be touched.
bool ReadAhead::Impl::prepareRange(Client &c)
{
    if (c.failed || c.detached.load(std::memory_order_acquire)) return false;
    const uint32_t g = c.rangeGen.load(std::memory_order_acquire);
    if (g != c.rangeSeenGen) {
        c.rangeSeenGen = g;
        const int first = c.rangeFirst.load(std::memory_order_relaxed);
        const int last  = c.rangeLast.load(std::memory_order_relaxed);
        const int n     = static_cast<int>(c.spans.size());
        if (first < 0 || first >= n) {
            c.rangeEnd = 0;
        } else {
            c.rangeFrontier = first;
            c.rangeEnd      = std::min(last, n - 1) + 1;
        }
        c.rangeLastPage = -1;
        c.rangeBytes    = 0;
        c.rangeStart    = Clock::now();
        c.rangeLogged   = false;
    }
    if (c.rangeFrontier < c.rangeEnd && c.rangeBytes < loopCapBytes) return true;

    if (!c.rangeLogged && c.rangeBytes > 0) {
        c.rangeLogged = true;
        const double s = secondsSince(c.rangeStart);
        if (c.rangeFrontier < c.rangeEnd) {
            qInfo("ReadAhead: %s — loop range stopped at the %lld GB cap "
                  "(QCV_READAHEAD_LOOP_GB), frame %d",
                  qPrintable(c.name), static_cast<long long>(loopCapBytes >> 30),
                  c.rangeFrontier);
        } else {
            qInfo("ReadAhead: %s — loop range %d-%d touched, %.1f MB in %.2f s "
                  "(%.1f MB/s)",
                  qPrintable(c.name), c.rangeFirst.load(), c.rangeLast.load(),
                  c.rangeBytes / 1e6, s, s > 0 ? c.rangeBytes / 1e6 / s : 0.0);
        }
    }
    c.rangeLogged = true;
    return false;
}

bool ReadAhead::Impl::openFile(Client &c)
{
    if (c.file) return true;
    c.file = std::make_unique<QFile>(c.path);
    if (!c.file->open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
        qWarning("ReadAhead: %s — cannot open (%s); read-ahead off for it",
                 qPrintable(c.name), qPrintable(c.file->errorString()));
        c.failed = true;
        return false;
    }
#if defined(__APPLE__)
    // The point is the volume's cache, not ours: keep macOS from
    // holding the touched pages in RAM.
    fcntl(c.file->handle(), F_NOCACHE, 1);
#endif
    return true;
}

// Worker only. Touches one frame's span: one byte in every page it covers,
// which makes the volume fetch and cache each page (measured on LucidLink,
// whose cache page is 1 MiB: its on-disk cache grew by the file's size).
// Returns false if abandoned partway (a jump, a range change, a detach).
bool ReadAhead::Impl::touchFrame(Client &c, int frame, int64_t &lastPage,
                                 const std::function<bool()> &abandon)
{
    const FrameSpan span = c.spans[static_cast<size_t>(frame)];
    if (span.pos < 0 || span.len <= 0) return true;
    const int64_t first = span.pos / pageBytes;
    const int64_t last  = (span.pos + span.len - 1) / pageBytes;
    for (int64_t p = first; p <= last; ++p) {
        if (p == lastPage) continue;
        if (c.detached.load(std::memory_order_acquire) || abandon()) return false;
        char b;
        const auto t = Clock::now();
        const bool ok = c.file->seek(p * pageBytes) && c.file->read(&b, 1) == 1;
        c.readSecs += secondsSince(t);
        if (!ok) break;
        lastPage = p;
    }
    c.bytes += span.len;
    return true;
}

// Worker only. The playhead window: next frame from the frontier.
void ReadAhead::Impl::warmOne(Client &c)
{
    if (!openFile(c)) return;
    const uint32_t g = c.seenGen;
    if (!touchFrame(c, c.frontier, c.lastPage, [&] {
            return c.gen.load(std::memory_order_acquire) != g;
        })) {
        return;   // the frontier stays put; prepare() restarts it
    }
    c.windowBytes += std::max<int64_t>(0, c.spans[static_cast<size_t>(c.frontier)].len);
    ++c.frontier;

    if (c.bytes - c.lastLogBytes >= (512LL << 20)) {
        c.lastLogBytes = c.bytes;
        logStats(c, "so far");
    }
}

// Worker only. The loop range: next frame from the range frontier. Yields
// to the window after every frame (run() checks windows first).
void ReadAhead::Impl::warmRangeOne(Client &c)
{
    if (!openFile(c)) return;
    const uint32_t rg = c.rangeSeenGen;
    const uint32_t wg = c.seenGen;
    if (!touchFrame(c, c.rangeFrontier, c.rangeLastPage, [&] {
            return c.rangeGen.load(std::memory_order_acquire) != rg
                || c.gen.load(std::memory_order_acquire) != wg;
        })) {
        return;
    }
    c.rangeBytes += std::max<int64_t>(0, c.spans[static_cast<size_t>(c.rangeFrontier)].len);
    ++c.rangeFrontier;
}

void ReadAhead::Impl::logStats(const Client &c, const char *when)
{
    qInfo("ReadAhead: %s — %s %.0f MB touched, %.1f MB/s while touching, %d restarts",
          qPrintable(c.name), when, c.bytes / 1e6,
          c.readSecs > 0 ? c.bytes / 1e6 / c.readSecs : 0.0, c.restarts);
}

} // namespace qcv
