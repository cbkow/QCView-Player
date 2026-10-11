// Vendored from QCBridgeAE (github.com/cbkow/QCBridgeAE, MIT — see its
// src/common/LICENSE) at 3d7de5b, src/common/surface/output_ring.h. The wire contract between the
// QCBridgeAE Transmit device and QCView, and of QCView's own output bus; keep
// in sync by re-copying, never by editing here. A version mismatch is refused
// at open, never misread.

// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2026 cbkow. See src/common/LICENSE: the ring and sidecar
// files are MIT in addition to the repository's GPL so that consumers under
// any licence can carry the wire contract.
// QCBridgeAE — the output ring: QCView's corrected canvas, out to a sink.
//
// The frame ring with the roles reversed and a consumer block added. The
// producer (QCView) creates it when the user enables an output, publishes a
// frame into a page-aligned slot each time the canvas changes, and reads the
// sink's request and heartbeat. The consumer (the sink process) opens it by
// name, writes a request for the raster it wants, keeps a heartbeat fresh,
// and samples the newest slot in place — handing the slot pointer straight
// to its SDK. Lossy by design (QCBridgeN/PLAN.md O5): the producer never
// waits for the sink.
//
// Lifecycle (O13) as the ring sees it:
//   * The ring exists while the output is enabled; Retired in host_state
//     says the producer is done with this mapping (re-open the name later).
//   * consumer_pid + heartbeat_ns say a sink is attached; the producer
//     renders only while consumer_present() holds. A sink that closes
//     cleanly clears its pid; one that crashes is found by the stale
//     heartbeat and process_alive().
//   * kOutputLaunchedByProducer in header_flags tells a sink it was started
//     by the producer and should quit, not wait, when the rings go.
//
// Everything that crosses is a plain struct or an atomic; no pointers, no
// strings beyond fixed NUL-terminated buffers.

#pragma once

#include "decode/qcbae/output_desc.h"
#include "decode/qcbae/ring_mapping.h"
#include "decode/qcbae/ring_slots.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace qcbae {

// Reused from the frame ring: Active while frames may come, Paused when the
// producer has nothing to publish (no source), Retired when the mapping is
// finished. PausedFocus does not apply.
enum class OutputState : uint32_t {
    Active  = 0,
    Paused  = 2,
    Retired = 3,
};

struct OutputRingHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t slot_count;
    uint32_t producer_pid;

    uint64_t slot_stride;      // bytes between slot starts
    uint64_t pixels_offset;    // from slot start to pixels (page-aligned)
    uint64_t pixels_capacity;  // usable pixel bytes per slot
    uint64_t slots_offset;     // from mapping start; page-aligned
    uint64_t total_size;

    uint32_t producer_protocol;   // kOutputDescVersion of the producer's copy
    uint32_t header_flags;        // OutputHeaderFlags

    std::atomic<uint64_t> latest;        // pack_latest(sequence, slot); 0 = nothing yet
    std::atomic<uint64_t> reader_claim;  // the packed value the consumer holds, or 0
    std::atomic<uint32_t> state;         // OutputState
    uint32_t              _pad0;

    // --- consumer-written ---------------------------------------------------
    std::atomic<uint32_t> consumer_pid;       // 0 = none attached
    std::atomic<uint32_t> consumer_protocol;  // the sink's kOutputDescVersion
    std::atomic<int64_t>  heartbeat_ns;       // OutputRing::now_ns() at the sink's last beat
    std::atomic<uint64_t> last_seen_seq;      // newest sequence the sink has taken (its own report)

    std::atomic<uint64_t> request_lock;       // seqlock
    std::atomic<uint64_t> request_generation; // 0 = never written
    OutputRequest         request;

    std::atomic<uint64_t> status_lock;
    std::atomic<uint64_t> status_generation;
    char                  status[kOutputStatusBytes];

    // --- producer-written ---------------------------------------------------
    std::atomic<uint64_t> reply_lock;
    std::atomic<uint64_t> reply_generation;   // 0 = never answered
    uint64_t              reply_for;          // request_generation the reply answers
    OutputReply           reply;
};

struct OutputSlotHeader {
    std::atomic<uint64_t> seq;   // seqlock; odd = write in progress
    OutputFrameDesc       desc;
};

class OutputRing {
public:
    OutputRing() = default;
    ~OutputRing();
    OutputRing(OutputRing&&) noexcept;
    OutputRing& operator=(OutputRing&&) noexcept;
    OutputRing(const OutputRing&) = delete;
    OutputRing& operator=(const OutputRing&) = delete;

    // "/qcview-out-" + id, or empty when the id is too long or not a plain
    // [a-z0-9-] token. The audio ring beside it is name + "-audio".
    static std::string name_for(const std::string& plugin_id);
    static std::string audio_name_for(const std::string& plugin_id);

    // The clock every stamp uses: a steady, system-wide nanosecond count that
    // two processes on the same machine can compare (mach_absolute_time on
    // macOS, QueryPerformanceCounter on Windows, CLOCK_MONOTONIC elsewhere).
    static int64_t now_ns();

    // --- Producer -------------------------------------------------------------
    // `pixels_capacity` bounds the largest frame this mapping will carry; a
    // request needing more is answered TooLarge until the producer re-creates
    // the ring larger (retire + create, as the Transmit device does).
    bool create(const std::string& name, uint64_t pixels_capacity,
                uint32_t slot_count = kDefaultSlots, uint32_t header_flags = 0u);

    // Re-reads the sink's request when its generation changed. `*known` is
    // the caller's last generation (start at 0).
    bool read_request(OutputRequest* out, uint64_t* known) const;
    // Answers the request of generation `request_generation`.
    void write_reply(const OutputReply& reply, uint64_t request_generation);

    // A sink is attached, alive, and beat within `max_age_ns`.
    bool consumer_present(int64_t now, int64_t max_age_ns = 1000000000) const;
    uint32_t consumer_pid() const;
    uint64_t consumer_last_seen() const;
    // Copies the sink's status line out on a generation change.
    bool read_status(std::string* out, uint64_t* known) const;

    void* begin_write(uint64_t bytes);
    void  commit(const OutputFrameDesc& desc);
    void  abandon();
    void  set_state(OutputState s);
    uint64_t newest_seq() const;

    // --- Consumer -------------------------------------------------------------
    bool open(const std::string& name);

    // Announce this process; call once after open, then heartbeat() often.
    void attach(int64_t now);
    void heartbeat(int64_t now);
    // Reports the newest sequence taken, for the producer's status line.
    void report_seen(uint64_t seq);
    void write_request(const OutputRequest& req);
    // The reply to the latest request, when it changed since `*known`.
    bool read_reply(OutputReply* out, uint64_t* known, uint64_t* reply_for = nullptr) const;
    void write_status(const char* text);
    uint64_t request_generation() const;

    bool acquire_latest(uint64_t* last_seen, OutputFrameDesc* out_desc, const void** out_pixels);
    void release();
    uint32_t held_slot() const { return latest_slot(held_); }

    bool producer_alive() const;
    bool launched_by_producer() const;
    uint32_t producer_protocol() const;

    // --- Both -------------------------------------------------------------------
    bool valid() const { return map_.valid(); }
    const std::string& error() const { return error_.empty() ? map_.error() : error_; }
    const OutputRingHeader* header() const { return header_; }
    const std::string& name() const { return map_.name(); }
    OutputState state() const;

private:
    void close();

    SharedMapping     map_;
    OutputRingHeader* header_ = nullptr;
    std::string       error_;
    ringslot::Writer     writer_;
    uint64_t          held_   = 0;
    bool              attached_ = false;
};

}  // namespace qcbae
