// Vendored from QCBridgeAE (github.com/cbkow/QCBridgeAE, MIT — see its
// src/common/LICENSE) at 3d7de5b, src/common/surface/ring_slots.h. The wire contract between the
// QCBridgeAE Transmit device and QCView, and of QCView's own output bus; keep
// in sync by re-copying, never by editing here. A version mismatch is refused
// at open, never misread.

// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2026 cbkow. See src/common/LICENSE: the ring and sidecar
// files are MIT in addition to the repository's GPL so that consumers under
// any licence can carry the wire contract.
// QCBridgeAE — the claim-and-seqlock slot protocol, shared by the frame ring
// and the output ring.
//
// Both rings publish whole frames into page-aligned slots that a consumer
// samples in place (zero-copy into a GPU texture), so both need the same
// three guarantees, written once here:
//   * Each slot carries a seqlock. Odd = a write is in progress; a reader
//     that sees an odd count, or a different count after reading, retries.
//   * The consumer publishes what it currently holds (reader_claim) and the
//     producer refuses to reuse that slot. With >= 3 slots the producer
//     always has somewhere to go: one held by the reader, one it is writing,
//     at least one free.
//   * Because the producer may step over the claimed slot, a frame's
//     sequence does NOT imply its slot. Both travel together in one atomic
//     word (pack_latest). Deriving the slot from the sequence instead reads
//     a stale but self-consistent frame — a bug that survives any test that
//     compares pixels only against their own sidecar.
//
// A Header must carry: slot_count, slot_stride, slots_offset, pixels_offset,
// pixels_capacity, and the atomics latest and reader_claim. A Slot must
// carry the atomic `seq` and a `desc` of the ring's descriptor type. The
// functions are templates over those two so each ring keeps its own wire
// layout; the frame ring's header is a shipped contract and has not moved.
//
// The namespace is `ringslot`, not `slots`: QCView vendors these files and
// Qt defines `slots` (and `signals`, `emit`) as macros, so that name would
// not survive a Qt translation unit.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace qcbae {

inline constexpr uint32_t kMinSlots     = 3u;
inline constexpr uint32_t kDefaultSlots = 3u;

// `latest` carries (sequence, slot) in one word so a consumer never has to
// infer where a frame landed. 8 bits of slot caps the ring at 255, which is
// ~250 more than anything sane, and leaves 56 bits of sequence.
inline constexpr uint32_t kSlotBits  = 8u;
inline constexpr uint64_t kSlotMask  = (1ull << kSlotBits) - 1ull;
inline constexpr uint32_t kMaxSlots  = static_cast<uint32_t>(kSlotMask);

inline constexpr uint64_t pack_latest(uint64_t seq, uint32_t slot) {
    return (seq << kSlotBits) | (slot & kSlotMask);
}
inline constexpr uint64_t latest_seq(uint64_t packed)  { return packed >> kSlotBits; }
inline constexpr uint32_t latest_slot(uint64_t packed) { return static_cast<uint32_t>(packed & kSlotMask); }

