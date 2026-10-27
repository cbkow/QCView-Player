// MetalAnnotationRenderer — see header for adaptation notes.

#include "metal_annotation_renderer.h"

#include "annotations/stroke_tessellator.h"
#include "metal_device_manager.h"

#import <Metal/Metal.h>

#include <QtLogging>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace qcv {

namespace {

// Stroke render kernel. Vertex format: float2 pos (pixel coords) +
// float4 color. The vertex shader scales pixel coords to NDC using
// a uniform target-size vec2; fragment passes color through. Alpha
// blending is configured on the pipeline.
constexpr const char *kStrokeMSL = R"(
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float2 position [[attribute(0)]];
    float4 color    [[attribute(1)]];
};

struct VertexOut {
    float4 position [[position]];
    float4 color;
};

struct UBO {
    float2 targetSize;   // pixels
    int    encoding;     // 0 sRGB as is, 2 PQ Rec.2100, 3 HLG Rec.2100
    float  refNits;      // PQ graphics white
};

float3 srgbToLinear(float3 c)
{
    float3 lo = c / 12.92;
    float3 hi = pow(max((c + 0.055) / 1.055, 0.0), 2.4);
    return select(lo, hi, c > 0.04045);
}
constant float3x3 kBt709ToBt2020 = float3x3(
    float3(0.6274040, 0.0690970, 0.0163914),
    float3(0.3292820, 0.9195400, 0.0880132),
    float3(0.0433136, 0.0113612, 0.8955950));
float3 pqEncode(float3 lin)
{
    const float m1 = 0.1593017578125, m2 = 78.84375, c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float3 Y = pow(max(lin, 0.0), m1);
    return pow((c1 + c2 * Y) / (1.0 + c3 * Y), m2);
}
float3 hlgEncode(float3 lin)
{
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    float3 l  = max(lin, 0.0);
    float3 lo = sqrt(3.0 * l);
    float3 hi = a * log(max(12.0 * l - b, 1e-6)) + c;
    return select(lo, hi, l > 1.0 / 12.0);
}

vertex VertexOut stroke_vertex(VertexIn in [[stage_in]],
                               constant UBO &u [[buffer(1)]])
{
    // Pixel coords (origin top-left) → NDC (origin center, y-up).
    float2 ndc;
    ndc.x = (in.position.x / u.targetSize.x) * 2.0 - 1.0;
    ndc.y = 1.0 - (in.position.y / u.targetSize.y) * 2.0;

    VertexOut out;
    out.position = float4(ndc, 0.0, 1.0);
    out.color    = in.color;
    return out;
}

fragment float4 stroke_fragment(VertexOut in [[stage_in]],
                                constant UBO &u [[buffer(1)]])
{
    if (u.encoding == 2) {
        float3 lin2020 = kBt709ToBt2020 * srgbToLinear(in.color.rgb);
        return float4(pqEncode(lin2020 * (u.refNits / 10000.0)), in.color.a);
    }
    if (u.encoding == 3) {
        float3 lin2020 = kBt709ToBt2020 * srgbToLinear(in.color.rgb);
        return float4(hlgEncode(lin2020 * 0.1875), in.color.a);   // 0.75^2 / 3: white on 75 %
    }
    return in.color;
}
)";

constexpr std::size_t kInitialVertexBufferBytes = 1u << 20;   // 1 MB

} // namespace

struct MetalAnnotationRenderer::Impl {
    id<MTLRenderPipelineState> pipeline       = nil;
    int                        encoding       = 0;
    float                      refNits        = 203.0f;
    id<MTLBuffer>              vertexBuffer   = nil;
    std::size_t                vertexBufferCap = 0;
    // Append cursor inside vertexBuffer. Each drawMesh writes at
    // this offset and advances; the GPU reads from the per-draw
    // offset. Without an offset, multiple drawMesh calls in the
    // same render encoder all share buffer offset 0 and the second
    // memcpy clobbers the first, so both encoded draws end up
    // sampling the second mesh's vertices. Reset by
    // beginFrame() each present cycle.
    std::size_t                vertexBufferOffset = 0;
    int                        targetPixelFmt = 0;
};

MetalAnnotationRenderer::MetalAnnotationRenderer()
    : m_impl(new Impl())
{
}

MetalAnnotationRenderer::~MetalAnnotationRenderer()
{
    shutdown();
    delete m_impl;
}

