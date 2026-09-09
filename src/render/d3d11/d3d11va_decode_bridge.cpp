#include "d3d11va_decode_bridge.h"

#if defined(Q_OS_WIN)

#include <QtLogging>

#include <cstring>
#include <unordered_map>

// D3D11 headers before the FFmpeg block (hwcontext_d3d11va.h includes
// <d3d11.h>; inside extern "C" its operator overloads fail to compile).
#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/pixdesc.h>
}

#include "d3d11_device_manager.h"
#include "decode/frame_handle.h"

using Microsoft::WRL::ComPtr;

namespace qcv {

namespace {

// Mirrors the GLSL in d3d11_vulkan_yuv_compositor.cpp (same constants,
// same range handling) so a clip looks identical whichever decoder
// produced it. Both planes are sampled as 0..1 UNORM: for P010 the
// 10-bit sample sits in the top bits, so UNORM16 ≈ v/1023 already.
constexpr const char *kCsHlsl = R"(
Texture2DArray<float>  lumaTex   : register(t0);
Texture2DArray<float2> chromaTex : register(t1);
SamplerState           samp      : register(s0);
RWTexture2D<float4>    outImg    : register(u0);

cbuffer Params : register(b0)
{
    uint width;        // picture (output) size
    uint height;
    uint colorSpace;   // 0 = BT.601, 1 = BT.709, 2 = BT.2020 NCL
    uint range;        // 0 = limited, 1 = full
    uint texWidth;     // decoder texture-array size (coded size: 1088 /
    uint texHeight;    //  1152 rows for 1080p) — normalize against THIS,
    uint pad0, pad1;   //  or the padding rows stretch the picture
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(texWidth, texHeight);
    float  y  = lumaTex.SampleLevel(samp, float3(uv, 0.0), 0).r;
    float2 c  = chromaTex.SampleLevel(samp, float3(uv, 0.0), 0).rg;
    float  u  = c.x, v = c.y;
    if (range == 0) {
        y = (y - 16.0/255.0) * (255.0/219.0);
        u = (u - 128.0/255.0) * (255.0/224.0);
        v = (v - 128.0/255.0) * (255.0/224.0);
    } else {
        u -= 0.5; v -= 0.5;
    }
    float r, g, b;
    if (colorSpace == 1) {
        r = y + 1.5748   * v;
        g = y - 0.1873   * u - 0.4681  * v;
        b = y + 1.8556   * u;
    } else if (colorSpace == 2) {
        r = y + 1.4746   * v;
        g = y - 0.16455  * u - 0.57135 * v;
        b = y + 1.8814   * u;
    } else {
        r = y + 1.402    * v;
        g = y - 0.34414  * u - 0.71414 * v;
        b = y + 1.772    * u;
    }
    outImg[id.xy] = float4(r, g, b, 1.0);
}
)";

struct ParamsCb {
    uint32_t width, height, colorSpace, range;
    uint32_t texWidth, texHeight, pad0, pad1;
};

struct SliceViews {
    ComPtr<ID3D11ShaderResourceView> luma;
    ComPtr<ID3D11ShaderResourceView> chroma;
};

} // namespace

struct D3D11VaDecodeBridge::Impl {
    bool initialized = false;

    ComPtr<ID3D11ComputeShader> cs;
    ComPtr<ID3D11Buffer>        cbuf;
    ComPtr<ID3D11SamplerState>  sampler;

    // Output — recreated on size change only.
    ComPtr<ID3D11Texture2D>           outTex;
    ComPtr<ID3D11ShaderResourceView>  outSrv;
    ComPtr<ID3D11UnorderedAccessView> outUav;
    int outW = 0, outH = 0;

    // Per-slice views, keyed by (array texture, slice). A new pool
    // (different texture pointer) invalidates the whole map.
    ID3D11Texture2D *cachedArray = nullptr;
    std::unordered_map<unsigned, SliceViews> views;

    D3D11VulkanDecodeBridge::ImportedFrame frame{};
    uint64_t sequence = 0;
    bool loggedFirst  = false;

