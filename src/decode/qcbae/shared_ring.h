// Vendored from QCBridgeAE (github.com/cbkow/QCBridgeAE, MIT — see its
// src/common/LICENSE) at 3d7de5b, src/common/surface/shared_ring.h. The wire contract between the
// QCBridgeAE Transmit device and QCView, and of QCView's own output bus; keep
// in sync by re-copying, never by editing here. A version mismatch is refused
// at open, never misread.

// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2026 cbkow. See src/common/LICENSE: the ring and sidecar
// files are MIT in addition to the repository's GPL so that consumers under
// any licence can carry the wire contract.
// QCBridgeAE — the shared frame ring.
//
// A fixed set of page-aligned pixel slots in shared memory, plus a sidecar
// FrameDesc per slot and one ICC region for the project's working profile.
// Producer (the AE plugin) writes; consumer (QCView, or the probe viewer)
// samples the newest slot in place. Latest-wins throughout: a consumer that
// falls behind skips frames rather than accruing arrears.
//
// Why shared memory rather than IOSurface, which the design notes name: IOSurface's
// cross-process handoff (IOSurfaceCreateMachPort / LookupFromMachPort) needs a
// Mach rendezvous that a plugin living inside AE cannot get without a
// LaunchAgent-registered service. A page-aligned mmap is rendezvous-by-path,
// works the same on both platforms, and is still zero-copy on Apple Silicon:
// MTLDevice.makeBuffer(bytesNoCopy:) over these pages gives the GPU the exact
// memory the CPU wrote. See lab/results/2026-09-20-a1-ring-spine/notes.md.
//
// The concurrency contract (seqlock per slot, reader claim, sequence and
// slot packed in one word) lives in ring_slots.h, shared with the output
// ring; the mapping itself in ring_mapping.h. The header layout here is the
// shipped FrameDesc v3 contract and has not moved.

#pragma once

#include "decode/qcbae/frame_desc.h"
#include "decode/qcbae/ring_mapping.h"
#include "decode/qcbae/ring_slots.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace qcbae {

// Fixed ring names, one per host (DESIGN-NOTES A6). A consumer lists these; a
// second running copy of the same host would collide, which v1 accepts.
inline constexpr const char* kRingNameAfterEffects = "/qcbae-ae";
inline constexpr const char* kRingNamePremiere     = "/qcbae-premiere";

// What the producer's host is doing, for a consumer to explain what it sees.
// It lives in the ring header, not in FrameDesc, because the interesting
// states are exactly the ones in which no frames arrive.
enum class HostState : uint32_t {
    Active       = 0,
    // The host switched the device's video off because the application lost
    // focus. With AE's default preferences this is every time the user looks
    // at QCView; unticking "Disable video output when in the background"
    // stops it (lab/results/2026-09-21-a4-transmit-probe/, section 12).
    PausedFocus  = 1,
    Paused       = 2,   // video switched off for another reason
    // This mapping is finished: the producer is replacing it (a larger frame
    // needed more room, or a module reset) or shutting down. A consumer
    // should close it and re-open the name. A name alone cannot tell a
    // consumer its mapping went stale — the old one stays readable forever.
    Retired      = 3,
};

struct RingHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t slot_count;
    uint32_t producer_pid;

    uint64_t slot_stride;      // bytes between slot starts
    uint64_t pixels_offset;    // offset from slot start to pixels (page-aligned)
    uint64_t pixels_capacity;  // usable pixel bytes per slot
    uint64_t icc_offset;       // from mapping start
    uint64_t icc_capacity;
    // Where slot 0 begins, from mapping start. Page-aligned, and NOT
    // sizeof(RingHeader): the header is padded up to a page so that every
    // slot's pixel region lands on a page boundary in absolute terms. Metal
    // rejects a linear texture over a 16-byte-misaligned address, and that is
    // what deriving this from sizeof(RingHeader) produced.
    uint64_t slots_offset;
    uint64_t total_size;

    // pack_latest(sequence, slot) of the newest published frame. 0 = nothing
    // published yet. Sequences are monotonic; slots are not implied by them.
    std::atomic<uint64_t> latest;

    // The packed value the consumer is holding, or 0. Producer will not reuse
    // that slot.
    std::atomic<uint64_t> reader_claim;

    // ICC region seqlock + length. The profile changes on project settings,
    // not per frame, so FrameDesc carries only a generation counter.
    std::atomic<uint64_t> icc_seq;
    std::atomic<uint64_t> icc_generation;
    std::atomic<uint32_t> icc_len;
    std::atomic<uint32_t> host_state;   // HostState; was padding before FrameDesc v3
};

struct SlotHeader {
    std::atomic<uint64_t> seq;  // seqlock; odd = write in progress
    FrameDesc desc;
};

class SharedRing {
public:
    SharedRing() = default;
    ~SharedRing();
    SharedRing(SharedRing&&) noexcept;
    SharedRing& operator=(SharedRing&&) noexcept;
    SharedRing(const SharedRing&) = delete;
    SharedRing& operator=(const SharedRing&) = delete;

    // Producer side. `name` must start with '/' and be <= kMaxShmName bytes.
    // An existing mapping of the same name is replaced.
    bool create(const std::string& name,
                uint64_t pixels_capacity,
                uint32_t slot_count = kDefaultSlots,
                uint64_t icc_capacity = 64u * 1024u);

    // Consumer side.
    bool open(const std::string& name);

    bool valid() const { return map_.valid(); }
    const std::string& error() const { return error_.empty() ? map_.error() : error_; }
    const RingHeader* header() const { return header_; }

    // --- Producer ----------------------------------------------------------
    // Returns the pixel buffer to fill, or nullptr if the ring is invalid or
    // `bytes` exceeds a slot. Must be paired with commit() or abandon().
    void* begin_write(uint64_t bytes);
    void  commit(const FrameDesc& desc);
    void  abandon();

    // Replace the ICC blob and bump its generation. Cheap enough to call on
    // every project-settings change; never on a per-frame path.
    bool  set_icc_profile(const void* data, uint32_t len);

    void  set_host_state(HostState s);

    // --- Consumer ----------------------------------------------------------
    // Claims the newest frame if it differs from `last_seen`. On success the
    // pixels stay valid until release(). Updates `last_seen` to the acquired
    // sequence. Returns false when there is nothing newer.
    bool acquire_latest(uint64_t* last_seen, FrameDesc* out_desc, const void** out_pixels);
    void release();

    // Which slot the currently-held frame lives in. A consumer that builds one
    // GPU texture per slot needs this to index its cache; digging it out of
    // reader_claim works but makes an internal a public contract.
    uint32_t held_slot() const { return latest_slot(held_); }

    // Copies the ICC blob out if `generation` differs from the caller's.
    // Returns the new generation, or 0 when unchanged or absent.
    uint64_t read_icc_profile(uint64_t known_generation, std::string* out_blob) const;

    HostState host_state() const;

private:
    void close();

    SharedMapping map_;
    RingHeader*   header_ = nullptr;
    std::string   error_;
    ringslot::Writer writer_;
    uint64_t      held_ = 0;   // consumer: the packed claim it holds, or 0
};

}  // namespace qcbae
