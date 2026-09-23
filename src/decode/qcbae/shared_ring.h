// Vendored from QCBridgeAE (github.com/cbkow/QCBridgeAE, MIT) at 5057e96,
// src/common/surface/shared_ring.h. The wire contract between the QCBridgeAE Transmit device
// and QCView. Keep in sync by re-copying, never by editing here:
// SharedRing::open() rejects a ring whose version != kFrameDescVersion, so
// a mismatch fails loudly instead of misreading frames.

// QCBridgeAE — the shared frame ring.
//
// A fixed set of page-aligned pixel slots in shared memory, plus a sidecar
// FrameDesc per slot and one ICC region for the project's working profile.
// Producer (the AE plugin) writes; consumer (QCView, or the probe viewer)
// samples the newest slot in place. Latest-wins throughout: a consumer that
// falls behind skips frames rather than accruing arrears.
//
// Why shared memory rather than IOSurface, which PLAN.md names: IOSurface's
// cross-process handoff (IOSurfaceCreateMachPort / LookupFromMachPort) needs a
// Mach rendezvous that a plugin living inside AE cannot get without a
// LaunchAgent-registered service. A page-aligned mmap is rendezvous-by-path,
// works the same on both platforms, and is still zero-copy on Apple Silicon:
// MTLDevice.makeBuffer(bytesNoCopy:) over these pages gives the GPU the exact
// memory the CPU wrote. See lab/results/2026-09-20-a1-ring-spine/notes.md.
//
// Concurrency contract, no locks:
//   * Each slot carries a seqlock. Odd = a write is in progress; a reader that
//     sees an odd count, or a different count after reading, retries.
//   * The consumer publishes what it currently holds (reader_claim) and the
//     producer refuses to reuse that slot. With >= 3 slots the producer
//     always has somewhere to go: one held by the reader, one it is writing,
//     at least one free.
//   * Because the producer may step over the claimed slot, a frame's sequence
//     does NOT imply its slot. Both travel together in one atomic word (see
//     pack_latest). Deriving the slot from the sequence instead reads a stale
//     but self-consistent frame — a bug that survives any test comparing
//     pixels only against their own sidecar.
//   * The seqlock alone would be enough for a copying reader. The claim is
//     what makes it safe to sample the pixels in place, which is the whole
//     point of the exercise.

#pragma once

#include "decode/qcbae/frame_desc.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace qcbae {

inline constexpr uint32_t kMinSlots     = 3u;
inline constexpr uint32_t kDefaultSlots = 3u;

// macOS caps shm_open names at 31 bytes including the leading slash, which is
// tighter than anyone expects. Names are validated at create/open. Windows
// has no such cap, but the same rule applies there so a name that works on
// one platform works on the other: the ring is `Local\` + the name with its
// leading slash removed, a session-local page-file-backed file mapping (AE
// and QCView run in the same logon session, so `Global\` and its privilege
// are not needed).
inline constexpr size_t kMaxShmName = 31u;

// `latest` carries (sequence, slot) in one word so a consumer never has to
// infer where a frame landed. 8 bits of slot caps the ring at 255, which is
// ~250 more than anything sane, and leaves 56 bits of sequence.
// Fixed ring names, one per host (PLAN.md A6). A consumer lists these; a
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

inline constexpr uint32_t kSlotBits  = 8u;
inline constexpr uint64_t kSlotMask  = (1ull << kSlotBits) - 1ull;
inline constexpr uint32_t kMaxSlots  = static_cast<uint32_t>(kSlotMask);

inline constexpr uint64_t pack_latest(uint64_t seq, uint32_t slot) {
    return (seq << kSlotBits) | (slot & kSlotMask);
}
inline constexpr uint64_t latest_seq(uint64_t packed)  { return packed >> kSlotBits; }
inline constexpr uint32_t latest_slot(uint64_t packed) { return static_cast<uint32_t>(packed & kSlotMask); }

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

    bool valid() const { return base_ != nullptr; }
    const std::string& error() const { return error_; }
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
    void  close();
    SlotHeader* slot_at(uint32_t index) const;

    void*       base_       = nullptr;
    size_t      size_       = 0;
    RingHeader* header_     = nullptr;
#if defined(_WIN32)
    void*       mapping_    = nullptr;   // HANDLE from CreateFileMapping / OpenFileMapping
#else
    int         fd_         = -1;
#endif
    bool        owner_      = false;
    std::string name_;
    std::string error_;

    // Producer bookkeeping
    uint32_t    writing_slot_ = UINT32_MAX;
    uint64_t    write_seq_    = 0;

    // Consumer bookkeeping
    uint64_t    held_         = 0;
};

// Is the process that wrote `producer_pid` still running? A dead producer's
// ring stays readable (neither host unloads the Transmit device on quit), so
// the pid in the header is the consumer's only truth. POSIX: kill(pid, 0),
// with EPERM counting as alive. Windows: OpenProcess(SYNCHRONIZE) and a
// zero-timeout wait; a process we may not open counts as alive, the way
// EPERM does. Windows recycles pids far faster than macOS, so a consumer
// polling every few hundred ms can in principle mistake an unrelated new
// process for the producer; the ring then reads as alive-but-silent until
// that process exits. Accepted for v1 and noted in lab/results.
bool process_alive(uint32_t pid);

}  // namespace qcbae
