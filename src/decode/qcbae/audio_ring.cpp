// Vendored from QCBridgeAE (github.com/cbkow/QCBridgeAE, MIT — see its
// src/common/LICENSE) at 5849977, src/common/surface/audio_ring.cpp. The wire contract between the
// QCBridgeAE Transmit device and QCView, and of QCView's own output bus; keep
// in sync by re-copying, never by editing here. A version mismatch is refused
// at open, never misread.

// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2026 cbkow. See src/common/LICENSE: the ring and sidecar
// files are MIT in addition to the repository's GPL so that consumers under
// any licence can carry the wire contract.
#include "decode/qcbae/audio_ring.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace qcbae {
namespace {

uint64_t audio_total(const void* header) {
    return static_cast<const AudioRingHeader*>(header)->total_size;
}

void audio_retire(void* header) {
    static_cast<AudioRingHeader*>(header)->state.store(
        static_cast<uint32_t>(AudioState::Retired), std::memory_order_release);
}

}  // namespace

AudioRing::~AudioRing() { close(); }

AudioRing::AudioRing(AudioRing&& o) noexcept { *this = std::move(o); }

AudioRing& AudioRing::operator=(AudioRing&& o) noexcept {
    if (this != &o) {
        close();
        map_          = std::move(o.map_);
        header_       = o.header_;       o.header_ = nullptr;
        error_        = std::move(o.error_);
        write_seq_    = o.write_seq_;    o.write_seq_ = 0;
        write_frames_ = o.write_frames_; o.write_frames_ = 0;
    }
    return *this;
}

void AudioRing::close() {
    map_.close();
    header_ = nullptr;
}

AudioSlotHeader* AudioRing::slot_at(uint32_t index) const {
    auto* bytes = static_cast<uint8_t*>(map_.base());
    return reinterpret_cast<AudioSlotHeader*>(bytes + header_->slots_offset
                                              + index * header_->slot_stride);
}

float* AudioRing::planes_at(uint32_t index) const {
    auto* bytes = static_cast<uint8_t*>(map_.base());
    return reinterpret_cast<float*>(bytes + header_->slots_offset + index * header_->slot_stride
                                    + header_->samples_offset);
}

bool AudioRing::create(const std::string& name, int64_t time_scale, uint32_t slot_count,
                       uint32_t slot_frames, uint32_t max_channels) {
    close();
    error_.clear();
    if (slot_count < kMinAudioSlots) { error_ = "slot_count must be >= 4"; return false; }
    if (slot_frames == 0u)           { error_ = "slot_frames must be > 0"; return false; }
    if (max_channels == 0u || max_channels > kMaxAudioChannels) {
        error_ = "max_channels must be 1..16"; return false;
    }
    if (time_scale <= 0) { error_ = "time_scale must be positive"; return false; }

    const uint64_t ps          = SharedMapping::page_size();
    const uint64_t head_bytes  = SharedMapping::round_up(sizeof(AudioRingHeader), ps);
    const uint64_t samples_off = SharedMapping::round_up(sizeof(AudioSlotHeader), kAudioPlaneAlignment);
    const uint64_t plane_bytes = static_cast<uint64_t>(slot_frames) * sizeof(float);
    const uint64_t stride      = SharedMapping::round_up(samples_off + plane_bytes * max_channels, kAudioPlaneAlignment);
    const uint64_t total       = head_bytes + stride * slot_count;

    // Same dance as the frame ring on Windows: a section cannot be unlinked,
    // so a name still held by a consumer hands back the old object. The
    // mapping marks that one Retired and retries while its holders let go.
    if (!map_.create(name, total, head_bytes, kAudioRingMagic, audio_retire)) return false;
    write_seq_ = 0; write_frames_ = 0;

    header_ = static_cast<AudioRingHeader*>(map_.base());
    header_->magic          = kAudioRingMagic;
    header_->version        = kAudioDescVersion;
    header_->producer_pid   = SharedMapping::self_pid();
    header_->slot_count     = slot_count;
    header_->slot_frames    = slot_frames;
    header_->max_channels   = max_channels;
    header_->slot_stride    = stride;
    header_->slots_offset   = head_bytes;
    header_->samples_offset = samples_off;
    header_->total_size     = total;
    header_->time_scale     = time_scale;
    // Atomics zeroed by the memset: latest 0 (nothing yet), state Idle,
    // host_audio Unknown, generation 0 (no session yet).
    return true;
}

