// Vendored from QCBridgeAE (github.com/cbkow/QCBridgeAE, MIT — see its
// src/common/LICENSE) at 3d7de5b, src/common/protocol/output_desc.h. The wire contract between the
// QCBridgeAE Transmit device and QCView, and of QCView's own output bus; keep
// in sync by re-copying, never by editing here. A version mismatch is refused
// at open, never misread.

// SPDX-License-Identifier: MIT OR GPL-3.0-or-later
// Copyright (c) 2026 cbkow. See src/common/LICENSE: the ring and sidecar
// files are MIT in addition to the repository's GPL so that consumers under
// any licence can carry the wire contract.
// QCBridgeAE — the output bus sidecar: what QCView says about a frame it
// publishes, and the request/reply a sink and QCView exchange first.
//
// The Transmit rings carry a host's picture INTO QCView. The output bus
// carries QCView's corrected canvas OUT, to a sink process (an NDI sender, an
// SDI card, a recorder) that reads it from shared memory. Same slot and
// seqlock machinery (ring_slots.h), its own magic and version so the
// Transmit contract never moves for it. The decisions are QCBridgeN/PLAN.md
// O2–O9; the page-long protocol there is this header plus output_ring.h.
//
// Negotiation (O4): the sink writes an OutputRequest; the producer replies
// with an OutputReply that either restates what it will produce or names
// why it refuses. No request, no frames. A sink whose request is refused
// may write another; the producer re-reads on every generation change.
//
// Audio travels in the ordinary audio ring beside this one
// (`<name>-audio`, audio_desc.h): a session of 48 kHz stereo, packets
// stamped on the same clock as the frames, kAudioPacketMuted for the feed
// mute. Nothing here duplicates that.

#pragma once

#include <cstdint>

