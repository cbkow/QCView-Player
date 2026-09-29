// MetalYuvRenderer — see header for adaptation notes.

#include "metal_yuv_renderer.h"

#include "decode/yuv_planar.h"

#include "metal_device_manager.h"

#import <Metal/Metal.h>

#include <QtLogging>

namespace qcv {

namespace {

// MSL compute kernels — byte-identical to old app's
// metal_yuv_renderer.mm. Color-matrix constants are BT.709 NCL
// (default) and BT.2020 NCL (for 10-bit HDR sources).
constexpr const char *kYuvShaderSource = R"(
#include <metal_stdlib>
using namespace metal;

struct YUVParams {
    uint width;
    uint height;
    uint bit_depth;
    uint is_full_range;
    uint is_bt2020;
    uint is_hdr;
    uint subsampling;  // 0=4:2:0, 1=4:2:2, 2=4:4:4
};

// Video-range level removal. >8-bit planes arrive as 16-bit unorm with the
// code value MSB-aligned (x420/x422/x444: 10-bit << 6), and the 16-bit
// video-range formats (sv22/sv44/y416) use the same levels, so both share
// the 16-bit constants: Y 4096..60160, C 4096..61440, C mid 32768.
// The 8-bit fractions (128/255 = 0.50196 vs 32768/65535 = 0.50001) would
// bias Cb/Cr negative and tint every neutral green.
float3 video_range_to_ycbcr(float y_val, float cb_raw, float cr_raw, uint bit_depth)
{
    if (bit_depth > 8) {
        return float3((y_val  -  4096.0/65535.0) * (65535.0/56064.0),
                      (cb_raw - 32768.0/65535.0) * (65535.0/57344.0),
                      (cr_raw - 32768.0/65535.0) * (65535.0/57344.0));
    }
    // Video range: Y [16/255, 235/255], UV [16/255, 240/255]
    return float3((y_val  -  16.0/255.0) * (255.0/219.0),
                  (cb_raw - 128.0/255.0) * (255.0/224.0),
                  (cr_raw - 128.0/255.0) * (255.0/224.0));
}

kernel void yuv_to_rgba(
    texture2d<float, access::read>  y_tex   [[texture(0)]],
    texture2d<float, access::read>  uv_tex  [[texture(1)]],
    texture2d<float, access::write> out_tex [[texture(2)]],
    constant YUVParams &params              [[buffer(0)]],
    uint2 gid                               [[thread_position_in_grid]])
{
    if (gid.x >= params.width || gid.y >= params.height) return;

    float y_val = y_tex.read(gid).r;

    uint2 uv_pos;
    if (params.subsampling == 2) {
        uv_pos = gid;                          // 4:4:4
    } else if (params.subsampling == 1) {
        uv_pos = uint2(gid.x / 2, gid.y);      // 4:2:2
    } else {
        uv_pos = uint2(gid.x / 2, gid.y / 2);  // 4:2:0
    }
    float2 uv_val = uv_tex.read(uv_pos).rg;

    float y, cb, cr;
    if (params.is_full_range) {
        y  = y_val;
        cb = uv_val.x - 0.5;
        cr = uv_val.y - 0.5;
    } else {
        float3 ycc = video_range_to_ycbcr(y_val, uv_val.x, uv_val.y, params.bit_depth);
        y  = ycc.x;
        cb = ycc.y;
        cr = ycc.z;
    }

    float r, g, b;
    if (params.is_bt2020) {
        r = y + 1.4746   * cr;
        g = y - 0.16455  * cb - 0.57135 * cr;
        b = y + 1.8814   * cb;
    } else {
        r = y + 1.5748   * cr;
        g = y - 0.1873   * cb - 0.4681  * cr;
        b = y + 1.8556   * cb;
    }

    // Unclamped: super-whites, sub-blacks and out-of-gamut triplets ride
    // into OCIO (RGBA16F holds them). The present pass clamps to 0..1 when
    // OCIO is bypassed, so the raw view is unchanged.
    out_tex.write(float4(r, g, b, 1.0), gid);
}

// Y416 / Y408 layout: A,Y,Cb,Cr packed into RGBA channels of one
// 16-bit unorm texture. Used by ProRes 4444 alpha video.
kernel void yuv_interleaved_to_rgba(
    texture2d<float, access::read>  in_tex  [[texture(0)]],
    texture2d<float, access::write> out_tex [[texture(1)]],
    constant YUVParams &params              [[buffer(0)]],
    uint2 gid                               [[thread_position_in_grid]])
{
    if (gid.x >= params.width || gid.y >= params.height) return;

    float4 sample = in_tex.read(gid);
    float alpha  = sample.r;
    float y_val  = sample.g;
    float cb_raw = sample.b;
    float cr_raw = sample.a;

    float y, cb, cr;
    if (params.is_full_range) {
        y  = y_val;
        cb = cb_raw - 0.5;
        cr = cr_raw - 0.5;
    } else {
        float3 ycc = video_range_to_ycbcr(y_val, cb_raw, cr_raw, params.bit_depth);
        y  = ycc.x;
        cb = ycc.y;
        cr = ycc.z;
    }

    float r, g, b;
    if (params.is_bt2020) {
        r = y + 1.4746   * cr;
        g = y - 0.16455  * cb - 0.57135 * cr;
        b = y + 1.8814   * cb;
    } else {
        r = y + 1.5748   * cr;
        g = y - 0.1873   * cb - 0.4681  * cr;
        b = y + 1.8556   * cb;
    }

    out_tex.write(float4(r, g, b, alpha), gid);   // unclamped, see yuv_to_rgba
}

// Clean YUV (decode/yuv_planar.h): software-decoded planes, any depth /
// alignment / subsampling. Levels in code values (sample × codeMax); the
// matrix from Kr / Kb, so BT.601 / 709 / 2020 / 240M / FCC all decode as
// swscale did — minus its clamp. Chroma is sampled bilinearly at the luma
// pixel centre.
struct PlanarParams {
    uint  width;
    uint  height;
    uint  interleaved;   // chroma as one RG texture (NV12 / P010)
    uint  has_alpha;
    uint  full_range;
    float code_max;
    float level_k;
    float full_max;
    float chroma_mid;
    float kr;
    float kb;
};

kernel void yuv_planar_to_rgba(
    texture2d<float, access::read>   y_tex   [[texture(0)]],
    texture2d<float, access::sample> u_tex   [[texture(1)]],
    texture2d<float, access::sample> v_tex   [[texture(2)]],
    texture2d<float, access::read>   a_tex   [[texture(3)]],
    texture2d<float, access::write>  out_tex [[texture(4)]],
    constant PlanarParams &p                 [[buffer(0)]],
    uint2 gid                                [[thread_position_in_grid]])
{
    if (gid.x >= p.width || gid.y >= p.height) return;
    constexpr sampler bilinear(coord::normalized, filter::linear, address::clamp_to_edge);
    const float2 uv = (float2(gid) + 0.5) / float2(p.width, p.height);

    const float yc = y_tex.read(gid).r * p.code_max;
    float uc, vc;
    if (p.interleaved) {
        const float2 c = u_tex.sample(bilinear, uv).rg * p.code_max;
        uc = c.x;
        vc = c.y;
    } else {
        uc = u_tex.sample(bilinear, uv).r * p.code_max;
        vc = v_tex.sample(bilinear, uv).r * p.code_max;
    }

    float y, cb, cr;
    if (p.full_range) {
        y  = yc / p.full_max;
        cb = (uc - p.chroma_mid) / p.full_max;
        cr = (vc - p.chroma_mid) / p.full_max;
    } else {
        y  = (yc -  16.0 * p.level_k) / (219.0 * p.level_k);
        cb = (uc - 128.0 * p.level_k) / (224.0 * p.level_k);
        cr = (vc - 128.0 * p.level_k) / (224.0 * p.level_k);
    }
    const float r = y + 2.0 * (1.0 - p.kr) * cr;
    const float b = y + 2.0 * (1.0 - p.kb) * cb;
    const float g = (y - p.kr * r - p.kb * b) / (1.0 - p.kr - p.kb);
    const float a = p.has_alpha ? a_tex.read(gid).r * p.code_max / p.full_max : 1.0;

    // Unclamped, like the biplanar kernel.
    out_tex.write(float4(r, g, b, a), gid);
}
)";

} // namespace

