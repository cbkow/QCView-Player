// Vendored from QCBridgeAE (github.com/cbkow/QCBridgeAE) at db8bf2a,
// src/common/protocol/audio_desc.h. Keep identical to the plugin's copy;
// kAudioDescVersion guards a mismatch.
// QCBridgeAE — the audio sidecar schema (DESIGN-NOTES D6).
//
// Audio crosses in its own segment, next to the frame ring, because the frame
// ring's version is checked exactly by every consumer and audio must not
// break a viewer that predates it. The segment carries what Premiere pushes
// to a "mirror" Transmit device, as pushed: planar float32, the sequence's
// channel count, each packet stamped with the timeline position of its first
// sample (A8, lab/results/2026-10-10-a8-transmit-audio). No clock of ours,
// no resampling, no fold — those are the consumer's, by its own settings.
//
// Same wire rules as frame_desc.h: POD only, fixed-width types, explicit
// padding, no pointers, never reorder or resize a field — add at the end and
// bump kAudioDescVersion.

#pragma once

#include <cstdint>

namespace qcbae {

inline constexpr uint32_t kAudioRingMagic   = 0x51434141u;  // 'QCAA'
inline constexpr uint32_t kAudioDescVersion = 1u;

// The frame ring's name plus this suffix: "/qcbae-premiere" -> "/qcbae-premiere-audio"
// (21 bytes, inside macOS's 31-byte PSHMNAMLEN). After Effects never pushes
// audio (A8), so only Premiere's exists in practice; the rule is per host.
inline constexpr const char* kAudioRingSuffix = "-audio";

// The host's channel count is capped here, not by what it sends: Premiere
// labels up to 16 (PrSDKTransmit kMaxTransmitAudioChannels).
inline constexpr uint32_t kMaxAudioChannels = 16u;

// Geometry defaults. 1024 frames is what the device asks the host for per
// push (21.3 ms at 48 kHz — the host honours it exactly); 128 slots is 2.7 s
// of history, more than any consumer's buffer and enough to spot a stall.
inline constexpr uint32_t kDefaultAudioSlots      = 128u;
inline constexpr uint32_t kDefaultAudioSlotFrames = 1024u;
inline constexpr uint32_t kMinAudioSlots          = 4u;

// What the producer is doing, for a consumer receiving no packets.
enum class AudioState : uint32_t {
    Idle    = 0,   // segment exists, no push session running (the host is not playing)
    Pushing = 1,   // between StartPushAudio and StopPushAudio
    Retired = 2,   // this mapping is finished; close it and re-open the name
};

// Whether the host has this device's audio switched on. The device cannot
// ask; it infers: Premiere pushes video in Playing mode without ever
// starting an audio session exactly when "Audio Stream" is unticked for the
// device in Preferences > Playback. A consumer turns Off into that sentence.
enum class HostAudio : uint32_t {
    Unknown = 0,
    Off     = 1,
    On      = 2,
};

enum AudioSessionFlags : uint32_t {
    kAudioSessionLoop      = 1u << 0,
    kAudioSessionScrubbing = 1u << 1,
};

enum AudioPacketFlags : uint32_t {
    kAudioPacketScrubbing  = 1u << 0,   // pushed during a scrub session
};

// One per StartPushAudio: what the host said playback is. The consumer reads
// it on a generation change, flushes, and reads on.
struct AudioSession {
    int64_t  start_time;    // timeline ticks (time_scale in the header)
    int64_t  in_time;
    int64_t  out_time;
    float    speed;         // 1.0 normal, -1.0 reverse; the host renders the samples accordingly
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t flags;         // AudioSessionFlags
    uint32_t push_frames;   // what the device asked for per push
    uint32_t _pad0;
};
static_assert(sizeof(AudioSession) == 48, "AudioSession is a wire format — size change needs a version bump");

// One per packet, in the packet's slot.
struct AudioPacketDesc {
    uint64_t packet_seq;    // 1-based, monotonic; equals the slot's sequence
    uint64_t first_frame;   // position of frames[0] in the session's sample stream
    int64_t  time_value;    // timeline ticks of frames[0]; exact, never a double
    uint32_t frames;        // sample frames in this packet (<= slot_frames)
    uint32_t channels;      // planes present (<= max_channels)
    uint32_t sample_rate;
    uint32_t flags;         // AudioPacketFlags
};
static_assert(sizeof(AudioPacketDesc) == 40, "AudioPacketDesc is a wire format — size change needs a version bump");

static_assert(sizeof(float) == 4, "the wire carries IEEE binary32");

}  // namespace qcbae
