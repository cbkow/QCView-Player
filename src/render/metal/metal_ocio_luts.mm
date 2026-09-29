// See metal_ocio_luts.h.

#include "metal_ocio_luts.h"

#include <algorithm>

namespace OCIO = OCIO_NAMESPACE;

namespace qcv {

// Upload one shader desc's LUTs, append their kernel parameters and the
// OCIO call's arguments. `nextTex` continues across descs.
bool gatherLuts(id<MTLDevice> device, const OCIO::ConstGpuShaderDescRcPtr &desc,
                int &nextTex, std::vector<id<MTLTexture>> &luts,
                QString &kernelArgs, QString &callArgs, QString &error,
                const QString &samplerName)
{
    // OCIO's emitted function takes its texture + sampler arguments in a
    // specific order: ALL 3D LUTs first (registration order), then ALL
    // 1D/2D LUTs, then float4 inPixel. The trailing digits of OCIO's
    // texName ("ocio_lut3d_2" → 2) give the registration index. Our
    // [[texture(N)]] order can be arbitrary — Metal resolves by name —
    // but the call expression MUST follow OCIO's order.
    struct CallArg { int regIndex; QString tname; };
    std::vector<CallArg> call3D, call2D;
    auto parseTrailingInt = [](const QString &s) -> int {
        const int us = s.lastIndexOf(QLatin1Char('_'));
        if (us < 0) return 0;
        bool ok = false;
        const int n = s.mid(us + 1).toInt(&ok);
        return ok ? n : 0;
    };

    // --- 1D + 2D LUTs ---
    for (unsigned i = 0; i < desc->getNumTextures(); ++i) {
        const char *texName = nullptr, *smpName = nullptr;
        unsigned w = 0, h = 0;
        OCIO::GpuShaderDesc::TextureType chan = OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL;
        OCIO::GpuShaderDesc::TextureDimensions dim = OCIO::GpuShaderDesc::TEXTURE_1D;
        OCIO::Interpolation interp = OCIO::INTERP_DEFAULT;
        desc->getTexture(i, texName, smpName, w, h, chan, dim, interp);
        if (!texName || !*texName) continue;

        const QString tname = QString::fromUtf8(texName);
        const bool isRgb = (chan == OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL);
        // OCIO emits real `texture1d<float>` parameters for 1D LUTs; the
        // MTLTexture type must match or OCIODisplay() won't resolve.
        const bool is1D = (dim == OCIO::GpuShaderDesc::TEXTURE_1D);

        const float *values = nullptr;
        desc->getTextureValues(i, values);
        if (!values) continue;

        const int width  = static_cast<int>(w > 0 ? w : 1);
        const int height = is1D ? 1 : static_cast<int>(h > 0 ? h : 1);

        // Pack to RGBA32Float if RGB (Metal has no RGB32F).
        MTLTextureDescriptor *td = [MTLTextureDescriptor new];
        td.pixelFormat = isRgb ? MTLPixelFormatRGBA32Float : MTLPixelFormatR32Float;
        td.width       = width;
        td.height      = height;
        td.depth       = 1;
        td.textureType = is1D ? MTLTextureType1D : MTLTextureType2D;
        td.usage       = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [device newTextureWithDescriptor:td];
        if (!tex) {
            error = QStringLiteral("MetalOcioRenderer: LUT texture create failed");
            return false;
        }

        const int n = width * height;
        const MTLRegion region = is1D ? MTLRegionMake1D(0, width)
                                      : MTLRegionMake2D(0, 0, width, height);
        const NSUInteger bpr = is1D
            ? 0
            : static_cast<NSUInteger>(width) * (isRgb ? 4u : 1u) * sizeof(float);
        if (isRgb) {
            std::vector<float> rgba(static_cast<std::size_t>(n) * 4);
            for (int p = 0; p < n; ++p) {
                rgba[p * 4 + 0] = values[p * 3 + 0];
                rgba[p * 4 + 1] = values[p * 3 + 1];
                rgba[p * 4 + 2] = values[p * 3 + 2];
                rgba[p * 4 + 3] = 1.0f;
            }
            [tex replaceRegion:region mipmapLevel:0 withBytes:rgba.data() bytesPerRow:bpr];
        } else {
            [tex replaceRegion:region mipmapLevel:0 withBytes:values bytesPerRow:bpr];
        }
        luts.push_back(tex);

        kernelArgs += QStringLiteral("    %3<float, access::sample> %1 [[texture(%2)]],\n")
            .arg(tname).arg(nextTex++)
            .arg(QString::fromUtf8(is1D ? "texture1d" : "texture2d"));
        call2D.push_back({ parseTrailingInt(tname), tname });
    }

    // --- 3D LUTs ---
    for (unsigned i = 0; i < desc->getNum3DTextures(); ++i) {
        const char *texName = nullptr, *smpName = nullptr;
        unsigned edgeLen = 0;
        OCIO::Interpolation interp = OCIO::INTERP_DEFAULT;
        desc->get3DTexture(i, texName, smpName, edgeLen, interp);
        if (!texName || !*texName) continue;
        const float *values = nullptr;
        desc->get3DTextureValues(i, values);
        if (!values || edgeLen == 0) continue;

        const int E = static_cast<int>(edgeLen);
        MTLTextureDescriptor *td = [MTLTextureDescriptor new];
        td.textureType = MTLTextureType3D;
        td.pixelFormat = MTLPixelFormatRGBA32Float;
        td.width  = E;
        td.height = E;
        td.depth  = E;
        td.usage       = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        id<MTLTexture> tex = [device newTextureWithDescriptor:td];
        if (!tex) {
            error = QStringLiteral("MetalOcioRenderer: 3D LUT texture create failed");
            return false;
        }
        // Pack RGB → RGBA per slice.
        const int sliceTexels = E * E;
        std::vector<float> slice(static_cast<std::size_t>(sliceTexels) * 4);
        for (int z = 0; z < E; ++z) {
            const float *srcV = values + z * sliceTexels * 3;
            for (int p = 0; p < sliceTexels; ++p) {
                slice[p * 4 + 0] = srcV[p * 3 + 0];
                slice[p * 4 + 1] = srcV[p * 3 + 1];
                slice[p * 4 + 2] = srcV[p * 3 + 2];
                slice[p * 4 + 3] = 1.0f;
            }
            [tex replaceRegion:MTLRegionMake3D(0, 0, z, E, E, 1)
                   mipmapLevel:0
                         slice:0
                     withBytes:slice.data()
                   bytesPerRow:E * 4 * sizeof(float)
                 bytesPerImage:0];
        }
        luts.push_back(tex);

        const QString tname = QString::fromUtf8(texName);
        kernelArgs += QStringLiteral("    texture3d<float, access::sample> %1 [[texture(%2)]],\n")
            .arg(tname).arg(nextTex++);
        call3D.push_back({ parseTrailingInt(tname), tname });
    }

    auto byRegIndex = [](const CallArg &a, const CallArg &b) { return a.regIndex < b.regIndex; };
    std::stable_sort(call3D.begin(), call3D.end(), byRegIndex);
    std::stable_sort(call2D.begin(), call2D.end(), byRegIndex);
    for (const auto &c : call3D) callArgs += QStringLiteral("%1, %2, ").arg(c.tname, samplerName);
    for (const auto &c : call2D) callArgs += QStringLiteral("%1, %2, ").arg(c.tname, samplerName);
    return true;
}

} // namespace qcv
