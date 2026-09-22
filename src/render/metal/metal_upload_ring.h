// MetalUploadRing — CPU frames uploaded into a few textures in turn, so an
// upload never overwrites a texture that a frame still in flight samples.
//
// The single view and the dual compositor used to replaceRegion each new
// CPU frame into one cached shared texture, with up to three drawables in
// flight and no wait: an upload could land in the middle of an earlier
// frame's GPU read, showing a mix of two frames for one refresh. Every
// CPU-decoded source went through it: FFV1 / DNxHR / ProRes RAW in software,
// EXR FP16 in dual, the QCBridge live feed.
//
// Each frame's command buffer gets a serial, and its completion handler
// records the newest finished one. A slot remembers the last serial that
// sampled it; an upload picks a slot whose last user has finished, and only
// if none has does it wait (bounded, logged). With three slots and at most
// three frames in flight, that wait should essentially never happen.
//
// QCV_UPLOAD_SLOTS sets the slot count (default 3). 1 reproduces the old
// behaviour for measurement: it overwrites regardless and counts each time
// the texture it overwrote was still in flight.
//
// Render thread only, apart from the completion handlers, which touch only
// the shared completion state.

#pragma once

#import <Metal/Metal.h>

#include <QImage>
#include <QString>
#include <QtLogging>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace qcv {

class MetalUploadRing
{
public:
    explicit MetalUploadRing(const char *name)
        : m_name(name), m_slots(static_cast<size_t>(slotsFromEnv())) {}

    // Once per frame, with the frame's command buffer, before upload() and
    // before current() is sampled. The buffer must be committed (its
    // completion is what frees slots; a wait for it times out otherwise).
    void beginFrame(id<MTLCommandBuffer> cb)
    {
        const uint64_t serial = ++m_serial;
        auto shared = m_shared;
        [cb addCompletedHandler:^(id<MTLCommandBuffer>) {
            std::lock_guard<std::mutex> lk(shared->m);
            shared->completed = std::max(shared->completed, serial);
            shared->cv.notify_all();
        }];
        // The current texture is sampled again this frame. Keep the previous
        // stamp: an upload before this buffer commits is safe for this frame,
        // so only earlier frames count as users.
        if (m_current >= 0) {
            Slot &cur = m_slots[static_cast<size_t>(m_current)];
            cur.prevUse = cur.lastUse;
            cur.lastUse = serial;
        }
    }

    // Uploads img into a slot no in-flight frame is sampling and makes it
    // current. Returns nil on failure.
    id<MTLTexture> upload(id<MTLDevice> device, const QImage &img, MTLPixelFormat fmt)
    {
        if (!device || img.isNull()) return nil;
        const int n = static_cast<int>(m_slots.size());
        int pick = -1;
        if (n == 1) {
            pick = 0;
            if (m_slots[0].prevUse > completed()) ++m_hazards;   // the old behaviour
        } else {
            pick = freeSlot();
            if (pick < 0) {
                ++m_waits;
                pick = waitForSlot();
            }
        }
        Slot &s = m_slots[static_cast<size_t>(pick)];
        const int w = img.width(), h = img.height();
        if (!s.tex || s.w != w || s.h != h || s.fmt != fmt) {
            MTLTextureDescriptor *desc =
                [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:fmt
                                                                    width:w
                                                                   height:h
                                                                mipmapped:NO];
            desc.storageMode = MTLStorageModeShared;
            desc.usage       = MTLTextureUsageShaderRead;
            s.tex = [device newTextureWithDescriptor:desc];
            s.w = w; s.h = h; s.fmt = fmt;
            if (!s.tex) return nil;
        }
        [s.tex replaceRegion:MTLRegionMake2D(0, 0, w, h)
                 mipmapLevel:0
                   withBytes:img.constBits()
                 bytesPerRow:img.bytesPerLine()];
        s.prevUse = s.lastUse;
        s.lastUse = m_serial;   // sampled by the frame being encoded
        m_current = pick;

        if (++m_uploads % 240 == 0) {
            qInfo("MetalUploadRing[%s]: %llu uploads, %llu overwrote a texture in flight, "
                  "%llu waited for one (%d slots)",
                  m_name, static_cast<unsigned long long>(m_uploads),
                  static_cast<unsigned long long>(m_hazards),
                  static_cast<unsigned long long>(m_waits), n);
        }
        return s.tex;
    }

    id<MTLTexture> current() const
    {
        return m_current >= 0 ? m_slots[static_cast<size_t>(m_current)].tex : nil;
    }

    // Drop the textures (media switch). Frames in flight keep their own
    // references to what they sample, so this never waits.
    void reset()
    {
        for (auto &s : m_slots) s = Slot{};
        m_current = -1;
    }

private:
    struct Slot {
        id<MTLTexture> tex = nil;
        int w = 0, h = 0;
        MTLPixelFormat fmt = MTLPixelFormatInvalid;
        uint64_t lastUse = 0;   // newest frame that samples it
        uint64_t prevUse = 0;   // the one before (for the 1-slot measurement)
    };
    struct Shared {
        std::mutex m;
        std::condition_variable cv;
        uint64_t completed = 0;
    };

    static int slotsFromEnv()
    {
        bool ok = false;
        const int v = qEnvironmentVariableIntValue("QCV_UPLOAD_SLOTS", &ok);
        return ok ? std::clamp(v, 1, 8) : 3;
    }

    uint64_t completed() const
    {
        std::lock_guard<std::mutex> lk(m_shared->m);
        return m_shared->completed;
    }

    // A slot, other than the current one, whose last user has finished;
    // the least recently used such slot. -1 if none.
    int freeSlot() const
    {
        const uint64_t done = completed();
        int best = -1;
        for (int i = 0; i < static_cast<int>(m_slots.size()); ++i) {
            if (i == m_current) continue;
            const Slot &s = m_slots[static_cast<size_t>(i)];
            if (s.lastUse > done) continue;
            if (best < 0 || s.lastUse < m_slots[static_cast<size_t>(best)].lastUse) best = i;
        }
        return best;
    }

    // Blocks until the least recently used non-current slot's last user
    // completes, bounded so an uncommitted buffer can't hang the render
    // thread (then that slot is overwritten anyway).
    int waitForSlot()
    {
        int lru = -1;
        for (int i = 0; i < static_cast<int>(m_slots.size()); ++i) {
            if (i == m_current) continue;
            if (lru < 0 || m_slots[static_cast<size_t>(i)].lastUse
                               < m_slots[static_cast<size_t>(lru)].lastUse) lru = i;
        }
        const uint64_t need = m_slots[static_cast<size_t>(lru)].lastUse;
        std::unique_lock<std::mutex> lk(m_shared->m);
        if (!m_shared->cv.wait_for(lk, std::chrono::milliseconds(100),
                                   [&] { return m_shared->completed >= need; })) {
            qWarning("MetalUploadRing[%s]: waited 100 ms for a slot; overwriting it",
                     m_name);
        }
        return lru;
    }

    const char *m_name;
    std::vector<Slot> m_slots;
    int m_current = -1;
    uint64_t m_serial = 0;
    uint64_t m_uploads = 0, m_hazards = 0, m_waits = 0;
    std::shared_ptr<Shared> m_shared = std::make_shared<Shared>();
};

} // namespace qcv