bool AudioRing::open(const std::string& name) {
    close();
    error_.clear();
    if (!map_.open(name, sizeof(AudioRingHeader), kAudioRingMagic, kAudioDescVersion,
                   audio_total, "audio ring"))
        return false;
    header_ = static_cast<AudioRingHeader*>(map_.base());
    // A foreign or corrupt header could claim a geometry that does not fit
    // its own size; refuse before any slot arithmetic trusts it.
    const uint64_t total = map_.size();
    const uint64_t need = header_->slots_offset
                        + static_cast<uint64_t>(header_->slot_count) * header_->slot_stride;
    if (header_->slot_count < kMinAudioSlots || header_->slot_frames == 0u
        || header_->max_channels == 0u || header_->max_channels > kMaxAudioChannels
        || header_->samples_offset + static_cast<uint64_t>(header_->slot_frames) * sizeof(float)
             * header_->max_channels > header_->slot_stride
        || need > total) {
        error_ = "audio ring geometry does not fit its mapping";
        close(); return false;
    }
    return true;
}

// --- Producer ------------------------------------------------------------------

void AudioRing::begin_session(const AudioSession& s) {
    if (!valid()) return;
    const uint64_t q = header_->session_seq.load(std::memory_order_relaxed);
    header_->session_seq.store(q + 1u, std::memory_order_release);   // odd: writing
    std::atomic_thread_fence(std::memory_order_release);
    header_->session = s;
    header_->generation.fetch_add(1u, std::memory_order_release);
    header_->session_seq.store(q + 2u, std::memory_order_release);   // even: stable
    header_->state.store(static_cast<uint32_t>(AudioState::Pushing), std::memory_order_release);
}

void AudioRing::end_session() {
    set_state(AudioState::Idle);
}

void AudioRing::set_state(AudioState s) {
    if (header_ != nullptr) header_->state.store(static_cast<uint32_t>(s), std::memory_order_release);
}

void AudioRing::set_host_audio(HostAudio h) {
    if (header_ != nullptr) header_->host_audio.store(static_cast<uint32_t>(h), std::memory_order_release);
}

uint32_t AudioRing::push(const float* const* planes, uint32_t channels, uint32_t sample_rate,
                         uint32_t frames, int64_t time_value, uint32_t flags) {
    if (!valid() || planes == nullptr || frames == 0u || sample_rate == 0u) return 0u;
    const uint32_t count       = header_->slot_count;
    const uint32_t slot_frames = header_->slot_frames;
    const uint32_t ch          = std::min(channels, header_->max_channels);
    uint32_t written = 0u;
    uint32_t offset  = 0u;   // frames already consumed from each plane

    while (frames > 0u) {
        const uint32_t n     = std::min(frames, slot_frames);
        const uint64_t seq   = write_seq_ + 1u;
        const uint32_t index = static_cast<uint32_t>((seq - 1u) % count);
        AudioSlotHeader* slot = slot_at(index);
        float* dst = planes_at(index);

        const uint64_t s = slot->seq.load(std::memory_order_relaxed);
        slot->seq.store(s + 1u, std::memory_order_release);   // odd: write in progress
        std::atomic_thread_fence(std::memory_order_release);

        for (uint32_t c = 0; c < ch; ++c) {
            if (planes[c] != nullptr)
                std::memcpy(dst + static_cast<size_t>(c) * slot_frames, planes[c] + offset,
                            static_cast<size_t>(n) * sizeof(float));
            else
                std::memset(dst + static_cast<size_t>(c) * slot_frames, 0, static_cast<size_t>(n) * sizeof(float));
        }
        slot->desc.packet_seq  = seq;
        slot->desc.first_frame = write_frames_;
        slot->desc.time_value  = time_value;
        slot->desc.frames      = n;
        slot->desc.channels    = ch;
        slot->desc.sample_rate = sample_rate;
        slot->desc.flags       = flags;

        slot->seq.store(s + 2u, std::memory_order_release);   // even: stable
        header_->latest.store(seq, std::memory_order_release);

        write_seq_     = seq;
        write_frames_ += n;
        // Exact for 48 kHz against Premiere's 254016000000 ticks/s; the
        // division is the only rounding, and it is per packet, not cumulative,
        // because the host restates the time on every push anyway.
        time_value += static_cast<int64_t>(n) * header_->time_scale / static_cast<int64_t>(sample_rate);
        offset += n;
        frames -= n;
        ++written;
    }
    return written;
}

