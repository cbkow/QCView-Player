#include "d3d11_yuv_planar_bridge.h"

#if defined(Q_OS_WIN)

#include <QtLogging>

#include <cstring>

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include "d3d11_device_manager.h"
#include "decode/frame_handle.h"
#include "decode/yuv_planar.h"

using Microsoft::WRL::ComPtr;

namespace qcv {

namespace {

// Metal's yuv_planar_to_rgba in HLSL (metal_yuv_renderer.mm) — keep the
// two in step. Every plane is bound as float4 so R8 / R16 (planar U, V)
// and R8G8 / R16G16 (interleaved UV) share a register type; unused slots
// stay unbound and read zero.
constexpr const char *kCsHlsl = R"(
Texture2D<float4>   yTex   : register(t0);
Texture2D<float4>   uTex   : register(t1);   // U, or UV when interleaved
Texture2D<float4>   vTex   : register(t2);
Texture2D<float4>   aTex   : register(t3);
SamplerState        samp   : register(s0);
RWTexture2D<float4> outImg : register(u0);

cbuffer Params : register(b0)
{
    uint  width;
    uint  height;
    uint  interleaved;
    uint  hasAlpha;
    uint  fullRange;
    float codeMax;     // sample * codeMax = stored code value
    float levelK;      // video-range levels = 8-bit levels * levelK
    float fullMax;
    float chromaMid;
    float kr;
    float kb;
    float pad0;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= width || id.y >= height) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(width, height);

    float yc = yTex.Load(int3(id.xy, 0)).r * codeMax;
    float uc, vc;
    if (interleaved != 0) {
        float2 c = uTex.SampleLevel(samp, uv, 0).rg * codeMax;
        uc = c.x;
        vc = c.y;
    } else {
        uc = uTex.SampleLevel(samp, uv, 0).r * codeMax;
        vc = vTex.SampleLevel(samp, uv, 0).r * codeMax;
    }

    float y, cb, cr;
    if (fullRange != 0) {
        y  = yc / fullMax;
        cb = (uc - chromaMid) / fullMax;
        cr = (vc - chromaMid) / fullMax;
    } else {
        y  = (yc -  16.0 * levelK) / (219.0 * levelK);
        cb = (uc - 128.0 * levelK) / (224.0 * levelK);
        cr = (vc - 128.0 * levelK) / (224.0 * levelK);
    }
    float r = y + 2.0 * (1.0 - kr) * cr;
    float b = y + 2.0 * (1.0 - kb) * cb;
    float g = (y - kr * r - kb * b) / (1.0 - kr - kb);
    float a = hasAlpha != 0 ? aTex.Load(int3(id.xy, 0)).r * codeMax / fullMax : 1.0;

    outImg[id.xy] = float4(r, g, b, a);   // unclamped
}
)";

struct ParamsCb {
    uint32_t width, height, interleaved, hasAlpha;
    uint32_t fullRange;
    float    codeMax, levelK, fullMax;
    float    chromaMid, kr, kb, pad0;
};
static_assert(sizeof(ParamsCb) % 16 == 0, "cbuffer size must be a multiple of 16");

DXGI_FORMAT planeFormat(int bytesPerSample, int channels)
{
    if (bytesPerSample == 2)
        return channels == 2 ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R16_UNORM;
    return channels == 2 ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R8_UNORM;
}

struct PlaneTex {
    ComPtr<ID3D11Texture2D>          tex;
    ComPtr<ID3D11ShaderResourceView> srv;
    int         w = 0, h = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
};

} // namespace

struct D3D11YuvPlanarBridge::Impl {
    bool initialized = false;

    ComPtr<ID3D11ComputeShader> cs;
    ComPtr<ID3D11Buffer>        cbuf;
    ComPtr<ID3D11SamplerState>  sampler;

    PlaneTex planes[4];

    ComPtr<ID3D11Texture2D>           outTex;
    ComPtr<ID3D11ShaderResourceView>  outSrv;
    ComPtr<ID3D11UnorderedAccessView> outUav;
    int outW = 0, outH = 0;

    D3D11VulkanDecodeBridge::ImportedFrame frame{};
    uint64_t sequence = 0;
    int loggedFormat  = -1;

