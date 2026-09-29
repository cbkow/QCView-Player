// Metal OCIO LUT upload, shared by MetalOcioRenderer and MetalScopeRenderer.
//
// OCIO's emitted MSL function takes its LUT textures + samplers as
// arguments. gatherLuts() uploads one shader desc's LUTs as MTLTextures
// and appends (a) the kernel parameter declarations and (b) the OCIO
// call's argument list, in the order OCIO's signature requires. Every
// LUT is passed the caller's sampler (`samplerName`): OCIO's LUT samplers
// are all linear + clamp, and one shared sampler keeps kernels with
// several OCIO functions inside Metal's 16-sampler limit.

#pragma once

#include <QString>

#include <OpenColorIO/OpenColorIO.h>

#import <Metal/Metal.h>

#include <vector>

namespace qcv {

// `nextTex` is the next free [[texture(N)]] index; it continues across
// calls so several descs can share one kernel. Returns false (and sets
// `error`) if a LUT texture can't be created.
bool gatherLuts(id<MTLDevice> device,
                const OCIO_NAMESPACE::ConstGpuShaderDescRcPtr &desc,
                int &nextTex,
                std::vector<id<MTLTexture>> &luts,
                QString &kernelArgs,
                QString &callArgs,
                QString &error,
                const QString &samplerName = QStringLiteral("src_smp"));

} // namespace qcv
