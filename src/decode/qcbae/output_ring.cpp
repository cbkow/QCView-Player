// Vendored from QCBridgeAE (github.com/cbkow/QCBridgeAE, MIT — see its
// src/common/LICENSE) at 3d7de5b, src/common/surface/output_ring.cpp. The wire contract between the
// QCBridgeAE Transmit device and QCView, and of QCView's own output bus; keep
// in sync by re-copying, never by editing here. A version mismatch is refused
// at open, never misread.

// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2026 cbkow. See src/common/LICENSE: the ring and sidecar
// files are MIT in addition to the repository's GPL so that consumers under
// any licence can carry the wire contract.
#include "decode/qcbae/output_ring.h"

#include <cstring>
#include <utility>

#if defined(__APPLE__)
#  include <mach/mach_time.h>
#elif defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <time.h>
#endif

namespace qcbae {
namespace {

uint64_t output_total(const void* header) {
    return static_cast<const OutputRingHeader*>(header)->total_size;
}

void output_retire(void* header) {
    static_cast<OutputRingHeader*>(header)->state.store(
        static_cast<uint32_t>(OutputState::Retired), std::memory_order_release);
}

bool id_ok(const std::string& id) {
    if (id.empty() || id.size() > kOutputIdMaxBytes) return false;
    for (char c : id) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
    }
    return true;
}

}  // namespace

std::string OutputRing::name_for(const std::string& plugin_id) {
    return id_ok(plugin_id) ? std::string(kOutputRingPrefix) + plugin_id : std::string();
}

std::string OutputRing::audio_name_for(const std::string& plugin_id) {
    const std::string n = name_for(plugin_id);
    return n.empty() ? n : n + "-audio";
}