    bool ensurePlane(ID3D11Device *device, int p, int w, int h, DXGI_FORMAT fmt)
    {
        PlaneTex &pt = planes[p];
        if (pt.tex && pt.w == w && pt.h == h && pt.fmt == fmt) return true;
        pt = PlaneTex{};
        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(w);
        td.Height = static_cast<UINT>(h);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = fmt;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&td, nullptr, pt.tex.GetAddressOf()))
            || FAILED(device->CreateShaderResourceView(pt.tex.Get(), nullptr, pt.srv.GetAddressOf()))) {
            qWarning("D3D11YuvPlanarBridge: plane %d (%dx%d fmt %d) create failed", p, w, h,
                     static_cast<int>(fmt));
            pt = PlaneTex{};
            return false;
        }
        pt.w = w; pt.h = h; pt.fmt = fmt;
        return true;
    }

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
        if (FAILED(device->CreateTexture2D(&td, nullptr, outTex.GetAddressOf()))
            || FAILED(device->CreateShaderResourceView(outTex.Get(), nullptr, outSrv.GetAddressOf()))
            || FAILED(device->CreateUnorderedAccessView(outTex.Get(), nullptr, outUav.GetAddressOf()))) {
            qWarning("D3D11YuvPlanarBridge: RGBA16F output %dx%d create failed", w, h);
            outUav.Reset(); outSrv.Reset(); outTex.Reset();
            return false;
        }
        outW = w; outH = h;
        return true;
    }
};

D3D11YuvPlanarBridge::D3D11YuvPlanarBridge() : m_impl(std::make_unique<Impl>()) {}
D3D11YuvPlanarBridge::~D3D11YuvPlanarBridge() { shutdown(); }

bool D3D11YuvPlanarBridge::isInitialized() const { return m_impl->initialized; }

bool D3D11YuvPlanarBridge::initialize()
{
    if (m_impl->initialized) return true;
    auto *device = static_cast<ID3D11Device *>(D3D11DeviceManager::instance().device());
    if (!device) return false;

    ComPtr<ID3DBlob> blob, err;
    HRESULT hr = D3DCompile(kCsHlsl, std::strlen(kCsHlsl), "yuv_planar_to_rgba", nullptr,
                            nullptr, "main", "cs_5_0", 0, 0,
                            blob.GetAddressOf(), err.GetAddressOf());
    if (FAILED(hr)) {
        qCritical("D3D11YuvPlanarBridge: compute shader compile failed (hr=0x%08lX)\n%s",
                  static_cast<unsigned long>(hr),
                  err ? static_cast<const char *>(err->GetBufferPointer()) : "");
        return false;
    }
    if (FAILED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(),
                                           nullptr, m_impl->cs.GetAddressOf()))) {
        qCritical("D3D11YuvPlanarBridge: CreateComputeShader failed");
        return false;
    }
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(ParamsCb);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&bd, nullptr, m_impl->cbuf.GetAddressOf()))) {
        qCritical("D3D11YuvPlanarBridge: constant buffer create failed");
        return false;
    }
    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    if (FAILED(device->CreateSamplerState(&sd, m_impl->sampler.GetAddressOf()))) {
        qCritical("D3D11YuvPlanarBridge: sampler create failed");
        return false;
    }
    m_impl->initialized = true;
    return true;
}

void D3D11YuvPlanarBridge::releaseTextures()
{
    auto &i = *m_impl;
    for (auto &p : i.planes) p = PlaneTex{};
    i.outUav.Reset(); i.outSrv.Reset(); i.outTex.Reset();
    i.outW = i.outH = 0;
    i.frame = {};
    i.loggedFormat = -1;
}

void D3D11YuvPlanarBridge::shutdown()
{
    releaseTextures();
    auto &i = *m_impl;
    i.sampler.Reset(); i.cbuf.Reset(); i.cs.Reset();
    i.initialized = false;
}

const D3D11VulkanDecodeBridge::ImportedFrame *
D3D11YuvPlanarBridge::consume(const FrameHandle &fh, int rangeOverride)
{
    if (fh.kind() != FrameHandle::Kind::CpuYuv) return nullptr;
    return consumeAVFrame(fh.cpuYuvAvFrame(), rangeOverride);
}