struct MetalYuvRenderer::Impl {
    id<MTLComputePipelineState> biplanarPso    = nil;
    id<MTLComputePipelineState> interleavedPso = nil;
    id<MTLComputePipelineState> planarPso      = nil;

    // Cached output texture descriptor — same dimensions across all
    // frames of a clip, so we re-use it instead of re-computing.
    MTLTextureDescriptor *cachedOutputDesc = nil;
    int                  cachedDescW = 0;
    int                  cachedDescH = 0;

    bool initialized = false;
};

MetalYuvRenderer::MetalYuvRenderer()
    : m_impl(new Impl())
{
}

MetalYuvRenderer::~MetalYuvRenderer()
{
    shutdown();
    delete m_impl;
}

bool MetalYuvRenderer::initialize()
{
    if (m_impl->initialized) return true;
    if (!createComputePipelines()) return false;
    m_impl->initialized = true;
    qInfo("MetalYuvRenderer: initialized");
    return true;
}

void MetalYuvRenderer::shutdown()
{
    if (!m_impl || !m_impl->initialized) return;
    m_impl->biplanarPso     = nil;
    m_impl->interleavedPso  = nil;
    m_impl->cachedOutputDesc = nil;
    m_impl->cachedDescW = 0;
    m_impl->cachedDescH = 0;
    m_impl->initialized = false;
}

