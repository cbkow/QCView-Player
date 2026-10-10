// Vendored from QCBridgeAE (github.com/cbkow/QCBridgeAE) at db8bf2a,
// src/common/surface/audio_ring.h. Keep identical to the plugin's copy;
// kAudioDescVersion guards a mismatch.
// QCBridgeAE — the shared audio ring (DESIGN-NOTES D6).
//
// A fixed set of slots in shared memory, one packet per slot, next to the
// frame ring and independent of it. Producer (the Transmit device, on the
// host's audio thread) writes; consumer (QCView) copies packets out in
// order. Unlike the frame ring there is no reader claim: the writer never
// waits for anyone, because it runs on a thread the host forbids to block.
// A consumer that falls behind by a ring's worth is lapped, notices, and
// resynchronises near the newest packet.
//
// Concurrency contract, no locks:
//   * Each slot carries a seqlock, exactly as SlotHeader does: odd = a write
//     is in progress. The writer stores odd, fences, writes samples and the
//     descriptor, stores even, then publishes `latest`. The reader loads the
//     seq, copies, fences, loads again, and discards the copy on a mismatch
//     or when the descriptor's sequence is not the one it expected.
//   * Packet p lives in slot (p - 1) % slot_count, always: sequences are
//     monotonic and slots are never skipped, so a reader walks p = last + 1
//     and the slot follows. (The frame ring cannot promise this because it
//     steps over the consumer's claim; this ring has no claim.)
//   * The session record has its own seqlock, like the frame ring's ICC
//     region, because the host thread writes it while the consumer polls.
//   * The payload is read with a plain memcpy under the seqlock. Formally a
//     data race; in practice the pattern the frame ring already relies on,
//     validated the same way, by a two-process tearing test.
//
// Geometry is fixed at create and format-independent: every slot has room
// for max_channels planes of slot_frames floats, whatever the session's
// channel count. So one segment serves a whole host session, across every
// StartPushAudio/StopPushAudio, and is never re-created for a format change
// — which is what keeps the Windows name-replace retry out of play start.

#pragma once

#include "decode/qcbae/audio_desc.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace qcbae {

static_assert(std::atomic<uint64_t>::is_always_lock_free, "the seqlocks must be lock-free across processes");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "the state words must be lock-free across processes");

struct AudioRingHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t producer_pid;
    uint32_t slot_count;

    uint32_t slot_frames;      // frames of room per slot (per channel)
    uint32_t max_channels;     // planes of room per slot
    uint32_t _pad0[2];

    uint64_t slot_stride;      // bytes between slot starts (64-byte multiple)
    uint64_t slots_offset;     // from mapping start; page-aligned
    uint64_t samples_offset;   // from slot start to plane 0; 64-byte aligned
    uint64_t total_size;

    int64_t  time_scale;       // ticks per second of every time_value in here

    // Sequence of the newest committed packet. 0 = nothing yet.
    std::atomic<uint64_t> latest;

    std::atomic<uint32_t> state;        // AudioState
    std::atomic<uint32_t> host_audio;   // HostAudio

    // Session record seqlock (odd = writing) and its generation, bumped on
    // every begin_session so a consumer knows to re-read and flush.
    std::atomic<uint64_t> session_seq;
    std::atomic<uint64_t> generation;
    AudioSession          session;
};

struct AudioSlotHeader {
    std::atomic<uint64_t> seq;   // seqlock; odd = write in progress
    AudioPacketDesc       desc;
};
static_assert(sizeof(AudioSlotHeader) == 48, "slot header grew; samples_offset keeps planes 64-byte aligned regardless");

// Sample planes start here inside a slot, so SIMD code on either side gets
// aligned rows. Not a page: nothing maps these as textures.
inline constexpr uint64_t kAudioPlaneAlignment = 64u;

class AudioRing {
public:
    AudioRing() = default;
    ~AudioRing();
    AudioRing(AudioRing&&) noexcept;
    AudioRing& operator=(AudioRing&&) noexcept;
    AudioRing(const AudioRing&) = delete;
    AudioRing& operator=(const AudioRing&) = delete;

    // Producer side. `name` must start with '/' and be <= kMaxShmName bytes.
    // `time_scale` is the host's ticks per second, stamped into the header
    // so the consumer never needs the producer alive to interpret a time.
    bool create(const std::string& name, int64_t time_scale,
                uint32_t slot_count = kDefaultAudioSlots,
                uint32_t slot_frames = kDefaultAudioSlotFrames,
                uint32_t max_channels = kMaxAudioChannels);

    // Consumer side.
    bool open(const std::string& name);

    bool valid() const { return base_ != nullptr; }
    const std::string& error() const { return error_; }
    const AudioRingHeader* header() const { return header_; }
    const std::string& name() const { return name_; }

    // --- Producer, host thread ---------------------------------------------
    void begin_session(const AudioSession& s);   // seqlock write, generation++, state Pushing
    void end_session();                          // state Idle
    void set_state(AudioState s);
    void set_host_audio(HostAudio h);

    // --- Producer, any thread (the host's audio thread) ----------------------
    // Writes `frames` frames of `channels` planes as one or more packets
    // (split at slot_frames, contiguous, time advanced by frames * time_scale
    // / sample_rate per packet). Never blocks, never allocates, never logs.
    // Channels beyond max_channels are dropped. Returns packets written.
    uint32_t push(const float* const* planes, uint32_t channels, uint32_t sample_rate,
                  uint32_t frames, int64_t time_value, uint32_t flags);

    // --- Consumer -------------------------------------------------------------
    enum class Next { None, Packet, Resynced };

    // Delivers packet `*last_seen + 1` if it is still intact, copying its
    // planes into `planes_out` (room for max_channels * slot_frames floats,
    // plane c at c * slot_frames; only `desc.channels` planes are written).
    // Resynced: the reader was lapped (or is starting cold on a ring that
    // already moved on); `*last_seen` has been moved to just behind the
    // newest packet and `*dropped` counts what was skipped. Call again.
    Next next_packet(uint64_t* last_seen, AudioPacketDesc* out_desc, float* planes_out,
                     uint64_t* dropped);

    // Start consuming from the newest packet onwards, ignoring history. For
    // a live consumer that just opened the ring.
    void skip_to_latest(uint64_t* last_seen) const;

    // Copies the session out if its generation differs from the caller's.
    // Returns true and updates `known_generation` on a change.
    bool read_session(AudioSession* out, uint64_t* known_generation) const;

    AudioState state() const;
    HostAudio  host_audio() const;
    uint64_t   generation() const;
    uint64_t   latest() const;

private:
    void close();
    AudioSlotHeader* slot_at(uint32_t index) const;
    float* planes_at(uint32_t index) const;

    void*            base_    = nullptr;
    size_t           size_    = 0;
    AudioRingHeader* header_  = nullptr;
#if defined(_WIN32)
    void*            mapping_ = nullptr;
#else
    int              fd_      = -1;
#endif
    bool             owner_   = false;
    std::string      name_;
    std::string      error_;

    // Producer bookkeeping; touched only by the pushing thread.
    uint64_t write_seq_    = 0;
    uint64_t write_frames_ = 0;
};

}  // namespace qcbae