int64_t OutputRing::now_ns() {
#if defined(__APPLE__)
    static const mach_timebase_info_data_t tb = [] {
        mach_timebase_info_data_t t {};
        ::mach_timebase_info(&t);
        return t;
    }();
    const uint64_t t = ::mach_absolute_time();
    return static_cast<int64_t>(t * tb.numer / tb.denom);
#elif defined(_WIN32)
    static const LONGLONG freq = [] {
        LARGE_INTEGER f {};
        ::QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c {};
    ::QueryPerformanceCounter(&c);
    // Split to keep the multiply in range for an uptime measured in weeks.
    const LONGLONG whole = c.QuadPart / freq, rem = c.QuadPart % freq;
    return static_cast<int64_t>(whole * 1000000000LL + rem * 1000000000LL / freq);
#else
    timespec ts {};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
#endif
}

OutputRing::~OutputRing() { close(); }

OutputRing::OutputRing(OutputRing&& o) noexcept { *this = std::move(o); }

// Closes what this ring held — for an attached consumer that clears its pid
// in the mapping — then takes the other's state and leaves it empty.
OutputRing& OutputRing::operator=(OutputRing&& o) noexcept {
    if (this != &o) {
        close();
        map_      = std::move(o.map_);
        header_   = o.header_;   o.header_ = nullptr;
        error_    = std::move(o.error_);
        writer_   = o.writer_;   o.writer_ = {};
        held_     = o.held_;     o.held_ = 0;
        attached_ = o.attached_; o.attached_ = false;
    }
    return *this;
}

void OutputRing::close() {
    // A sink leaving cleanly takes its pid with it, so the producer stops
    // rendering now rather than after the heartbeat ages out.
    if (attached_ && header_ != nullptr && !map_.owner()) {
        const uint32_t self = SharedMapping::self_pid();
        uint32_t expect = self;
        header_->consumer_pid.compare_exchange_strong(expect, 0u, std::memory_order_acq_rel);
    }
    attached_ = false;
    map_.close();
    header_ = nullptr;
    writer_ = {};
    held_   = 0;
}

// --- Producer -----------------------------------------------------------------

bool OutputRing::create(const std::string& name, uint64_t pixels_capacity,
                        uint32_t slot_count, uint32_t header_flags) {
    close();
    error_.clear();
    if (slot_count < kMinSlots || slot_count > kMaxSlots) {
        error_ = "slot_count must be between 3 and 255 (see pack_latest)";
        return false;
    }
    const uint64_t ps         = SharedMapping::page_size();
    const uint64_t pix_off    = SharedMapping::round_up(sizeof(OutputSlotHeader), ps);
    const uint64_t pix_cap    = SharedMapping::round_up(pixels_capacity, ps);
    const uint64_t stride     = pix_off + pix_cap;
    const uint64_t head_bytes = SharedMapping::round_up(sizeof(OutputRingHeader), ps);
    const uint64_t total      = head_bytes + stride * slot_count;

    if (!map_.create(name, total, head_bytes, kOutputRingMagic, output_retire)) return false;

    header_ = static_cast<OutputRingHeader*>(map_.base());
    header_->magic             = kOutputRingMagic;
    header_->version           = kOutputDescVersion;
    header_->slot_count        = slot_count;
    header_->producer_pid      = SharedMapping::self_pid();
    header_->slot_stride       = stride;
    header_->pixels_offset     = pix_off;
    header_->pixels_capacity   = pix_cap;
    header_->slots_offset      = head_bytes;
    header_->total_size        = total;
    header_->producer_protocol = kOutputDescVersion;
    header_->header_flags      = header_flags;
    // Atomics zeroed by the mapping: latest 0, no claim, state Active, no
    // consumer, no request, no reply.
    return true;
}

bool OutputRing::read_request(OutputRequest* out, uint64_t* known) const {
    if (!valid() || out == nullptr || known == nullptr) return false;
    return ringslot::read_record(&header_->request_lock, &header_->request_generation,
                              &header_->request, known, out);
}

void OutputRing::write_reply(const OutputReply& reply, uint64_t request_generation) {
    if (!valid()) return;
    const uint64_t s = header_->reply_lock.load(std::memory_order_relaxed);
    header_->reply_lock.store(s + 1u, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    header_->reply_for = request_generation;
    header_->reply     = reply;
    header_->reply.message[kOutputMessageBytes - 1u] = 0;
    header_->reply_generation.fetch_add(1u, std::memory_order_release);
    header_->reply_lock.store(s + 2u, std::memory_order_release);
}

bool OutputRing::consumer_present(int64_t now, int64_t max_age_ns) const {
    if (!valid()) return false;
    const uint32_t pid = header_->consumer_pid.load(std::memory_order_acquire);
    if (pid == 0u) return false;
    const int64_t beat = header_->heartbeat_ns.load(std::memory_order_acquire);
    if (now - beat > max_age_ns) return false;
    return process_alive(pid);
}

uint32_t OutputRing::consumer_pid() const {
    return valid() ? header_->consumer_pid.load(std::memory_order_acquire) : 0u;
}

uint64_t OutputRing::consumer_last_seen() const {
    return valid() ? header_->last_seen_seq.load(std::memory_order_acquire) : 0u;
}

bool OutputRing::read_status(std::string* out, uint64_t* known) const {
    if (!valid() || out == nullptr || known == nullptr) return false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint64_t s1 = header_->status_lock.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0u) continue;
        const uint64_t gen = header_->status_generation.load(std::memory_order_acquire);
        if (gen == 0u || gen == *known) return false;
        char buf[kOutputStatusBytes];
        std::memcpy(buf, header_->status, sizeof buf);
        buf[kOutputStatusBytes - 1u] = 0;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (header_->status_lock.load(std::memory_order_acquire) != s1) continue;
        out->assign(buf);
        *known = gen;
        return true;
    }
    return false;
}

void* OutputRing::begin_write(uint64_t bytes) {
    if (!valid()) return nullptr;
    void* p = ringslot::begin_write<OutputRingHeader, OutputSlotHeader>(map_.base(), header_, &writer_, bytes);
    if (p == nullptr && !writer_.open() && bytes > header_->pixels_capacity)
        error_ = "frame larger than slot capacity";
    return p;
}

void OutputRing::commit(const OutputFrameDesc& desc) {
    if (!valid()) return;
    ringslot::commit<OutputRingHeader, OutputSlotHeader, OutputFrameDesc>(map_.base(), header_, &writer_, desc);
}

void OutputRing::abandon() {
    if (!valid()) return;
    ringslot::abandon<OutputRingHeader, OutputSlotHeader>(map_.base(), header_, &writer_);
}

void OutputRing::set_state(OutputState s) {
    if (header_ != nullptr) header_->state.store(static_cast<uint32_t>(s), std::memory_order_release);
}