namespace qcbae {

inline constexpr uint32_t kOutputRingMagic    = 0x5143564Fu;   // 'QCVO'
inline constexpr uint32_t kOutputDescVersion  = 1u;

// Ring names: this prefix plus the sink's manifest id, at most 19 bytes of
// it, so "/qcview-out-ndi" and its "-audio" sibling both fit kMaxShmName.
inline constexpr const char* kOutputRingPrefix = "/qcview-out-";
inline constexpr uint32_t    kOutputIdMaxBytes = 13u;   // 31 - 12 (prefix) - 6 ("-audio")

// The canvas cap (O3): QCView never renders an output larger than this,
// whatever the source. A request above it is refused with TooLarge.
inline constexpr uint32_t kOutputMaxWidth  = 3840u;
inline constexpr uint32_t kOutputMaxHeight = 2160u;

inline constexpr uint32_t kOutputStatusBytes  = 128u;   // the sink's status line, NUL-terminated
inline constexpr uint32_t kOutputMessageBytes = 96u;    // the producer's refusal text, NUL-terminated

// Rows are padded to this, as the Transmit frames are, so a slot maps
// straight into a GPU texture on either side.
inline constexpr uint32_t kOutputRowAlignment = 256u;

enum class OutputPixelFormat : uint32_t {
    Unspecified = 0,
    BGRA8       = 1,   // 8-bit, the order NDI and every 8-bit card take natively
    RGBA16F     = 2,   // half float, for HDR transfers and scene-referred sinks
    RGB10       = 3,   // 10-bit RGB in a 32-bit word: R bits 0-9, G 10-19, B 20-29, top two
                       // unused. The GPU's R10G10B10A2 render target and AJA's 10-bit
                       // RGB frame store share this layout, so neither side repacks.
                       // Full range; the transfer says what the values encode.
    // P216 and v210 join here when a sink asks (O7); the output shader writes
    // them, the sink never repacks.
};

inline constexpr uint32_t output_bytes_per_pixel(OutputPixelFormat f) {
    switch (f) {
        case OutputPixelFormat::BGRA8:   return 4u;
        case OutputPixelFormat::RGBA16F: return 8u;
        case OutputPixelFormat::RGB10:   return 4u;
        default:                         return 0u;
    }
}

inline constexpr uint32_t output_aligned_bytes_per_row(uint32_t width, OutputPixelFormat f) {
    const uint32_t tight = width * output_bytes_per_pixel(f);
    return (tight + kOutputRowAlignment - 1u) / kOutputRowAlignment * kOutputRowAlignment;
}

inline constexpr uint64_t output_frame_bytes(uint32_t width, uint32_t height, OutputPixelFormat f) {
    return static_cast<uint64_t>(output_aligned_bytes_per_row(width, f)) * height;
}

// The display transform the sink wants the canvas rendered for. QCView maps
// it to an OCIO display/view (the user may override per output).
enum class OutputTransfer : uint32_t {
    Unspecified = 0,
    SdrRec709   = 1,   // Rec.709 primaries, BT.1886 / sRGB-class display
    PqRec2020   = 2,   // ST 2084 in Rec.2020
    HlgRec2020  = 3,
};

// How a canvas whose aspect differs from the raster is placed.
enum class OutputFit : uint32_t {
    Unspecified = 0,
    Letterbox   = 1,   // aspect-fit into width x height, bars around
    Native      = 2,   // the raster becomes the canvas's own aspect at the requested height
};

// Which picture (O3): the whole composite as displayed, or one side of a
// dual view alone. Side A with no layers is the clean source.
enum class OutputContent : uint32_t {
    Unspecified = 0,
    Composite   = 1,
    SideA       = 2,
    SideB       = 3,
};

// Layers the sink wants included (O14: both on by default).
enum OutputLayers : uint32_t {
    kOutputLayerGuides      = 1u << 0,   // safety guides
    kOutputLayerAnnotations = 1u << 1,
};

enum class OutputRefusal : uint32_t {
    None                = 0,
    VersionMismatch     = 1,   // the request's protocol_version is not ours
    TooLarge            = 2,   // above kOutputMaxWidth x kOutputMaxHeight or the ring's slot
    UnsupportedFormat   = 3,
    UnsupportedTransfer = 4,
    UnsupportedContent  = 5,   // e.g. SideB with no B loaded
    NoSource            = 6,   // nothing loaded in QCView yet
    Busy                = 7,   // the producer cannot take another output right now
};

// Header flags the producer sets at create.
enum OutputHeaderFlags : uint32_t {
    // The producer started the sink process itself (O13): such a sink quits
    // when its rings are retired; a sink the user started goes back to
    // waiting instead.
    kOutputLaunchedByProducer = 1u << 0,
};

// Per-frame flags.
enum OutputFrameFlags : uint32_t {
    kOutputFrameHasA          = 1u << 0,   // a source was present on side A
    kOutputFrameHasB          = 1u << 1,
    kOutputFrameHasInf        = 1u << 2,   // the canvas held non-finite values (RGBA16F only)
    kOutputFrameHasNaN        = 1u << 3,
    kOutputFrameTimecodeValid = 1u << 4,   // `timecode` carries a real source timecode (O8)
    kOutputFrameDropFrame     = 1u << 5,   // ...in drop-frame counting
    kOutputFramePaused        = 1u << 6,   // the producer's transport is paused (the frame is still new)
};

// Written by the sink. Zero fields mean "unspecified" and are refused
// except where a default is stated.
struct OutputRequest {
    uint32_t protocol_version;   // kOutputDescVersion of the sink's copy
    uint32_t width;              // raster, <= kOutputMaxWidth
    uint32_t height;
    uint32_t pixel_format;       // OutputPixelFormat
    uint32_t transfer;           // OutputTransfer
    uint32_t fit;                // OutputFit; 0 = Letterbox
    uint32_t content;            // OutputContent; 0 = Composite
    uint32_t layers;             // OutputLayers wanted; the producer may include fewer, never more
    uint32_t max_fps_num;        // the most frames per second the sink cares to receive; 0/0 = no cap
    uint32_t max_fps_den;
    uint32_t _pad0[2];
};
static_assert(sizeof(OutputRequest) == 48, "OutputRequest is a wire format — size change needs a version bump");

// Written by the producer in answer to a request.
struct OutputReply {
    uint32_t accepted;           // 1 = frames will follow in the shape below; 0 = see refusal
    uint32_t refusal;            // OutputRefusal
    uint32_t width;              // what will actually be produced (fit may change these)
    uint32_t height;
    uint32_t pixel_format;
    uint32_t transfer;
    uint32_t content;
    uint32_t layers;             // what will actually be included
    char     message[kOutputMessageBytes];   // human-readable reason, NUL-terminated
};
static_assert(sizeof(OutputReply) == 128, "OutputReply is a wire format — size change needs a version bump");

// Packed timecode: hours << 24 | minutes << 16 | seconds << 8 | frames.
inline constexpr uint32_t pack_timecode(uint32_t hh, uint32_t mm, uint32_t ss, uint32_t ff) {
    return (hh & 0xFFu) << 24 | (mm & 0xFFu) << 16 | (ss & 0xFFu) << 8 | (ff & 0xFFu);
}

// One per slot: how the pixels are laid out and when the frame is.
struct OutputFrameDesc {
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_row;      // output_aligned_bytes_per_row(width, format)
    uint32_t pixel_format;       // OutputPixelFormat
    uint32_t transfer;           // OutputTransfer the pixels are encoded for
    uint32_t content;            // OutputContent
    uint32_t layers;             // OutputLayers actually drawn into this frame
    uint32_t flags;              // OutputFrameFlags
    // When: the producer's steady clock (OutputRing::now_ns), the same one the
    // audio packets beside it are stamped with. Sinks pace themselves on it.
    int64_t  presentation_ns;
    // What: the source frame this canvas shows (side A's in dual), or -1 for
    // a live source with no index.
    int64_t  source_frame_index;
    uint32_t source_fps_num;     // 0/0 = unknown (live, or a still)
    uint32_t source_fps_den;
    uint32_t timecode;           // pack_timecode(...), valid when kOutputFrameTimecodeValid
    uint32_t _pad0;
};
static_assert(sizeof(OutputFrameDesc) == 64, "OutputFrameDesc is a wire format — size change needs a version bump");

}  // namespace qcbae
