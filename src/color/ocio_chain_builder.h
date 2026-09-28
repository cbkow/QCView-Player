// OcioChainBuilder — Phase 7.5 A.2.7.
//
// CPU-side OCIO group-transform construction. Given an
// OCIOConfigManager (which holds the active Input / Look / Scene-LUT
// / Display+View / Display-LUT chain), produces:
//   - A baked OCIO::GpuShaderDescRcPtr (LUT inventory + emitted GLSL
//     function, function name "OCIODisplay")
//   - The raw GLSL function body string
//   - A diagnostic message on failure
//
// This stops short of any GPU API: no QRhi, no QShaderBaker, no
// SPIR-V, no LUT upload. Per-platform renderers consume the returned
// `OcioChain` and finish the job:
//   - QRhi (UI window): src/render/ocio_shader_builder.cpp uses this
//     to drive QShaderBaker → QShader (GLSL_4_0), then uploads LUTs
//     as QRhiTexture
//   - Phase B (native Metal player): asks for MSL_2_0 (OCIO emits
//     MSL directly), wraps in a compute kernel, uploads LUTs as
//     MTLTexture
//   - Phase C (native Vulkan player): asks for GLSL_VK_4_6, runs
//     through shaderc to SPIR-V, uploads LUTs as VkImage
//
// The chain logic is preserved verbatim from old QCView's
// src/color/ocio_pipeline.cpp:179-262 (Look → Scene LUT →
// DisplayView → Display LUT, per Guide 05 D7) and the existing
// OcioShaderBuilder build() body — both shared a copy. Single source
// going forward.

#pragma once

#include <QString>

#include <OpenColorIO/OpenColorIO.h>

#include "linear_stage.h"

namespace qcv {

class OCIOConfigManager;

// Replaces the active Display/View (and drops the Display LUT, which is
// calibrated for the live display). Used for SDR captures — see
// OCIOConfigManager::sdrCaptureDisplayView.
struct DisplayViewOverride {
    QString display;
    QString view;
};

struct OcioChain {
    OCIO_NAMESPACE::ConstGpuShaderDescRcPtr desc;
    QString shaderText;    // OCIO's emitted shader function (language depends on call)
    QString errorMessage;  // populated when ok == false
    bool    ok = false;
    // The Display/View's output, for the viewer aids (build() only).
    OutputEncoding encoding = OutputEncoding::Sdr;
    int            outputPrimaries = 0;   // 0 = Rec.709, 1 = P3, 2 = Rec.2020
};

// The chain split around the linear stage (see linear_stage.h):
//   pre  = Input → Look → Scene LUT → interchange role   ("OCIOPre")
//   post = interchange role → Display/View → Display LUT ("OCIOPost")
// The interchange role is on the same reference-space side as the
// colourspace that enters the Display/View (the Look's result, else the
// Input), so the View sees the same kind of source as in the unsplit
// chain and the round trip is exact up to float precision.
struct OcioSplitChain {
    OcioChain       pre;
    OcioChain       post;
    InterchangeSide side = InterchangeSide::None;
    bool            displayIsSdr = true;   // the Display/View's colourspace encodes SDR
    OutputEncoding  encoding = OutputEncoding::Sdr;
    int             outputPrimaries = 0;
    QString         errorMessage;
    bool            ok = false;
};

// CPU form of the same split, for LUT baking.
struct OcioSplitTransforms {
    OCIO_NAMESPACE::GroupTransformRcPtr pre;
    OCIO_NAMESPACE::GroupTransformRcPtr post;
    InterchangeSide side = InterchangeSide::None;
    bool            displayIsSdr = true;
    OutputEncoding  encoding = OutputEncoding::Sdr;
    int             outputPrimaries = 0;
};

class OcioChainBuilder {
public:
    // Shader language for the emitted OCIO function.
    enum class Language {
        Glsl_4_0,    // QRhi path (QShaderBaker consumes; QShaderBaker also re-emits MSL)
        Msl_2_0,     // Native Metal path (compile via newLibraryWithSource: directly)
        GlslVk_4_6,  // Native Vulkan path (shaderc → SPIR-V)
        Hlsl_Sm_5_0, // Native D3D11 path (compile via D3DCompile)  — Phase F.2.5
    };

    // Build the OCIO chain for `ocio`'s currently active state. Pure
    // OCIO — no GPU API touched. Safe to call from any thread that
    // already serializes access to `ocio` (the caller is responsible
    // for that; OCIOConfigManager itself is thread-safe to read).
    static OcioChain build(OCIOConfigManager *ocio,
                           Language language = Language::Glsl_4_0,
                           const DisplayViewOverride *override = nullptr);

    // Construct just the OCIO::GroupTransform that mirrors the live
    // render chain (Look → Scene LUT → DisplayView → Display LUT).
    // Used by build() for the GPU-shader path and by the LUT-export
    // path (OCIOConfigManager::exportLut → OcioLutBaker) to keep
    // chain shape in one place. Returns nullptr on error (chain
    // incomplete, OCIO exception, etc.). If `errorOut` is non-null,
    // populates it with a human-readable diagnostic on failure.
    static OCIO_NAMESPACE::GroupTransformRcPtr buildGroupTransform(
        OCIOConfigManager *ocio,
        OCIO_NAMESPACE::ConstConfigRcPtr cfg,
        QString *errorOut = nullptr,
        const DisplayViewOverride *override = nullptr);

    // Split build (GPU): two shader functions, OCIOPre (prefix
    // "ocio_pre_") and OCIOPost ("ocio_post_"), for one shader with the
    // linear stage between them. Fails (ok = false) when the config
    // lacks the interchange role for the side, or the colourspace
    // entering the View is a data space — callers then fall back to the
    // unsplit chain and the stage is unavailable.
    static OcioSplitChain buildSplit(OCIOConfigManager *ocio,
                                     Language language,
                                     const DisplayViewOverride *override = nullptr);

    // Split build (CPU transforms), same shape as buildSplit().
    static bool buildSplitTransforms(OCIOConfigManager *ocio,
                                     OCIO_NAMESPACE::ConstConfigRcPtr cfg,
                                     OcioSplitTransforms &out,
                                     QString *errorOut = nullptr,
                                     const DisplayViewOverride *override = nullptr);
};

} // namespace qcv