// --- Consumer ------------------------------------------------------------------

void AudioRing::skip_to_latest(uint64_t* last_seen) const {
    if (!valid() || last_seen == nullptr) return;
    *last_seen = header_->latest.load(std::memory_order_acquire);
}

AudioRing::Next AudioRing::next_packet(uint64_t* last_seen, AudioPacketDesc* out_desc,
                                       float* planes_out, uint64_t* dropped) {
    if (!valid() || last_seen == nullptr) return Next::None;
    const uint64_t latest = header_->latest.load(std::memory_order_acquire);
    if (latest == 0u || latest == *last_seen) return Next::None;
    const uint32_t count = header_->slot_count;

    // Behind by a ring's worth, or ahead of it (a replaced ring): the slot we
    // want is being, or has been, rewritten. Rejoin just behind the newest
    // packet, leaving the slot after it (the writer's next target) alone.
    auto resync = [&]() {
        const uint64_t keep = count - 2u;
        const uint64_t target = latest > keep ? latest - keep : 0u;
        if (dropped != nullptr && target > *last_seen) *dropped += target - *last_seen;
        *last_seen = target;
        return Next::Resynced;
    };
    if (*last_seen > latest || latest - *last_seen > count - 2u) return resync();

    const uint64_t p = *last_seen + 1u;
    AudioSlotHeader* slot = slot_at(static_cast<uint32_t>((p - 1u) % count));
    const uint64_t s1 = slot->seq.load(std::memory_order_acquire);
    if ((s1 & 1u) != 0u) return resync();   // the writer is on this slot: we are lapped

    AudioPacketDesc desc = slot->desc;
    const uint32_t slot_frames = header_->slot_frames;
    if (desc.frames > slot_frames || desc.channels > header_->max_channels) return resync();
    if (planes_out != nullptr) {
        const float* src = planes_at(static_cast<uint32_t>((p - 1u) % count));
        for (uint32_t c = 0; c < desc.channels; ++c)
            std::memcpy(planes_out + static_cast<size_t>(c) * slot_frames,
                        src + static_cast<size_t>(c) * slot_frames,
                        static_cast<size_t>(desc.frames) * sizeof(float));
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint64_t s2 = slot->seq.load(std::memory_order_acquire);
    // Compare against the sequence we were told, not merely desc against
    // samples: a stale slot agrees with itself.
    if (s1 != s2 || desc.packet_seq != p) return resync();

    *last_seen = p;
    if (out_desc != nullptr) *out_desc = desc;
    return Next::Packet;
}

bool AudioRing::read_session(AudioSession* out, uint64_t* known_generation) const {
    if (!valid() || out == nullptr || known_generation == nullptr) return false;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const uint64_t q1 = header_->session_seq.load(std::memory_order_acquire);
        if ((q1 & 1u) != 0u) continue;
        const uint64_t gen = header_->generation.load(std::memory_order_acquire);
        if (gen == 0u || gen == *known_generation) return false;
        AudioSession s = header_->session;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (header_->session_seq.load(std::memory_order_acquire) != q1) continue;
        *out = s;
        *known_generation = gen;
        return true;
    }
    return false;
}

AudioState AudioRing::state() const {
    return header_ != nullptr ? static_cast<AudioState>(header_->state.load(std::memory_order_acquire))
                              : AudioState::Retired;
}

HostAudio AudioRing::host_audio() const {
    return header_ != nullptr ? static_cast<HostAudio>(header_->host_audio.load(std::memory_order_acquire))
                              : HostAudio::Unknown;
}

uint64_t AudioRing::generation() const {
    return header_ != nullptr ? header_->generation.load(std::memory_order_acquire) : 0u;
}

uint64_t AudioRing::latest() const {
    return header_ != nullptr ? header_->latest.load(std::memory_order_acquire) : 0u;
}

}  // namespace qcbae