    bool ensureOutput(ID3D11Device *device, int w, int h)
    {
        if (outTex && outW == w && outH == h) return true;
        outUav.Reset(); outSrv.Reset(); outTex.Reset();
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        if (FAILED(device->CreateTexture2D(&td, nullptr, outTex.GetAddressOf()))) {
            qWarning("D3D11VaDecodeBridge: RGBA16F output %dx%d create failed", w, h);
            return false;
        }
        if (FAILED(device->CreateShaderResourceView(outTex.Get(), nullptr, outSrv.GetAddressOf()))
            || FAILED(device->CreateUnorderedAccessView(outTex.Get(), nullptr, outUav.GetAddressOf()))) {
            qWarning("D3D11VaDecodeBridge: output view create failed");
            outUav.Reset(); outSrv.Reset(); outTex.Reset();
            return false;
        }
        outW = w; outH = h;
        qInfo("D3D11VaDecodeBridge: RGBA16F output allocated %dx%d", w, h);
        return true;
    }

    const SliceViews *sliceViews(ID3D11Device *device, ID3D11Texture2D *array,
                                 unsigned slice, bool tenBit)
    {
        if (array != cachedArray) {
            views.clear();
            cachedArray = array;
            loggedFirst = false;   // re-announce format/size for the new pool
            qInfo("D3D11VaDecodeBridge: new decoder texture array %p — view cache reset",
                  static_cast<void *>(array));
        }
        auto it = views.find(slice);
        if (it != views.end()) return &it->second;

        SliceViews sv;
        D3D11_SHADER_RESOURCE_VIEW_DESC d{};
        d.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        d.Texture2DArray.MostDetailedMip = 0;
        d.Texture2DArray.MipLevels = 1;
        d.Texture2DArray.FirstArraySlice = slice;
        d.Texture2DArray.ArraySize = 1;
        d.Format = tenBit ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
        if (FAILED(device->CreateShaderResourceView(array, &d, sv.luma.GetAddressOf()))) {
            qWarning("D3D11VaDecodeBridge: luma SRV (slice %u) failed", slice);
            return nullptr;
        }
        d.Format = tenBit ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
        if (FAILED(device->CreateShaderResourceView(array, &d, sv.chroma.GetAddressOf()))) {
            qWarning("D3D11VaDecodeBridge: chroma SRV (slice %u) failed", slice);
            return nullptr;
        }
        return &(views[slice] = std::move(sv));
    }
};

D3D11VaDecodeBridge::D3D11VaDecodeBridge() : m_impl(std::make_unique<Impl>()) {}
D3D11VaDecodeBridge::~D3D11VaDecodeBridge() { shutdown(); }

bool D3D11VaDecodeBridge::isInitialized() const { return m_impl->initialized; }

bool D3D11VaDecodeBridge::initialize()
{
    if (m_impl->initialized) return true;
    auto *device = static_cast<ID3D11Device *>(D3D11DeviceManager::instance().device());
    if (!device) return false;

    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3DCompile(kCsHlsl, std::strlen(kCsHlsl), "d3d11va_yuv2rgb", nullptr,
                            nullptr, "main", "cs_5_0", 0, 0,
                            blob.GetAddressOf(), err.GetAddressOf());
    if (FAILED(hr)) {
        qCritical("D3D11VaDecodeBridge: compute shader compile failed (hr=0x%08lX)\n%s",
                  static_cast<unsigned long>(hr),
                  err ? static_cast<const char *>(err->GetBufferPointer()) : "");
        return false;
    }
    if (FAILED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                           nullptr, m_impl->cs.GetAddressOf()))) {
        qCritical("D3D11VaDecodeBridge: CreateComputeShader failed");
        return false;
    }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(ParamsCb);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&bd, nullptr, m_impl->cbuf.GetAddressOf()))) {
        qCritical("D3D11VaDecodeBridge: constant buffer create failed");
        return false;
    }
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(device->CreateSamplerState(&sd, m_impl->sampler.GetAddressOf()))) {
        qCritical("D3D11VaDecodeBridge: sampler create failed");
        return false;
    }
    m_impl->initialized = true;
    qInfo("D3D11VaDecodeBridge: initialized (NV12 / P010 texture-array slices → RGBA16F compute)");
    return true;
}

void D3D11VaDecodeBridge::shutdown()
{
    auto &i = *m_impl;
    i.views.clear();
    i.cachedArray = nullptr;
    i.outUav.Reset(); i.outSrv.Reset(); i.outTex.Reset();
    i.outW = i.outH = 0;
    i.sampler.Reset(); i.cbuf.Reset(); i.cs.Reset();
    i.frame = {};
    i.initialized = false;
}

