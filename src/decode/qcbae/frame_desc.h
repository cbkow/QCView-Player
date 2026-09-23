// Vendored from QCBridgeAE (github.com/cbkow/QCBridgeAE, MIT) at 5057e96,
// src/common/protocol/frame_desc.h. The wire contract between the QCBridgeAE Transmit device
// and QCView. Keep in sync by re-copying, never by editing here:
// SharedRing::open() rejects a ring whose version != kFrameDescVersion, so
// a mismatch fails loudly instead of misreading frames.

// QCBridgeAE — the sidecar schema.
//
// The shared surface carries numbers; this struct carries how they are laid
// out (PLAN.md D5): dimensions, stride, format, channel order, value scale,
// flags. These are container facts — mechanical, always true, and a frame is
// visibly broken without them. Colour *meaning* is not here: QCView's user
// chooses the input transform. The ICC generation below is an optional hint
// from the AEGP tap, never authoritative; the Transmit device does not set it.
//
// This is a wire format shared across two processes and (later) two platforms
// and two compilers. Rules: POD only, fixed-width types, explicit padding,
// no pointers, no std:: containers, never reorder or resize a field — add at
// the end and bump kFrameDescVersion.

#pragma once

#include <cstdint>

namespace qcbae {

inline constexpr uint32_t kRingMagic        = 0x51434145u;  // 'QCAE'
inline constexpr uint32_t kFrameDescVersion = 3u;   // v2: channel_order; v3: non-finite flags replace the clamp flag, ring header gains host_state

inline constexpr uint32_t kMaxCompName = 128u;

// What the surface holds. One per AE tier (PLAN.md D1): the integer tiers
// travel native and the GPU's texture unit normalizes them for free, because
// converting them to half costs 4.5x (8bpc) and 1.2x (16bpc) for precision
// that is either already exact or actively worse. Only the float tier is
// converted, and only because QCView has no 32f flow (D2).
//
// All three sample as `texture2d<float>` through one shader and one pipeline —
// verified in tests/texture_format_test.mm, because that equivalence is what
// makes per-tier formats cost a switch rather than a code path.
enum class PixelFormat : uint32_t {
    Unknown     = 0,
    RGBA16F     = 1,   // 32 bpc stopped down.  MTLPixelFormatRGBA16Float
    RGBA8Unorm  = 2,   // AE 8 bpc,  native.    MTLPixelFormatRGBA8Unorm
    RGBA16Unorm = 3,   // AE 16 bpc, native.    MTLPixelFormatRGBA16Unorm
    RGBA32Float = 4,   // AE 32 bpc, native.    MTLPixelFormatRGBA32Float
};

// Which AE tier the pixels came FROM, before conversion to the wire format.
// Not redundant with PixelFormat: it tells QCView what precision is really
// present, so a 16bpc source can be reported honestly as ~11 effective bits
// near white rather than implied to be full half precision (PLAN.md D2).
enum class SourceTier : uint32_t {
    Unknown = 0,
    Int8    = 1,   // AE 8 bpc   — exact in half
    Int16   = 2,   // AE 16 bpc  — 0..32768, lossy above ~0.031 (PLAN.md D2/D3)
    Float32 = 3,   // AE 32 bpc  — IEEE to half; beyond ±65504 becomes ±inf (PLAN.md D4)
};

// After Effects stores pixels as ARGB, not RGBA — PF_Pixel8/16 lead with
// alpha and PF_PixelFloat is {alpha, red, green, blue}. Reading an AE world as
// RGBA rotates every channel.
//
// The producer states what it sent; a consumer honouring this field reorders.
// The AEGP tap and the A4 probe send AE's native order untouched. The
// Transmit device (A6) reorders to RGBA inside its one conversion pass,
// because QCView's upload path has no swizzle and the pass touches every
// pixel anyway (PLAN.md D1).
enum class ChannelOrder : uint32_t {
    Unknown = 0,
    RGBA    = 1,
    ARGB    = 2,   // After Effects native
    BGRA    = 3,   // several Premiere PrPixelFormats, for Route A later
};

enum FrameFlags : uint32_t {
    kFlagNone            = 0u,
    kFlagPremultiplied   = 1u << 0,  // alpha is premultiplied (AE's normal state)
    // Non-finite samples are carried, never clamped (PLAN.md D4); these say a
    // frame has some, so a consumer can show them rather than stumble on them.
    kFlagHasInf          = 1u << 1,  // at least one ±inf (source inf, or beyond ±65504 in half)
    kFlagHasNaN          = 1u << 2,  // at least one NaN
};

// GPU row-stride alignment. A linear texture over shared memory has a
// per-device minimum for bytes_per_row: Metal exposes it as
// minimumLinearTextureAlignmentForPixelFormat, D3D11 has an equivalent
// constraint. 256 satisfies every device either API reports, and padding a
// 4K RGBA16F row costs nothing (30720 is already a multiple of it).
//
// The producer owns this: it pads rows on the way in, and bytes_per_row says
// what it did. A consumer that ignores the field and computes width * 8 will
// shear every frame whose width isn't a multiple of 32 pixels.
inline constexpr uint32_t kRowAlignment = 256u;

inline constexpr uint32_t aligned_bytes_per_row(uint32_t width, uint32_t bytes_per_pixel) {
    const uint32_t tight = width * bytes_per_pixel;
    return (tight + kRowAlignment - 1u) / kRowAlignment * kRowAlignment;
}

inline constexpr uint32_t bytes_per_pixel(PixelFormat f) {
    switch (f) {
        case PixelFormat::RGBA8Unorm:  return 4u;
        case PixelFormat::RGBA16F:
        case PixelFormat::RGBA16Unorm: return 8u;
        case PixelFormat::RGBA32Float: return 16u;
        default:                       return 0u;
    }
}

// The wire format for a given AE tier.
//
// The integer tiers travel native, always: cheaper AND lossless, so there is
// nothing to trade (PLAN.md D1).
//
// 32 bpc converts to half by default, and the reason is not speed — native
// costs only ~8% CPU and 0.16 ms GPU at 4K. It is that **QCView quantizes to
// 16F at ingest today**, so a native wire would pay 2x the memory (380 MB at
// 4K for three slots, 1.5 GB at 8K) to carry bits that are discarded at the
// door. Measured both ways in lab/results/2026-09-20-a1c-wire-format/.
//
// `native_float32` keeps the lossless path built and tested, because the
// trade-off inverts the day QCView grows a 32f pipeline: at that point the
// bits stop being discarded and 2x memory buys 13 bits of significand. That is
// the trigger to flip this default — it is a flag, not a rewrite.
inline constexpr PixelFormat wire_format_for(SourceTier t, bool native_float32 = false) {
    switch (t) {
        case SourceTier::Int8:    return PixelFormat::RGBA8Unorm;
        case SourceTier::Int16:   return PixelFormat::RGBA16Unorm;
        case SourceTier::Float32: return native_float32 ? PixelFormat::RGBA32Float
                                                        : PixelFormat::RGBA16F;
        default:                  return PixelFormat::Unknown;
    }
}

inline constexpr uint64_t frame_bytes(uint32_t width, uint32_t height, PixelFormat f) {
    return static_cast<uint64_t>(aligned_bytes_per_row(width, bytes_per_pixel(f))) * height;
}

// Worst case across the DEFAULT formats (8 B/px; native 32f is opt-in and a
// ring using it must be sized with frame_bytes instead). Sizing a ring this
// way survives a mid-session bit-depth change without a rebuild, at the cost
// of giving an 8 bpc project twice the memory it needs. See PLAN.md A2.
inline constexpr uint64_t max_frame_bytes(uint32_t width, uint32_t height) {
    return static_cast<uint64_t>(aligned_bytes_per_row(width, 8u)) * height;
}

struct FrameDesc {
    uint32_t width;
    uint32_t height;
    uint32_t bytes_per_row;
    PixelFormat pixel_format;