const D3D11VulkanDecodeBridge::ImportedFrame *
D3D11YuvPlanarBridge::consumeAVFrame(const AVFrame *avFrame, int rangeOverride)
{
    auto &i = *m_impl;
    if (!i.initialized || !avFrame) return nullptr;
    const YuvPlanarDesc d = yuvPlanarDesc(avFrame, rangeOverride);
    if (!d.ok) return nullptr;

    auto *device = static_cast<ID3D11Device *>(D3D11DeviceManager::instance().device());
    auto *ctx    = static_cast<ID3D11DeviceContext *>(D3D11DeviceManager::instance().context());
    if (!device || !ctx) return nullptr;
    if (!i.ensureOutput(device, d.width, d.height)) return nullptr;

    ID3D11ShaderResourceView *srvs[4] = {};
    for (int p = 0; p < d.planeCount; ++p) {
        const auto &pl = d.planes[p];
        if (!i.ensurePlane(device, p, pl.width, pl.height,
                           planeFormat(d.bytesPerSample, pl.channels)))
            return nullptr;
        const int linesize = avFrame->linesize[pl.dataIndex];
        if (!avFrame->data[pl.dataIndex] || linesize <= 0) return nullptr;
        ctx->UpdateSubresource(i.planes[p].tex.Get(), 0, nullptr, avFrame->data[pl.dataIndex],
                               static_cast<UINT>(linesize), 0);
        srvs[p] = i.planes[p].srv.Get();
    }
    // Slot layout in the shader: Y t0, U|UV t1, V t2, A t3.
    ID3D11ShaderResourceView *bound[4] = {};
    bound[0] = srvs[0];
    bound[1] = srvs[1];
    int next = 2;
    if (!d.interleavedChroma) bound[2] = srvs[next++];
    if (d.hasAlpha)           bound[3] = srvs[next];

    ParamsCb pc{};
    pc.width       = static_cast<uint32_t>(d.width);
    pc.height      = static_cast<uint32_t>(d.height);
    pc.interleaved = d.interleavedChroma ? 1u : 0u;
    pc.hasAlpha    = d.hasAlpha ? 1u : 0u;
    pc.fullRange   = d.fullRange ? 1u : 0u;
    pc.codeMax     = d.codeMax;
    pc.levelK      = d.levelK;
    pc.fullMax     = d.fullMax;
    pc.chromaMid   = d.chromaMid;
    pc.kr          = d.kr;
    pc.kb          = d.kb;
    ctx->UpdateSubresource(i.cbuf.Get(), 0, nullptr, &pc, 0, 0);

    ID3D11UnorderedAccessView *uav = i.outUav.Get();
    ID3D11SamplerState *samp = i.sampler.Get();
    ID3D11Buffer *cb = i.cbuf.Get();
    ctx->CSSetShader(i.cs.Get(), nullptr, 0);
    ctx->CSSetShaderResources(0, 4, bound);
    ctx->CSSetSamplers(0, 1, &samp);
    ctx->CSSetConstantBuffers(0, 1, &cb);
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ctx->Dispatch((static_cast<UINT>(d.width) + 7) / 8, (static_cast<UINT>(d.height) + 7) / 8, 1);
    ID3D11ShaderResourceView *nullSrvs[4] = {};
    ID3D11UnorderedAccessView *nullUav = nullptr;
    ctx->CSSetShaderResources(0, 4, nullSrvs);
    ctx->CSSetUnorderedAccessViews(0, 1, &nullUav, nullptr);
    ctx->CSSetShader(nullptr, nullptr, 0);

    if (i.loggedFormat != avFrame->format) {
        const char *name = av_get_pix_fmt_name(static_cast<AVPixelFormat>(avFrame->format));
        qInfo("D3D11YuvPlanarBridge: %s %dx%d → RGBA16F (%s, Kr=%.4f Kb=%.4f)",
              name ? name : "?", d.width, d.height, d.fullRange ? "full" : "limited",
              d.kr, d.kb);
        i.loggedFormat = avFrame->format;
    }

    i.frame.planes.clear();
    D3D11VulkanDecodeBridge::ImportedPlane p{};
    p.texture    = i.outTex.Get();
    p.srv        = i.outSrv.Get();
    p.width      = d.width;
    p.height     = d.height;
    p.dxgiFormat = static_cast<int>(DXGI_FORMAT_R16G16B16A16_FLOAT);
    i.frame.planes.push_back(p);
    i.frame.pictureWidth  = d.width;
    i.frame.pictureHeight = d.height;
    i.frame.avSwFormat    = avFrame->format;
    i.frame.frameSequence = ++i.sequence;
    return &i.frame;
}

} // namespace qcv

#endif // Q_OS_WIN