bool MetalYuvRenderer::isInitialized() const
{
    return m_impl && m_impl->initialized;
}

bool MetalYuvRenderer::createComputePipelines()
{
    auto &mgr = MetalDeviceManager::instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.device();
    if (!device) {
        qWarning("MetalYuvRenderer: no Metal device");
        return false;
    }

    NSError *err = nil;
    NSString *src = [NSString stringWithUTF8String:kYuvShaderSource];
    id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil error:&err];
    if (!lib) {
        qWarning("MetalYuvRenderer: shader compile failed: %s",
                 err ? [err.localizedDescription UTF8String] : "(no error)");
        return false;
    }

    id<MTLFunction> biplanarFn = [lib newFunctionWithName:@"yuv_to_rgba"];
    if (!biplanarFn) {
        qWarning("MetalYuvRenderer: yuv_to_rgba kernel not found");
        return false;
    }
    m_impl->biplanarPso =
        [device newComputePipelineStateWithFunction:biplanarFn error:&err];
    if (!m_impl->biplanarPso) {
        qWarning("MetalYuvRenderer: biplanar pipeline creation failed: %s",
                 err ? [err.localizedDescription UTF8String] : "(no error)");
        return false;
    }

    if (id<MTLFunction> planarFn = [lib newFunctionWithName:@"yuv_planar_to_rgba"]) {
        m_impl->planarPso = [device newComputePipelineStateWithFunction:planarFn error:&err];
        if (!m_impl->planarPso) {
            qWarning("MetalYuvRenderer: planar pipeline creation failed: %s",
                     err ? [err.localizedDescription UTF8String] : "(no error)");
            // Non-fatal — software YUV then keeps the swscale path.
        }
    }

    id<MTLFunction> interleavedFn =
        [lib newFunctionWithName:@"yuv_interleaved_to_rgba"];
    if (interleavedFn) {
        m_impl->interleavedPso =
            [device newComputePipelineStateWithFunction:interleavedFn error:&err];
        if (!m_impl->interleavedPso) {
            qWarning("MetalYuvRenderer: interleaved pipeline creation failed: %s",
                     err ? [err.localizedDescription UTF8String] : "(no error)");
            // Non-fatal — biplanar still works for non-ProRes-4444.
        }
    }
    return true;
}