uint64_t OutputRing::newest_seq() const {
    return valid() ? qcbae::latest_seq(header_->latest.load(std::memory_order_acquire)) : 0u;
}

// --- Consumer -----------------------------------------------------------------

bool OutputRing::open(const std::string& name) {
    close();
    error_.clear();
    if (!map_.open(name, sizeof(OutputRingHeader), kOutputRingMagic, kOutputDescVersion,
                   output_total, "output ring"))
        return false;
    header_ = static_cast<OutputRingHeader*>(map_.base());
    const uint64_t need = header_->slots_offset
                        + static_cast<uint64_t>(header_->slot_count) * header_->slot_stride;
    if (header_->slot_count < kMinSlots || header_->slot_count > kMaxSlots
        || header_->pixels_offset + header_->pixels_capacity > header_->slot_stride
        || need > map_.size()) {
        error_ = "output ring geometry does not fit its mapping";
        close(); return false;
    }
    return true;
}

void OutputRing::attach(int64_t now) {
    if (!valid()) return;
    header_->consumer_protocol.store(kOutputDescVersion, std::memory_order_release);
    header_->heartbeat_ns.store(now, std::memory_order_release);
    header_->consumer_pid.store(SharedMapping::self_pid(), std::memory_order_release);
    attached_ = true;
}

void OutputRing::heartbeat(int64_t now) {
    if (valid()) header_->heartbeat_ns.store(now, std::memory_order_release);
}

void OutputRing::report_seen(uint64_t seq) {
    if (valid()) header_->last_seen_seq.store(seq, std::memory_order_release);
}

void OutputRing::write_request(const OutputRequest& req) {
    if (!valid()) return;
    ringslot::write_record(&header_->request_lock, &header_->request_generation, &header_->request, req);
}

bool OutputRing::read_reply(OutputReply* out, uint64_t* known, uint64_t* reply_for) const {
    if (!valid() || out == nullptr || known == nullptr) return false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint64_t s1 = header_->reply_lock.load(std::memory_order_acquire);
        if ((s1 & 1u) != 0u) continue;
        const uint64_t gen = header_->reply_generation.load(std::memory_order_acquire);
        if (gen == 0u || gen == *known) return false;
        OutputReply copy = header_->reply;
        const uint64_t for_gen = header_->reply_for;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (header_->reply_lock.load(std::memory_order_acquire) != s1) continue;
        copy.message[kOutputMessageBytes - 1u] = 0;
        *out = copy;
        *known = gen;
        if (reply_for != nullptr) *reply_for = for_gen;
        return true;
    }
    return false;
}

void OutputRing::write_status(const char* text) {
    if (!valid()) return;
    char buf[kOutputStatusBytes] {};
    if (text != nullptr) std::strncpy(buf, text, kOutputStatusBytes - 1u);
    const uint64_t s = header_->status_lock.load(std::memory_order_relaxed);
    header_->status_lock.store(s + 1u, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(header_->status, buf, sizeof buf);
    header_->status_generation.fetch_add(1u, std::memory_order_release);
    header_->status_lock.store(s + 2u, std::memory_order_release);
}

uint64_t OutputRing::request_generation() const {
    return valid() ? header_->request_generation.load(std::memory_order_acquire) : 0u;
}

bool OutputRing::acquire_latest(uint64_t* last_seen, OutputFrameDesc* out_desc, const void** out_pixels) {
    if (!valid()) return false;
    return ringslot::acquire_latest<OutputRingHeader, OutputSlotHeader, OutputFrameDesc>(
        map_.base(), header_, &held_, last_seen, out_desc, out_pixels);
}

void OutputRing::release() {
    if (!valid()) return;
    ringslot::release(header_, &held_);
}

bool OutputRing::producer_alive() const {
    return valid() && process_alive(header_->producer_pid);
}

bool OutputRing::launched_by_producer() const {
    return valid() && (header_->header_flags & kOutputLaunchedByProducer) != 0u;
}

uint32_t OutputRing::producer_protocol() const {
    return valid() ? header_->producer_protocol : 0u;
}

OutputState OutputRing::state() const {
    return header_ != nullptr ? static_cast<OutputState>(header_->state.load(std::memory_order_acquire))
                              : OutputState::Retired;
}

}  // namespace qcbae