bool MetalAnnotationRenderer::initialize(int targetPixelFormat)
{
    if (m_impl->pipeline) return true;

    auto &mgr = MetalDeviceManager::instance();
    id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.device();
    if (!device) {
        qWarning("MetalAnnotationRenderer: no Metal device");
        return false;
    }

    NSError *err = nil;
    NSString *src = [NSString stringWithUTF8String:kStrokeMSL];
    id<MTLLibrary> lib = [device newLibraryWithSource:src options:nil error:&err];
    if (!lib) {
        qWarning("MetalAnnotationRenderer: shader compile failed: %s",
                 err ? [err.localizedDescription UTF8String] : "(no error)");
        return false;
    }
    id<MTLFunction> vsFn = [lib newFunctionWithName:@"stroke_vertex"];
    id<MTLFunction> fsFn = [lib newFunctionWithName:@"stroke_fragment"];
    if (!vsFn || !fsFn) {
        qWarning("MetalAnnotationRenderer: stroke_vertex / stroke_fragment not found");
        return false;
    }

    // Match TessVertex layout (struct from stroke_tessellator.h):
    //   float x, y;       (offset 0,  size 8)
    //   float r, g, b, a; (offset 8,  size 16)
    //   stride = 24 bytes
    MTLVertexDescriptor *vd = [MTLVertexDescriptor new];
    vd.attributes[0].format      = MTLVertexFormatFloat2;
    vd.attributes[0].offset      = 0;
    vd.attributes[0].bufferIndex = 0;
    vd.attributes[1].format      = MTLVertexFormatFloat4;
    vd.attributes[1].offset      = 2 * sizeof(float);
    vd.attributes[1].bufferIndex = 0;
    vd.layouts[0].stride         = 6 * sizeof(float);   // x,y,r,g,b,a
    vd.layouts[0].stepFunction   = MTLVertexStepFunctionPerVertex;

    MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
    rd.vertexFunction   = vsFn;
    rd.fragmentFunction = fsFn;
    rd.vertexDescriptor = vd;
    rd.colorAttachments[0].pixelFormat = static_cast<MTLPixelFormat>(targetPixelFormat);

    // Straight (non-premultiplied) alpha blending. The tessellator
    // emits stroke colors in non-premultiplied form (per
    // ActiveStroke::color); standard source-over compositing.
    rd.colorAttachments[0].blendingEnabled = YES;
    rd.colorAttachments[0].sourceRGBBlendFactor      = MTLBlendFactorSourceAlpha;
    rd.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    rd.colorAttachments[0].sourceAlphaBlendFactor      = MTLBlendFactorOne;
    rd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

    id<MTLRenderPipelineState> pso =
        [device newRenderPipelineStateWithDescriptor:rd error:&err];
    if (!pso) {
        qWarning("MetalAnnotationRenderer: pipeline creation failed: %s",
                 err ? [err.localizedDescription UTF8String] : "(no error)");
        return false;
    }

    // Vertex buffer — grows as needed.
    id<MTLBuffer> vb = [device newBufferWithLength:kInitialVertexBufferBytes
                                           options:MTLResourceStorageModeShared];
    if (!vb) {
        qWarning("MetalAnnotationRenderer: vertex buffer alloc failed");
        return false;
    }

    m_impl->pipeline       = pso;
    m_impl->vertexBuffer   = vb;
    m_impl->vertexBufferCap = kInitialVertexBufferBytes;
    m_impl->targetPixelFmt = targetPixelFormat;
    return true;
}

void MetalAnnotationRenderer::shutdown()
{
    if (!m_impl) return;
    m_impl->pipeline       = nil;
    m_impl->vertexBuffer   = nil;
    m_impl->vertexBufferCap = 0;
    m_impl->targetPixelFmt = 0;
}

bool MetalAnnotationRenderer::isInitialized() const
{
    return m_impl && m_impl->pipeline != nil;
}

void MetalAnnotationRenderer::setEncoding(int encoding, float refNits)
{
    if (!m_impl) return;
    m_impl->encoding = encoding;
    m_impl->refNits  = refNits;
}

int MetalAnnotationRenderer::targetPixelFormat() const
{
    return m_impl ? m_impl->targetPixelFmt : 0;
}