    SourceTier source_tier;
    ChannelOrder channel_order;
    uint32_t flags;
    uint32_t _pad0;

    // AE comp time as an exact rational — never a float. A frame number is
    // not derivable from a double without rounding arguments nobody wins.
    int64_t  time_value;
    int64_t  time_scale;

    // From AEGP_ColorSettingsSuite6. The ICC blob itself lives in the ring's
    // profile region (it changes on project settings, not per frame); this
    // generation counter tells the consumer when its cached copy is stale.
    uint64_t icc_generation;
    float    graphics_white;   // nits; 0 = unspecified

    // Multiply what the GPU samples by this to recover the intended value.
    //
    // It exists because AE's 16 bpc white is 32768, not 65535: carried in an
    // RGBA16Unorm texture, hardware normalization lands on 0.50001 and the
    // image is half-bright — a silent transformation, the exact class of bug
    // this project exists to prevent (PLAN.md D3). 65535/32768 corrects it and
    // is exact in fp32.
    //
    // Carried per frame rather than inferred from the format, so the producer
    // states what it did instead of the consumer keeping a table of special
    // cases. 1.0 for every other tier. A consumer must apply it unconditionally
    // and must not special-case the format.
    float    value_scale;

    // Display label for the QCView media item. In memory only — never logged
    // (PLAN.md §Privacy 5).
    char     comp_name[kMaxCompName];
};

static_assert(sizeof(FrameDesc) == 192, "FrameDesc is a wire format — size change needs a version bump");

// AE 16 bpc: PF_MAX_CHAN16 is 32768, the unorm container is 65535.
inline constexpr float kAE16ValueScale = 65535.0f / 32768.0f;

}  // namespace qcbae