namespace {

// YUVParams uniform block — must match MSL struct layout.
struct YuvUniforms {
    uint32_t width;
    uint32_t height;
    uint32_t bitDepth;
    uint32_t isFullRange;
    uint32_t isBt2020;
    uint32_t isHdr;
    uint32_t subsampling;
};

// Refresh the cached output descriptor if the requested size differs.
void refreshOutputDesc(MetalYuvRenderer::Impl *impl, int width, int height)
{
    if (impl->cachedOutputDesc && width == impl->cachedDescW &&
        height == impl->cachedDescH) {
        return;
    }
    MTLTextureDescriptor *desc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA16Float
                                     width:width
                                    height:height
                                 mipmapped:NO];
    desc.usage       = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    desc.storageMode = MTLStorageModePrivate;
    impl->cachedOutputDesc = desc;
    impl->cachedDescW      = width;
    impl->cachedDescH      = height;
}

} // namespace

void *MetalYuvRenderer::renderToRgba(void *cbPtr,
                                     void *yTexture, void *uvTexture,
                                     int width, int height,
                                     int bitDepth, bool isFullRange,
                                     bool isBt2020, bool isHdr,
                                     int subsampling)
{
    if (!m_impl->initialized || !yTexture || !uvTexture) return nullptr;
    if (!m_impl->biplanarPso || !cbPtr) return nullptr;

    auto &mgr = MetalDeviceManager::instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.device();

    refreshOutputDesc(m_impl, width, height);
    id<MTLTexture> output =
        [device newTextureWithDescriptor:m_impl->cachedOutputDesc];
    if (!output) {
        qWarning("MetalYuvRenderer: newTexture failed (%dx%d)", width, height);
        return nullptr;
    }

    // Encode into the caller's command buffer — no separate cb,
    // no commit, no waitUntilCompleted. The compositor encoder
    // that runs later in the same buffer reads `output` with
    // Metal's automatic resource hazard tracking guaranteeing
    // ordering.
    id<MTLCommandBuffer>         cb  = (__bridge id<MTLCommandBuffer>)cbPtr;
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:m_impl->biplanarPso];

    [enc setTexture:(__bridge id<MTLTexture>)yTexture  atIndex:0];
    [enc setTexture:(__bridge id<MTLTexture>)uvTexture atIndex:1];
    [enc setTexture:output                              atIndex:2];

    YuvUniforms u = {
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        static_cast<uint32_t>(bitDepth),
        isFullRange ? 1u : 0u,
        isBt2020    ? 1u : 0u,
        isHdr       ? 1u : 0u,
        static_cast<uint32_t>(subsampling),
    };
    [enc setBytes:&u length:sizeof(u) atIndex:0];

    const MTLSize tg   = MTLSizeMake(16, 16, 1);
    const MTLSize grid = MTLSizeMake(width, height, 1);
    [enc dispatchThreads:grid threadsPerThreadgroup:tg];

    [enc endEncoding];

    // __bridge_retained: hand a +1 reference out to the caller.
    return (__bridge_retained void *)output;
}