const D3D11VulkanDecodeBridge::ImportedFrame *
D3D11VaDecodeBridge::consume(const FrameHandle &fh, int rangeOverride)
{
    auto &i = *m_impl;
    if (!i.initialized) return nullptr;
    if (fh.kind() != FrameHandle::Kind::D3D11) return nullptr;
    const AVFrame *avFrame = fh.d3d11AvFrame();
    if (!avFrame || avFrame->format != AV_PIX_FMT_D3D11 || !avFrame->hw_frames_ctx) return nullptr;

    auto *array = reinterpret_cast<ID3D11Texture2D *>(avFrame->data[0]);
    const unsigned slice = static_cast<unsigned>(reinterpret_cast<intptr_t>(avFrame->data[1]));
    const auto *fc = reinterpret_cast<const AVHWFramesContext *>(avFrame->hw_frames_ctx->data);
    if (!array || !fc) return nullptr;
    const bool tenBit = (fc->sw_format == AV_PIX_FMT_P010);
    if (fc->sw_format != AV_PIX_FMT_NV12 && !tenBit) return nullptr;

    auto *device = static_cast<ID3D11Device *>(D3D11DeviceManager::instance().device());
    auto *ctx    = static_cast<ID3D11DeviceContext *>(D3D11DeviceManager::instance().context());
    if (!device || !ctx) return nullptr;

    const int w = avFrame->width, h = avFrame->height;
    if (!i.ensureOutput(device, w, h)) return nullptr;
    const SliceViews *sv = i.sliceViews(device, array, slice, tenBit);
    if (!sv) return nullptr;

    ParamsCb pc{};
    pc.width  = static_cast<uint32_t>(w);
    pc.height = static_cast<uint32_t>(h);
    {
        // The pool is allocated at coded size (FFmpeg aligns to the
        // codec's macroblock / CTB); sampling must be normalized to it.
        D3D11_TEXTURE2D_DESC td{};
        array->GetDesc(&td);
        pc.texWidth  = td.Width;
        pc.texHeight = td.Height;
    }
    switch (avFrame->colorspace) {
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M: pc.colorSpace = 0; break;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL: pc.colorSpace = 2; break;
        case AVCOL_SPC_BT709:     pc.colorSpace = 1; break;
        default:                  pc.colorSpace = (w >= 1280 || h >= 720) ? 1 : 0; break;
    }
    if (rangeOverride == 1)      pc.range = 1;
    else if (rangeOverride == 2) pc.range = 0;
    else                         pc.range = (avFrame->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
    ctx->UpdateSubresource(i.cbuf.Get(), 0, nullptr, &pc, 0, 0);

    ID3D11ShaderResourceView *srvs[2] = { sv->luma.Get(), sv->chroma.Get() };
    ID3D11UnorderedAccessView *uav = i.outUav.Get();
    ID3D11SamplerState *samp = i.sampler.Get();
    ID3D11Buffer *cb = i.cbuf.Get();
    ctx->CSSetShader(i.cs.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 2, srvs);
    ctx->CSSetSamplers(0, 1, &samp);
    ctx->CSSetConstantBuffers(0, 1, &cb);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx->Dispatch((static_cast<UINT>(w) + 7) / 8, (static_cast<UINT>(h) + 7) / 8, 1);
    // Unbind so the composite pass can sample the output and the
    // decoder can keep writing the array without a bind hazard.
    ID3D11ShaderResourceView *nullSrvs[2] = { nullptr, nullptr };
    ID3D11UnorderedAccessView *nullUav = nullptr;
    ctx->CSSetShaderResources(0, 2, nullSrvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);

    if (!i.loggedFirst) {
        qInfo("D3D11VaDecodeBridge: zero-copy %s slice %u → RGBA16F %dx%d "
              "(colorSpace=%u range=%u)",
              tenBit ? "P010" : "NV12", slice, w, h, pc.colorSpace, pc.range);
        i.loggedFirst = true;
    }

    i.frame.planes.clear();
    D3D11VulkanDecodeBridge::ImportedPlane p{};
    p.texture    = i.outTex.Get();
    p.srv        = i.outSrv.Get();
    p.width      = w;
    p.height     = h;
    p.dxgiFormat = static_cast<int>(DXGI_FORMAT_R16G16B16A16_FLOAT);
    i.frame.planes.push_back(p);
    i.frame.pictureWidth  = w;
    i.frame.pictureHeight = h;
    i.frame.avSwFormat    = fc->sw_format;
    i.frame.frameSequence = ++i.sequence;
    return &i.frame;
}

} // namespace qcv

#endif // Q_OS_WIN
