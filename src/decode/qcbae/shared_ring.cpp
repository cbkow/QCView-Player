// Vendored from QCBridgeAE (github.com/cbkow/QCBridgeAE, MIT — see its
// src/common/LICENSE) at 5849977, src/common/surface/shared_ring.cpp. The wire contract between the
// QCBridgeAE Transmit device and QCView, and of QCView's own output bus; keep
// in sync by re-copying, never by editing here. A version mismatch is refused
// at open, never misread.

// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2026 cbkow. See src/common/LICENSE: the ring and sidecar
// files are MIT in addition to the repository's GPL so that consumers under
// any licence can carry the wire contract.
#include "decode/qcbae/shared_ring.h"

#include <cstring>
#include <utility>

namespace qcbae {
namespace {

uint64_t ring_total(const void* header) {
    return static_cast<const RingHeader*>(header)->total_size;
}

void ring_retire(void* header) {
    static_cast<RingHeader*>(header)->host_state.store(
        static_cast<uint32_t>(HostState::Retired), std::memory_order_release);
}

}  // namespace

SharedRing::~SharedRing() { close(); }

SharedRing::SharedRing(SharedRing&& o) noexcept { *this = std::move(o); }

// Closes what this ring held, takes the other's mapping, and leaves the
// other empty — a moved-from ring must not keep a header pointer into a
// mapping it no longer owns.
SharedRing& SharedRing::operator=(SharedRing&& o) noexcept {
    if (this != &o) {
        close();
        map_    = std::move(o.map_);
        header_ = o.header_;  o.header_ = nullptr;
        error_  = std::move(o.error_);
        writer_ = o.writer_;  o.writer_ = {};
        held_   = o.held_;    o.held_ = 0;
    }
    return *this;
}

void SharedRing::close() {
    map_.close();
    header_ = nullptr;
    writer_ = {};
    held_   = 0;
}

bool SharedRing::create(const std::string& name, uint64_t pixels_capacity,
                        uint32_t slot_count, uint64_t icc_capacity) {
    close();
    error_.clear();
    if (slot_count < kMinSlots || slot_count > kMaxSlots) {
        error_ = "slot_count must be between 3 and 255 (see pack_latest)";
        return false;
    }

    const uint64_t ps         = SharedMapping::page_size();
    icc_capacity              = SharedMapping::round_up(icc_capacity, ps);
    const uint64_t pix_off    = SharedMapping::round_up(sizeof(SlotHeader), ps);
    const uint64_t pix_cap    = SharedMapping::round_up(pixels_capacity, ps);
    const uint64_t stride     = pix_off + pix_cap;
    const uint64_t head_bytes = SharedMapping::round_up(sizeof(RingHeader), ps);
    const uint64_t total      = head_bytes + icc_capacity + stride * slot_count;

    if (!map_.create(name, total, head_bytes, kRingMagic, ring_retire)) return false;

    header_ = static_cast<RingHeader*>(map_.base());
    header_->magic           = kRingMagic;
    header_->version         = kFrameDescVersion;
    header_->slot_count      = slot_count;
    header_->producer_pid    = SharedMapping::self_pid();
    header_->slot_stride     = stride;
    header_->pixels_offset   = pix_off;
    header_->pixels_capacity = pix_cap;
    header_->icc_offset      = head_bytes;
    header_->icc_capacity    = icc_capacity;
    header_->slots_offset    = head_bytes + icc_capacity;
    header_->total_size      = total;
    // header_ atomics are zeroed by the mapping's memset, which is the
    // intended initial state (latest = 0 means "nothing published yet").
    return true;
}

bool SharedRing::open(const std::string& name) {
    close();
    error_.clear();
    if (!map_.open(name, sizeof(RingHeader), kRingMagic, kFrameDescVersion, ring_total, "ring"))
        return false;
    header_ = static_cast<RingHeader*>(map_.base());
    return true;
}

void* SharedRing::begin_write(uint64_t bytes) {
    if (!valid()) return nullptr;
    void* p = slots::begin_write<RingHeader, SlotHeader>(map_.base(), header_, &writer_, bytes);
    if (p == nullptr && !writer_.open() && bytes > header_->pixels_capacity)
        error_ = "frame larger than slot capacity";
    return p;
}

void SharedRing::commit(const FrameDesc& desc) {
    if (!valid()) return;
    slots::commit<RingHeader, SlotHeader, FrameDesc>(map_.base(), header_, &writer_, desc);
}

void SharedRing::abandon() {
    if (!valid()) return;
    slots::abandon<RingHeader, SlotHeader>(map_.base(), header_, &writer_);
}

bool SharedRing::acquire_latest(uint64_t* last_seen, FrameDesc* out_desc, const void** out_pixels) {
    if (!valid()) return false;
    return slots::acquire_latest<RingHeader, SlotHeader, FrameDesc>(
        map_.base(), header_, &held_, last_seen, out_desc, out_pixels);
}

void SharedRing::release() {
    if (!valid()) return;
    slots::release(header_, &held_);
}

bool SharedRing::set_icc_profile(const void* data, uint32_t len) {
    if (!valid()) return false;
    if (len > header_->icc_capacity) { error_ = "ICC profile exceeds region"; return false; }
    auto* dst = static_cast<uint8_t*>(map_.base()) + header_->icc_offset;
    const uint64_t s = header_->icc_seq.load(std::memory_order_relaxed);
    header_->icc_seq.store(s + 1u, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    if (len > 0u && data != nullptr) std::memcpy(dst, data, len);
    header_->icc_len.store(len, std::memory_order_release);
    header_->icc_generation.fetch_add(1u, std::memory_order_release);
    header_->icc_seq.store(s + 2u, std::memory_order_release);
    return true;
}

uint64_t SharedRing::read_icc_profile(uint64_t known_generation, std::string* out_blob) const {
    if (!valid() || out_blob == nullptr) return 0u;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint64_t s1 = header_->icc_seq.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0u) continue;
        const uint64_t gen = header_->icc_generation.load(std::memory_order_acquire);
        if (gen == 0u || gen == known_generation) return 0u;
        const uint32_t len = header_->icc_len.load(std::memory_order_acquire);
        if (len > header_->icc_capacity) return 0u;
        const auto* src = static_cast<const uint8_t*>(map_.base()) + header_->icc_offset;
        std::string blob(reinterpret_cast<const char*>(src), len);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (header_->icc_seq.load(std::memory_order_acquire) != s1) continue;
        *out_blob = std::move(blob);
        return gen;
    }
    return 0u;
}

void SharedRing::set_host_state(HostState st) {
    if (header_ != nullptr) header_->host_state.store(static_cast<uint32_t>(st), std::memory_order_release);
}

HostState SharedRing::host_state() const {
    return header_ != nullptr ? static_cast<HostState>(header_->host_state.load(std::memory_order_acquire))
                              : HostState::Retired;
}

}  // namespace qcbae