void *MetalYuvRenderer::renderInterleavedToRgba(void *cbPtr,
                                                void *inTexture,
                                                int width, int height,
                                                int bitDepth, bool isFullRange,
                                                bool isBt2020, bool isHdr)
{
    if (!m_impl->initialized || !inTexture) return nullptr;
    if (!m_impl->interleavedPso) {
        qWarning("MetalYuvRenderer: interleaved pipeline unavailable");
        return nullptr;
    }
    if (!cbPtr) return nullptr;

    auto &mgr = MetalDeviceManager::instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.device();

    refreshOutputDesc(m_impl, width, height);
    id<MTLTexture> output =
        [device newTextureWithDescriptor:m_impl->cachedOutputDesc];
    if (!output) {
        qWarning("MetalYuvRenderer: newTexture failed (%dx%d, interleaved)",
                 width, height);
        return nullptr;
    }

    id<MTLCommandBuffer>         cb  = (__bridge id<MTLCommandBuffer>)cbPtr;
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:m_impl->interleavedPso];

    [enc setTexture:(__bridge id<MTLTexture>)inTexture atIndex:0];
    [enc setTexture:output                              atIndex:1];

    YuvUniforms u = {
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        static_cast<uint32_t>(bitDepth),
        isFullRange ? 1u : 0u,
        isBt2020    ? 1u : 0u,
        isHdr       ? 1u : 0u,
        2u,    // interleaved is always 4:4:4 by definition
    };
    [enc setBytes:&u length:sizeof(u) atIndex:0];

    const MTLSize tg   = MTLSizeMake(16, 16, 1);
    const MTLSize grid = MTLSizeMake(width, height, 1);
    [enc dispatchThreads:grid threadsPerThreadgroup:tg];

    [enc endEncoding];

    return (__bridge_retained void *)output;
}

bool MetalYuvRenderer::hasPlanar() const
{
    return m_impl->initialized && m_impl->planarPso != nil;
}

void *MetalYuvRenderer::renderPlanarToRgba(void *cbPtr, void *const *planeTextures,
                                           const YuvPlanarDesc &d)
{
    if (!hasPlanar() || !cbPtr || !planeTextures || !d.ok) return nullptr;
    for (int i = 0; i < d.planeCount; ++i) {
        if (!planeTextures[i]) return nullptr;
    }

    auto &mgr = MetalDeviceManager::instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.device();
    refreshOutputDesc(m_impl, d.width, d.height);
    id<MTLTexture> output = [device newTextureWithDescriptor:m_impl->cachedOutputDesc];
    if (!output) {
        qWarning("MetalYuvRenderer: newTexture failed (%dx%d, planar)", d.width, d.height);
        return nullptr;
    }

    // Planes in YuvPlanarDesc order: Y, U, V[, A] or Y, UV[, A]. Unused
    // slots get a bound stand-in (the kernel never reads them).
    auto tex = [&](int i) { return (__bridge id<MTLTexture>)planeTextures[i]; };
    id<MTLTexture> yT = tex(0), uT = tex(1);
    id<MTLTexture> vT = d.interleavedChroma ? uT : tex(2);
    id<MTLTexture> aT = d.hasAlpha ? tex(d.planeCount - 1) : yT;

    id<MTLCommandBuffer>         cb  = (__bridge id<MTLCommandBuffer>)cbPtr;
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:m_impl->planarPso];
    [enc setTexture:yT     atIndex:0];
    [enc setTexture:uT     atIndex:1];
    [enc setTexture:vT     atIndex:2];
    [enc setTexture:aT     atIndex:3];
    [enc setTexture:output atIndex:4];

    struct {
        uint32_t width, height, interleaved, hasAlpha, fullRange;
        float    codeMax, levelK, fullMax, chromaMid, kr, kb;
    } u = {
        static_cast<uint32_t>(d.width), static_cast<uint32_t>(d.height),
        d.interleavedChroma ? 1u : 0u, d.hasAlpha ? 1u : 0u, d.fullRange ? 1u : 0u,
        d.codeMax, d.levelK, d.fullMax, d.chromaMid, d.kr, d.kb,
    };
    [enc setBytes:&u length:sizeof(u) atIndex:0];
    [enc dispatchThreads:MTLSizeMake(d.width, d.height, 1)
        threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
    [enc endEncoding];
    return (__bridge_retained void *)output;
}

} // namespace qcv
