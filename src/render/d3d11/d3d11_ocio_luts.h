// D3D11 OCIO resource helpers, shared by D3D11OcioRenderer and
// D3D11ScopeRenderer: HLSL compile, LUT upload (one OCIO shader desc at a
// time — several descs can share one shader, their resource prefixes keep
// the names apart) and bind-slot resolution by shader reflection.

#pragma once

#include <QString>

#include <OpenColorIO/OpenColorIO.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcommon.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace qcv {

struct LutResource {
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    Microsoft::WRL::ComPtr<ID3D11SamplerState>       sampler;
    std::string texName;                 // OCIO's emitted Texture* name
    std::string smpName;                 // OCIO's emitted SamplerState name
    bool        is3d = false;
    const char *dimLabel = "1D";         // "1D" / "2D" (logging)
    int         texSlot = -1;            // from resolveLutSlots()
    int         smpSlot = -1;
};

// `extraFlags` ORs into the D3DCOMPILE flags (OPTIMIZATION_LEVEL1 is
// always set): the minColor kernel adds IEEE_STRICTNESS.
Microsoft::WRL::ComPtr<ID3DBlob> compileHlsl(const std::string &source,
                                             const char *entry, const char *target,
                                             QString *errorOut, unsigned extraFlags = 0);

Microsoft::WRL::ComPtr<ID3D11SamplerState> makeLinearClampSampler(ID3D11Device *device);

// Upload one desc's LUTs (3D first, then 1D/2D) and append to `newLuts`.
bool createLuts(ID3D11Device *device, const OCIO_NAMESPACE::ConstGpuShaderDescRcPtr &desc,
                std::vector<LutResource> &newLuts, QString &error);

// Resolve each LUT's texture / sampler slot in the compiled shader by
// name (falls back to declaration order from t1).
void resolveLutSlots(ID3DBlob *shaderBlob, std::vector<LutResource> &luts,
                     int &byName, int &byFallback);

} // namespace qcv
