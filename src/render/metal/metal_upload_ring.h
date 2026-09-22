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
// A slot remembers the command buffer that last sampled it, and Metal
// reports that buffer's own status: an upload picks a slot whose last user
// has finished, and only if none has does it wait on that buffer. With three
// slots and at most three frames in flight, that wait should essentially
// never happen. (No completion handlers: a block capturing shared state is
// heap-copied per frame, which ThreadSanitizer flagged against an earlier
// handler still reading the reused block.)
//
// QCV_UPLOAD_SLOTS sets the slot count (default 3). 1 reproduces the old
// behaviour for measurement: it overwrites regardless and counts each time
// the texture it overwrote was still in flight.
//
// Render thread only.

#pragma once

#import <Metal/Metal.h>

#include <QImage>
#include <QString>
#include <QtLogging>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace qcv {

class MetalUploadRing
{
public:
    explicit MetalUploadRing(const char *name)
        : m_name(name), m_slots(static_cast<size_t>(slotsFromEnv())) {}

    // Once per frame, with the frame's command buffer, before upload() and
    // before current() is sampled. The buffer must be committed: its
    // completion is what frees slots again.
    void beginFrame(id<MTLCommandBuffer> cb)
    {
        m_frameCb = cb;
        // The current texture is sampled again this frame. Keep the previous
        // user: an upload before this buffer commits is safe for this frame,
        // so only earlier frames count.
        if (m_current >= 0) {
            Slot &cur = m_slots[static_cast<size_t>(m_current)];
            cur.prevUser = cur.lastUser;
            cur.lastUser = cb;
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
            if (inFlight(m_slots[0].prevUser)) ++m_hazards;   // the old behaviour
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
        s.prevUser = s.lastUser;
        s.lastUser = m_frameCb;   // sampled by the frame being encoded
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
        m_frameCb = nil;
    }

private:
    struct Slot {
        id<MTLTexture> tex = nil;
        int w = 0, h = 0;
        MTLPixelFormat fmt = MTLPixelFormatInvalid;
        id<MTLCommandBuffer> lastUser = nil;   // newest frame that samples it
        id<MTLCommandBuffer> prevUser = nil;   // the one before (1-slot measurement)
    };

    // A buffer that has not finished on the GPU. nil (never used) and the
    // two terminal states are free; anything else is still in flight.
    static bool inFlight(id<MTLCommandBuffer> cb)
    {
        if (!cb) return false;
        const MTLCommandBufferStatus st = cb.status;
        return st != MTLCommandBufferStatusCompleted
            && st != MTLCommandBufferStatusError;
    }

    static int slotsFromEnv()
    {
        bool ok = false;
        const int v = qEnvironmentVariableIntValue("QCV_UPLOAD_SLOTS", &ok);
        return ok ? std::clamp(v, 1, 8) : 3;
    }

    // A slot, other than the current one, whose last user has finished.
    // Oldest first, so a slot gets the longest possible rest.
    int freeSlot() const
    {
        for (int i = 1; i <= static_cast<int>(m_slots.size()); ++i) {
            const int idx = (m_current + i) % static_cast<int>(m_slots.size());
            if (idx == m_current) continue;
            if (!inFlight(m_slots[static_cast<size_t>(idx)].lastUser)) return idx;
        }
        return -1;
    }

    // Every other slot is still in flight: wait for the one after the
    // current, which is the oldest.
    int waitForSlot()
    {
        const int idx = (m_current + 1) % static_cast<int>(m_slots.size());
        id<MTLCommandBuffer> user = m_slots[static_cast<size_t>(idx)].lastUser;
        if (user) [user waitUntilCompleted];
        return idx;
    }

    const char *m_name;
    std::vector<Slot> m_slots;
    int m_current = -1;
    id<MTLCommandBuffer> m_frameCb = nil;
    uint64_t m_uploads = 0, m_hazards = 0, m_waits = 0;
};

} // namespace qcv