void MetalAnnotationRenderer::drawMesh(void *encoderPtr,
                                       const TessellatedMesh &mesh,
                                       int targetWidth, int targetHeight,
                                       const MeshClip *clip)
{
    if (!isInitialized() || !encoderPtr) return;
    if (mesh.vertices.empty())            return;
    if (targetWidth <= 0 || targetHeight <= 0) return;

    // Scissor to the image rect, clamped to the target (Metal rejects
    // a scissor that leaves the attachment). An empty intersection
    // means nothing of the picture is on screen: skip the draw.
    MTLScissorRect scissor{0, 0, NSUInteger(targetWidth), NSUInteger(targetHeight)};
    if (clip) {
        const double x0 = std::clamp<double>(std::floor(clip->x), 0.0, targetWidth);
        const double y0 = std::clamp<double>(std::floor(clip->y), 0.0, targetHeight);
        const double x1 = std::clamp<double>(std::ceil(clip->x + clip->w), 0.0, targetWidth);
        const double y1 = std::clamp<double>(std::ceil(clip->y + clip->h), 0.0, targetHeight);
        if (x1 <= x0 || y1 <= y0) return;
        scissor = MTLScissorRect{NSUInteger(x0), NSUInteger(y0),
                                 NSUInteger(x1 - x0), NSUInteger(y1 - y0)};
    }

    const std::size_t needBytes = mesh.vertices.size() * sizeof(TessVertex);

    // Align write offset to vertex stride so Metal can interpret
    // [[stage_in]] correctly when the buffer is bound at a non-zero
    // offset.
    constexpr std::size_t kAlign = sizeof(TessVertex);
    if (m_impl->vertexBufferOffset % kAlign != 0) {
        m_impl->vertexBufferOffset +=
            kAlign - (m_impl->vertexBufferOffset % kAlign);
    }

    // Grow vertex buffer if needed. Reserve enough for the running
    // append cursor + this mesh.
    const std::size_t requiredCap = m_impl->vertexBufferOffset + needBytes;
    if (requiredCap > m_impl->vertexBufferCap) {
        auto &mgr = MetalDeviceManager::instance();
        id<MTLDevice> device = (__bridge id<MTLDevice>)mgr.device();
        std::size_t newCap = m_impl->vertexBufferCap;
        if (newCap == 0) newCap = kInitialVertexBufferBytes;
        while (newCap < requiredCap) newCap *= 2;
        id<MTLBuffer> newBuf = [device newBufferWithLength:newCap
                                                  options:MTLResourceStorageModeShared];
        if (!newBuf) {
            qWarning("MetalAnnotationRenderer: vertex buffer grow to %zu failed",
                     newCap);
            return;
        }
        // Carry forward already-encoded data so prior draws in the
        // same encoder still read valid vertices after the swap.
        if (m_impl->vertexBufferOffset > 0 && m_impl->vertexBuffer) {
            std::memcpy(newBuf.contents, m_impl->vertexBuffer.contents,
                        m_impl->vertexBufferOffset);
        }
        m_impl->vertexBuffer    = newBuf;
        m_impl->vertexBufferCap = newCap;
    }

    auto *dst = static_cast<uint8_t *>(m_impl->vertexBuffer.contents)
                + m_impl->vertexBufferOffset;
    std::memcpy(dst, mesh.vertices.data(), needBytes);

    id<MTLRenderCommandEncoder> enc =
        (__bridge id<MTLRenderCommandEncoder>)encoderPtr;

    [enc setRenderPipelineState:m_impl->pipeline];
    [enc setVertexBuffer:m_impl->vertexBuffer
                  offset:m_impl->vertexBufferOffset
                 atIndex:0];

    struct UBO {
        float targetWidth, targetHeight;
        int   encoding;
        float refNits;
    } ubo = { static_cast<float>(targetWidth), static_cast<float>(targetHeight), m_impl->encoding, m_impl->refNits };
    [enc setVertexBytes:&ubo length:sizeof(ubo) atIndex:1];
    [enc setFragmentBytes:&ubo length:sizeof(ubo) atIndex:1];

    if (clip) [enc setScissorRect:scissor];
    [enc drawPrimitives:MTLPrimitiveTypeTriangle
            vertexStart:0
            vertexCount:mesh.vertices.size()];
    if (clip) {
        [enc setScissorRect:MTLScissorRect{0, 0, NSUInteger(targetWidth),
                                           NSUInteger(targetHeight)}];
    }

    m_impl->vertexBufferOffset += needBytes;
}

void MetalAnnotationRenderer::beginFrame()
{
    // Reset the append cursor at the start of each frame's render
    // encoding. Prior frame's draws are already encoded into a
    // distinct cb (we don't reuse encoders across frames) and the
    // GPU has either consumed those buffer ranges or is doing so;
    // overwriting now is safe as long as we don't reach back into
    // the same draw indices.
    if (m_impl) m_impl->vertexBufferOffset = 0;
}

} // namespace qcv
