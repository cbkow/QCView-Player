// See d3d11_ocio_luts.h.

#include "d3d11_ocio_luts.h"

#include <d3d11shader.h>
#include <d3dcompiler.h>

#include <QtLogging>

#include <cstring>

using Microsoft::WRL::ComPtr;
namespace OCIO = OCIO_NAMESPACE;

namespace qcv {

ComPtr<ID3DBlob> compileHlsl(const std::string &source,
                                const char *entry, const char *target,
                                QString *errorOut, unsigned extraFlags)
{
    ComPtr<ID3DBlob> code, errors;
    // LEVEL_1 trades runtime PS speed for compile speed. OCIO chains
    // are straight-line math per pixel — little for the optimizer to
    // win — and we pay compile cost on every chain change. LEVEL_3 is
    // multi-second on large chains; LEVEL_1 is sub-second. SKIP was
    // tried and broke OCIO output (suspected register-packing diff
    // with large chains), so LEVEL_1 is the conservative pick.
    const HRESULT hr = D3DCompile(
        source.data(), source.size(),
        entry, nullptr, nullptr,
        entry, target,
        D3DCOMPILE_OPTIMIZATION_LEVEL1 | extraFlags, 0,
        code.GetAddressOf(), errors.GetAddressOf());
    if (FAILED(hr)) {
        QString msg = QStringLiteral("D3DCompile %1 failed (hr=0x%2)")
            .arg(QString::fromUtf8(entry))
            .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
        if (errors) {
            msg += QStringLiteral(" — ") +
                   QString::fromUtf8(static_cast<const char *>(errors->GetBufferPointer()));
        }
        if (errorOut) *errorOut = msg;
        return nullptr;
    }
    return code;
}


ComPtr<ID3D11SamplerState> makeLinearClampSampler(ID3D11Device *device)
{
    D3D11_SAMPLER_DESC ss{};
    ss.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    ss.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    ss.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    ss.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ss.MaxLOD   = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> smp;
    device->CreateSamplerState(&ss, smp.GetAddressOf());
    return smp;
}

// Upload one shader desc's LUTs (3D first, then 1D/2D — OCIO's HLSL
// declaration order) and append them to `newLuts`. Slots are resolved
// later by reflection, by name, so several descs can share one shader.
bool createLuts(ID3D11Device *device, const OCIO::ConstGpuShaderDescRcPtr &desc,
                std::vector<LutResource> &newLuts, QString &error)
{
    const int n3d = desc->getNum3DTextures();
    const int n1d = desc->getNumTextures();

    newLuts.reserve(newLuts.size() + static_cast<std::size_t>(n3d + n1d));

    // --- 3D LUTs (declared first by OCIO HLSL emit) ---
    for (int i = 0; i < n3d; ++i) {
        const char *texName = nullptr, *smpName = nullptr;
        unsigned edge = 0;
        OCIO::Interpolation interp = OCIO::INTERP_DEFAULT;
        desc->get3DTexture(static_cast<unsigned>(i), texName, smpName, edge, interp);
        const float *values = nullptr;
        desc->get3DTextureValues(static_cast<unsigned>(i), values);
        if (!values || edge == 0) continue;

        // D3D11 has no RGB32_FLOAT 3D — unpack RGB → RGBA per voxel.
        const std::size_t voxels = static_cast<std::size_t>(edge) * edge * edge;
        std::vector<float> rgba(voxels * 4);
        for (std::size_t v = 0; v < voxels; ++v) {
            rgba[v*4 + 0] = values[v*3 + 0];
            rgba[v*4 + 1] = values[v*3 + 1];
            rgba[v*4 + 2] = values[v*3 + 2];
            rgba[v*4 + 3] = 1.0f;
        }

        D3D11_TEXTURE3D_DESC td{};
        td.Width     = edge;
        td.Height    = edge;
        td.Depth     = edge;
        td.MipLevels = 1;
        td.Format    = DXGI_FORMAT_R32G32B32A32_FLOAT;
        td.Usage     = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA init{};
        init.pSysMem          = rgba.data();
        init.SysMemPitch      = edge * 4 * sizeof(float);
        init.SysMemSlicePitch = edge * edge * 4 * sizeof(float);
        ComPtr<ID3D11Texture3D> tex;
        if (FAILED(device->CreateTexture3D(&td, &init, tex.GetAddressOf()))) {
            error = QStringLiteral("D3D11OcioRenderer: CreateTexture3D LUT3D failed");
            return false;
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format        = td.Format;
        sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE3D;
        sd.Texture3D.MipLevels = 1;
        LutResource r;
        r.is3d    = true;
        r.texName = texName ? texName : "";
        r.smpName = smpName ? smpName : "";
        if (FAILED(device->CreateShaderResourceView(tex.Get(), &sd,
                                                              r.srv.GetAddressOf()))) {
            error = QStringLiteral("D3D11OcioRenderer: SRV LUT3D failed");
            return false;
        }
        r.sampler = makeLinearClampSampler(device);
        newLuts.push_back(std::move(r));
    }

    // --- 1D/2D LUTs ---
    // OCIO 2.x HLSL emits real `Texture1D` declarations for 1D LUTs
    // (height == 1) — not `Texture2D` as Nx1. Binding a Texture2D
    // SRV at a slot the HLSL declares as Texture1D is a resource-
    // type mismatch: D3D11 silently returns zeros instead of
    // sampling — which renders OCIO transforms as no-op / wrong
    // colors without a validation error. We branch here on OCIO's
    // reported dim so the SRV type matches the HLSL declaration.
    // Mirrors the Metal renderer's `is1D ? MTLTextureType1D :
    // MTLTextureType2D` branch.
    for (int i = 0; i < n1d; ++i) {
        const char *texName = nullptr, *smpName = nullptr;
        unsigned w = 0, h = 0;
        OCIO::GpuShaderDesc::TextureType chan =
            OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL;
        OCIO::GpuShaderDesc::TextureDimensions dim =
            OCIO::GpuShaderDesc::TEXTURE_1D;
        OCIO::Interpolation interp = OCIO::INTERP_DEFAULT;
        desc->getTexture(static_cast<unsigned>(i), texName, smpName, w, h, chan, dim, interp);
        const float *values = nullptr;
        desc->getTextureValues(static_cast<unsigned>(i), values);
        if (!values || w == 0) continue;

        const bool is1D  = (dim == OCIO::GpuShaderDesc::TEXTURE_1D);
        const bool isRgb = (chan == OCIO::GpuShaderDesc::TEXTURE_RGB_CHANNEL);
        const DXGI_FORMAT fmt = isRgb ? DXGI_FORMAT_R32G32B32_FLOAT
                                       : DXGI_FORMAT_R32_FLOAT;
        const int channels = isRgb ? 3 : 1;

        ComPtr<ID3D11Resource> tex;
        D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = fmt;
        if (is1D) {
            D3D11_TEXTURE1D_DESC td{};
            td.Width     = w;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format    = fmt;
            td.Usage     = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{};
            init.pSysMem = values;
            // SysMemPitch is ignored for Texture1D, but we set it for
            // clarity and to keep it correct if this layout is reused
            // elsewhere.
            init.SysMemPitch = w * channels * sizeof(float);
            ComPtr<ID3D11Texture1D> t1d;
            if (FAILED(device->CreateTexture1D(&td, &init, t1d.GetAddressOf()))) {
                error = QStringLiteral("D3D11OcioRenderer: CreateTexture1D LUT1D failed");
                return false;
            }
            sd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE1D;
            sd.Texture1D.MipLevels = 1;
            tex = t1d;
        } else {
            const unsigned height = h > 0 ? h : 1;
            D3D11_TEXTURE2D_DESC td{};
            td.Width     = w;
            td.Height    = height;
            td.MipLevels = 1;
            td.ArraySize = 1;
            td.Format    = fmt;
            td.SampleDesc.Count = 1;
            td.Usage     = D3D11_USAGE_IMMUTABLE;
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{};
            init.pSysMem     = values;
            init.SysMemPitch = w * channels * sizeof(float);
            ComPtr<ID3D11Texture2D> t2d;
            if (FAILED(device->CreateTexture2D(&td, &init, t2d.GetAddressOf()))) {
                error = QStringLiteral("D3D11OcioRenderer: CreateTexture2D LUT2D failed");
                return false;
            }
            sd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
            sd.Texture2D.MipLevels = 1;
            tex = t2d;
        }

        LutResource r;
        r.is3d     = false;
        r.dimLabel = is1D ? "1D" : "2D";
        r.texName  = texName ? texName : "";
        r.smpName  = smpName ? smpName : "";
        if (FAILED(device->CreateShaderResourceView(tex.Get(), &sd,
                                                              r.srv.GetAddressOf()))) {
            error = QStringLiteral(
                "D3D11OcioRenderer: SRV %1 failed").arg(is1D ? "1D" : "2D");
            return false;
        }
        r.sampler = makeLinearClampSampler(device);
        newLuts.push_back(std::move(r));
    }

    return true;
}


void resolveLutSlots(ID3DBlob *shaderBlob, std::vector<LutResource> &luts,
                     int &byName, int &byFallback)
{
    byName = byFallback = 0;
    // Reflect the compiled shader to find each LUT's bind slot by name —
    // matches the Metal pattern (function parameter names) rather than
    // relying on declaration order. A LUT not found by name (an emitter
    // can rename or drop unused LUTs) falls back to declaration order.
    // __uuidof avoids needing dxguid.lib at link time.
    ComPtr<ID3D11ShaderReflection> refl;
    D3DReflect(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(),
               __uuidof(ID3D11ShaderReflection),
               reinterpret_cast<void **>(refl.GetAddressOf()));
    int iterSlot = 1;   // t0 = src, LUTs at t1+ in OCIO's declaration order
    for (auto &lut : luts) {
        D3D11_SHADER_INPUT_BIND_DESC bd{};
        if (refl && SUCCEEDED(refl->GetResourceBindingDescByName(lut.texName.c_str(), &bd))) {
            lut.texSlot = static_cast<int>(bd.BindPoint);
            ++byName;
        } else {
            lut.texSlot = iterSlot;
            ++byFallback;
        }
        if (refl && SUCCEEDED(refl->GetResourceBindingDescByName(lut.smpName.c_str(), &bd))) {
            lut.smpSlot = static_cast<int>(bd.BindPoint);
        } else {
            lut.smpSlot = lut.texSlot;  // matched-pair convention
        }
        ++iterSlot;
    }
}

} // namespace qcv
