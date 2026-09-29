// Colourspace names the scopes' tag rules look for (ScopeController),
// first match wins, aliases count. One list per file kind.
//
// CONFIG UPGRADE: the bundled configs change often. Every list must still
// resolve (directly or through an alias) in the scopes' fallback config
// (Blender 5.2 — ScopeController's kScopeFallbackConfigDir), and should in
// the other bundled configs; a renamed colourspace silently drops that
// file kind to the Signal tier. tools/probe-ocio-metal checks this ("names"
// lines) — run it after replacing any assets/OCIO config. Checklist:
// assets/OCIO/patches/README.md, "Names QCView depends on".

#pragma once

namespace qcv::scope_names {

inline constexpr const char *kExrLinear[] = {"Linear Rec.709 (sRGB)", "Linear Rec.709"};
inline constexpr const char *kStill[]     = {"sRGB - Display", "sRGB Encoded Rec.709 (sRGB)",
                                             "sRGB", "sRGB - Texture"};
inline constexpr const char *kPqP3[]      = {"ST2084-P3-D65 - Display", "ST2084-P3-D65"};
inline constexpr const char *kPq2020[]    = {"Rec.2100-PQ - Display", "Rec.2100-PQ"};
inline constexpr const char *kHlg[]       = {"Rec.2100-HLG - Display", "Rec.2100-HLG"};
inline constexpr const char *kSdrVideo[]  = {"Rec.1886 Rec.709 - Display", "Rec.1886"};

// For checks: every list with a label.
struct NameList {
    const char        *label;
    const char *const *names;
    int                count;
};
template <int N>
constexpr NameList list(const char *label, const char *const (&names)[N]) { return {label, names, N}; }
inline constexpr NameList kAll[] = {
    list("EXR (linear 709)", kExrLinear), list("stills (sRGB)", kStill),
    list("PQ P3-D65", kPqP3),             list("PQ Rec.2020", kPq2020),
    list("HLG", kHlg),                    list("SDR video (BT.1886)", kSdrVideo),
};

} // namespace qcv::scope_names
