// OutputTarget — the render thread's view of one enabled output
// (QCBridgeN/PLAN.md P2). The bus (window/output_bus.*) owns the rings and
// the plugin process; this is the slice the renderer touches per frame:
// is a sink present and accepted, what raster/format/content it asked for,
// and the slot to render into.
//
// Threads: the GUI thread sets the spec and the active flag; the render
// thread reads them once per frame and does begin/commit on the ring. The
// ring pointer itself is only ever used by the render thread between
// IPlayerRenderer::setOutputTargets(...) and the next call that removes
// the target — that setter is the handshake (it takes the renderer's
// source mutex, like setDualController), so by the time the bus destroys
// a ring no frame can be mid-write into it.
//
// No Qt here: qcv_render's Metal and D3D11 implementations include this.

#pragma once

#include "decode/qcbae/output_desc.h"
#include "decode/qcbae/output_ring.h"

#include <atomic>
#include <cstdint>
#include <mutex>

namespace qcv::output {

struct OutputSpec {
    uint32_t width   = 0;
    uint32_t height  = 0;
    qcbae::OutputPixelFormat format   = qcbae::OutputPixelFormat::BGRA8;
    qcbae::OutputTransfer    transfer = qcbae::OutputTransfer::SdrRec709;
    qcbae::OutputContent     content  = qcbae::OutputContent::Composite;
    qcbae::OutputFit         fit      = qcbae::OutputFit::Letterbox;
    uint32_t layers = 0;   // qcbae::OutputLayers the producer will draw

    bool valid() const { return width > 0 && height > 0; }
    uint32_t bytesPerRow() const { return qcbae::output_aligned_bytes_per_row(width, format); }
    uint64_t frameBytes() const  { return qcbae::output_frame_bytes(width, height, format); }
};

class OutputTarget {
public:
    explicit OutputTarget(qcbae::OutputRing *ring) : m_ring(ring) {}

    // ---- GUI thread ----
    void setSpec(const OutputSpec &spec)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_spec = spec;
        ++m_generation;
    }
    // A sink is attached, alive and its request accepted: render.
    void setActive(bool on) { m_active.store(on, std::memory_order_release); }
    // Facts the renderer cannot know on its own; stamped into every frame.
    void setSourceFps(uint32_t num, uint32_t den)
    {
        m_fpsNum.store(num, std::memory_order_relaxed);
        m_fpsDen.store(den, std::memory_order_relaxed);
    }

    // ---- render thread ----
    bool active() const { return m_active.load(std::memory_order_acquire); }
    OutputSpec spec(uint64_t *generation) const
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (generation) *generation = m_generation;
        return m_spec;
    }
    uint32_t fpsNum() const { return m_fpsNum.load(std::memory_order_relaxed); }
    uint32_t fpsDen() const { return m_fpsDen.load(std::memory_order_relaxed); }

    // The ring slot to render into (page-aligned; capacity() bytes), or
    // nullptr when a write is still open or the frame does not fit.
    void *beginFrame(uint64_t bytes) { return m_ring ? m_ring->begin_write(bytes) : nullptr; }
    void  commitFrame(const qcbae::OutputFrameDesc &d) { if (m_ring) m_ring->commit(d); }
    void  abandonFrame() { if (m_ring) m_ring->abandon(); }
    uint64_t capacity() const { return m_ring && m_ring->header() ? m_ring->header()->pixels_capacity : 0; }

    qcbae::OutputRing *ring() const { return m_ring; }

private:
    qcbae::OutputRing   *m_ring;
    mutable std::mutex   m_mutex;
    OutputSpec           m_spec;
    uint64_t             m_generation = 0;
    std::atomic<bool>    m_active{false};
    std::atomic<uint32_t> m_fpsNum{0};
    std::atomic<uint32_t> m_fpsDen{0};
};

} // namespace qcv::output