namespace ringslot {

template <class Header, class Slot>
inline Slot* slot_at(void* base, const Header* h, uint32_t index) {
    auto* bytes = static_cast<uint8_t*>(base);
    return reinterpret_cast<Slot*>(bytes + h->slots_offset + index * h->slot_stride);
}

template <class Header>
inline uint8_t* pixels_at(void* base, const Header* h, uint32_t index) {
    return static_cast<uint8_t*>(base) + h->slots_offset + index * h->slot_stride + h->pixels_offset;
}

// Producer bookkeeping the ring object owns between begin_write and commit.
struct Writer {
    uint32_t slot = UINT32_MAX;
    uint64_t seq  = 0;
    bool open() const { return slot != UINT32_MAX; }
};

// Picks the next free slot, marks it odd, and returns its pixel buffer; or
// nullptr when a write is already open or `bytes` exceeds a slot.
template <class Header, class Slot>
inline void* begin_write(void* base, Header* h, Writer* w, uint64_t bytes) {
    if (base == nullptr || w->open() || bytes > h->pixels_capacity) return nullptr;
    const uint64_t packed = h->latest.load(std::memory_order_relaxed);
    const uint64_t next   = latest_seq(packed) + 1u;
    const uint32_t count  = h->slot_count;
    const uint64_t claim  = h->reader_claim.load(std::memory_order_acquire);

    // Advance from the slot we last wrote, stepping over whatever the
    // consumer is holding. With >= 3 slots the next one is guaranteed free.
    uint32_t index = (packed == 0u) ? 0u : (latest_slot(packed) + 1u) % count;
    if (claim != 0u && index == latest_slot(claim)) index = (index + 1u) % count;

    Slot* slot = slot_at<Header, Slot>(base, h, index);
    const uint64_t s = slot->seq.load(std::memory_order_relaxed);
    slot->seq.store(s + 1u, std::memory_order_release);   // odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);

    w->slot = index;
    w->seq  = next;
    return pixels_at(base, h, index);
}

template <class Header, class Slot, class Desc>
inline void commit(void* base, Header* h, Writer* w, const Desc& desc) {
    if (base == nullptr || !w->open()) return;
    Slot* slot = slot_at<Header, Slot>(base, h, w->slot);
    slot->desc = desc;
    const uint64_t s = slot->seq.load(std::memory_order_relaxed);
    slot->seq.store(s + 1u, std::memory_order_release);   // even: stable again
    h->latest.store(pack_latest(w->seq, w->slot), std::memory_order_release);
    w->slot = UINT32_MAX;
}

template <class Header, class Slot>
inline void abandon(void* base, Header* h, Writer* w) {
    if (base == nullptr || !w->open()) return;
    Slot* slot = slot_at<Header, Slot>(base, h, w->slot);
    const uint64_t s = slot->seq.load(std::memory_order_relaxed);
    slot->seq.store(s + 1u, std::memory_order_release);   // back to even; latest untouched
    w->slot = UINT32_MAX;
}

// Claims the newest frame if it differs from `*last_seen`; `*held` is the
// consumer's current claim (0 = none) and is updated on success. The pixels
// stay valid until release(). Returns false when there is nothing newer, or
// the newest slot was mid-write (call again).
template <class Header, class Slot, class Desc>
inline bool acquire_latest(void* base, Header* h, uint64_t* held, uint64_t* last_seen,
                           Desc* out_desc, const void** out_pixels) {
    if (base == nullptr || last_seen == nullptr) return false;
    const uint64_t packed = h->latest.load(std::memory_order_acquire);
    if (packed == 0u) return false;
    const uint64_t seq   = latest_seq(packed);
    const uint32_t index = latest_slot(packed);
    if (seq == *last_seen) return false;
    if (index >= h->slot_count) return false;   // corrupt or foreign ring
    Slot* slot = slot_at<Header, Slot>(base, h, index);

    // Claim before reading so the producer stops reusing this slot, then
    // verify with the seqlock that nothing was mid-write when we claimed.
    h->reader_claim.store(packed, std::memory_order_release);

    const uint64_t s1 = slot->seq.load(std::memory_order_acquire);
    if ((s1 & 1u) != 0u) { h->reader_claim.store(*held, std::memory_order_release); return false; }

    Desc desc = slot->desc;
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint64_t s2 = slot->seq.load(std::memory_order_acquire);
    if (s1 != s2) { h->reader_claim.store(*held, std::memory_order_release); return false; }

    *held = packed;
    *last_seen = seq;
    if (out_desc != nullptr) *out_desc = desc;
    if (out_pixels != nullptr) *out_pixels = pixels_at(base, h, index);
    return true;
}

template <class Header>
inline void release(Header* h, uint64_t* held) {
    if (h == nullptr) return;
    *held = 0;
    h->reader_claim.store(0u, std::memory_order_release);
}

// A seqlocked record in the header (the ICC blob, a request, a reply): the
// writer stores odd, fences, writes, stores even and bumps a generation.
// Readers copy under the lock and compare generations.
template <class T>
inline void write_record(std::atomic<uint64_t>* lock, std::atomic<uint64_t>* generation,
                         T* record, const T& value) {
    const uint64_t s = lock->load(std::memory_order_relaxed);
    lock->store(s + 1u, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    *record = value;
    generation->fetch_add(1u, std::memory_order_release);
    lock->store(s + 2u, std::memory_order_release);
}

// Copies the record out when its generation differs from `*known`; returns
// true and updates `*known` on a change. Generation 0 means never written.
template <class T>
inline bool read_record(const std::atomic<uint64_t>* lock, const std::atomic<uint64_t>* generation,
                        const T* record, uint64_t* known, T* out) {
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint64_t s1 = lock->load(std::memory_order_acquire);
        if ((s1 & 1u) != 0u) continue;
        const uint64_t gen = generation->load(std::memory_order_acquire);
        if (gen == 0u || gen == *known) return false;
        T copy = *record;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (lock->load(std::memory_order_acquire) != s1) continue;
        *out = copy;
        *known = gen;
        return true;
    }
    return false;
}

}  // namespace ringslot
}  // namespace qcbae
