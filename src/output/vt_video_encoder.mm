#include "vt_video_encoder.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

#include <QtLogging>

#include <cstring>

namespace qcv::output {

struct VtVideoEncoder::Impl {
    VTCompressionSessionRef session = nullptr;
    CVPixelBufferPoolRef    pool    = nullptr;
    uint32_t poolW = 0, poolH = 0;
    Callback    cb;
    EncoderSpec spec;
    std::atomic<bool> open{false};
    std::atomic<uint64_t> submitted{0}, encoded{0}, keyframes{0}, bytes{0}, errors{0};
    std::mutex poolMutex;

    static void outputCb(void *refcon, void * /*sourceFrameRefCon*/, OSStatus status,
                         VTEncodeInfoFlags flags, CMSampleBufferRef sbuf)
    {
        auto *self = static_cast<Impl *>(refcon);
        if (status != noErr) {
            self->errors.fetch_add(1, std::memory_order_relaxed);
            if (self->errors.load() <= 3) qWarning("VtVideoEncoder: encode error %d", static_cast<int>(status));
            return;
        }
        if ((flags & kVTEncodeInfo_FrameDropped) || !sbuf) return;
        CMBlockBufferRef data = CMSampleBufferGetDataBuffer(sbuf);
        if (!data) return;

        // Keyframe: no NotSync attachment, or the attachment says sync.
        bool key = true;
        if (CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sbuf, false); att && CFArrayGetCount(att) > 0) {
            auto dict = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(att, 0));
            CFBooleanRef notSync = nullptr;
            if (CFDictionaryGetValueIfPresent(dict, kCMSampleAttachmentKey_NotSync, reinterpret_cast<const void **>(&notSync)) && notSync) {
                key = !CFBooleanGetValue(notSync);
            }
        }

        EncodedUnit au;
        au.keyframe = key;
        const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sbuf);
        const CMTime dur = CMSampleBufferGetDuration(sbuf);
        au.pts = CMTIME_IS_VALID(pts) ? CMTimeConvertScale(pts, 90000, kCMTimeRoundingMethod_Default).value : 0;
        au.dur = CMTIME_IS_VALID(dur) ? CMTimeConvertScale(dur, 90000, kCMTimeRoundingMethod_Default).value : 0;

        static const uint8_t kStart[4] = {0, 0, 0, 1};
        size_t total = 0;
        char *base = nullptr;
        std::vector<uint8_t> tmp;
        if (CMBlockBufferGetDataPointer(data, 0, nullptr, &total, &base) != noErr || !base) {
            // Not contiguous: copy it out first.
            total = CMBlockBufferGetDataLength(data);
            tmp.resize(total);
            if (CMBlockBufferCopyDataBytes(data, 0, total, tmp.data()) != noErr) return;
            base = reinterpret_cast<char *>(tmp.data());
        }
        au.data.reserve(total + 256);
        self->emit(au, sbuf, key, reinterpret_cast<const uint8_t *>(base), total, kStart);
    }

    void emit(EncodedUnit &au, CMSampleBufferRef sbuf, bool key, const uint8_t *p, size_t total, const uint8_t *kStart)
    {
        // Parameter sets in band on keyframes (Louper/TS want them there).
        if (key) {
            if (CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sbuf)) {
                size_t count = 0; int nalLen = 4;
                if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, nullptr, nullptr, &count, &nalLen) == noErr) {
                    for (size_t i = 0; i < count; ++i) {
                        const uint8_t *ps = nullptr; size_t psLen = 0;
                        if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &ps, &psLen, nullptr, nullptr) == noErr && ps) {
                            au.data.insert(au.data.end(), kStart, kStart + 4);
                            au.data.insert(au.data.end(), ps, ps + psLen);
                        }
                    }
                }
            }
        }
        // AVCC (4-byte big-endian lengths) → Annex-B start codes. The
        // session's nalUnitHeaderLength is 4 for every H.264 encoder Apple
        // ships; the parameter-set query above would say otherwise.
        size_t off = 0;
        while (off + 4 <= total) {
            const uint32_t n = (uint32_t(p[off]) << 24) | (uint32_t(p[off + 1]) << 16) | (uint32_t(p[off + 2]) << 8) | uint32_t(p[off + 3]);
            off += 4;
            if (n == 0 || off + n > total) break;
            au.data.insert(au.data.end(), kStart, kStart + 4);
            au.data.insert(au.data.end(), p + off, p + off + n);
            off += n;
        }
        if (au.data.empty()) return;
        encoded.fetch_add(1, std::memory_order_relaxed);
        if (key) keyframes.fetch_add(1, std::memory_order_relaxed);
        bytes.fetch_add(au.data.size(), std::memory_order_relaxed);
        if (cb) cb(std::move(au));
    }

    bool setProp(CFStringRef key, CFTypeRef value, const char *what)
    {
        const OSStatus st = VTSessionSetProperty(session, key, value);
        if (st != noErr) qWarning("VtVideoEncoder: property %s rejected (%d)", what, static_cast<int>(st));
        return st == noErr;
    }
    bool setBool(CFStringRef key, bool v, const char *what) { return setProp(key, v ? kCFBooleanTrue : kCFBooleanFalse, what); }
    bool setInt(CFStringRef key, int v, const char *what)
    {
        CFNumberRef n = CFNumberCreate(nullptr, kCFNumberIntType, &v);
        const bool ok = setProp(key, n, what);
        CFRelease(n);
        return ok;
    }
    bool setDouble(CFStringRef key, double v, const char *what)
    {
        CFNumberRef n = CFNumberCreate(nullptr, kCFNumberDoubleType, &v);
        const bool ok = setProp(key, n, what);
        CFRelease(n);
        return ok;
    }
};

VtVideoEncoder::VtVideoEncoder() : m_impl(std::make_unique<Impl>()) {}
VtVideoEncoder::~VtVideoEncoder() { close(); releaseSurfaces(); }

bool VtVideoEncoder::isOpen() const { return m_impl->open.load(std::memory_order_acquire); }

bool VtVideoEncoder::open(const EncoderSpec &spec, Callback cb, std::string *error)
{
    close();
    m_impl->spec = spec;
    m_impl->cb   = std::move(cb);
    auto fail = [&](const std::string &msg) {
        if (error) *error = msg;
        qWarning("VtVideoEncoder: %s", msg.c_str());
        close();
        return false;
    };

    if (!m_impl->pool || m_impl->poolW != spec.width || m_impl->poolH != spec.height) {
        std::string perr;
        if (!prepareSurfaces(spec.width, spec.height, &perr)) return fail(perr);
    }
    // The session takes our pool's buffers; its own imageBufferAttributes
    // describe what it accepts so a mismatch shows up at EncodeFrame.
    NSDictionary *imageAttrs = @{
        (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (id)kCVPixelBufferWidthKey:  @(spec.width),
        (id)kCVPixelBufferHeightKey: @(spec.height),
    };
    NSDictionary *encoderSpec = @{
        (id)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder: @YES,
    };
    OSStatus st = VTCompressionSessionCreate(
        kCFAllocatorDefault, static_cast<int32_t>(spec.width), static_cast<int32_t>(spec.height),
        kCMVideoCodecType_H264, (__bridge CFDictionaryRef)encoderSpec, (__bridge CFDictionaryRef)imageAttrs,
        nullptr, &Impl::outputCb, m_impl.get(), &m_impl->session);
    if (st != noErr || !m_impl->session) {
        return fail("VTCompressionSessionCreate failed (" + std::to_string(st) + ")");
    }
    Impl &I = *m_impl;
    const double fps = spec.fpsDen ? double(spec.fpsNum) / double(spec.fpsDen) : 30.0;
    I.setBool(kVTCompressionPropertyKey_RealTime, true, "RealTime");
    I.setBool(kVTCompressionPropertyKey_AllowFrameReordering, false, "AllowFrameReordering");
    I.setBool(kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, true, "PrioritizeEncodingSpeedOverQuality");
    I.setDouble(kVTCompressionPropertyKey_ExpectedFrameRate, fps, "ExpectedFrameRate");
    I.setInt(kVTCompressionPropertyKey_MaxKeyFrameInterval, static_cast<int>(spec.gopFrames ? spec.gopFrames : 30), "MaxKeyFrameInterval");
    I.setDouble(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, 1.0, "MaxKeyFrameIntervalDuration");
    I.setInt(kVTCompressionPropertyKey_AverageBitRate, static_cast<int>(spec.bitrateKbps) * 1000, "AverageBitRate");
    {
        // Hard cap: 2x the average over any one second, in bytes.
        const int bytesPerSec = static_cast<int>(spec.bitrateKbps) * 1000 / 8 * 2;
        const int window = 1;
        CFNumberRef a = CFNumberCreate(nullptr, kCFNumberIntType, &bytesPerSec);
        CFNumberRef b = CFNumberCreate(nullptr, kCFNumberIntType, &window);
        const void *vals[2] = { a, b };
        CFArrayRef limits = CFArrayCreate(nullptr, vals, 2, &kCFTypeArrayCallBacks);
        I.setProp(kVTCompressionPropertyKey_DataRateLimits, limits, "DataRateLimits");
        CFRelease(limits); CFRelease(a); CFRelease(b);
    }
    I.setProp(kVTCompressionPropertyKey_ProfileLevel,
              spec.profile == H264Profile::Main ? kVTProfileLevel_H264_Main_AutoLevel
                                                : kVTProfileLevel_H264_Baseline_AutoLevel, "ProfileLevel");
    I.setProp(kVTCompressionPropertyKey_H264EntropyMode,
              spec.profile == H264Profile::Main ? kVTH264EntropyMode_CABAC : kVTH264EntropyMode_CAVLC, "H264EntropyMode");
    I.setProp(kVTCompressionPropertyKey_ColorPrimaries,   kCVImageBufferColorPrimaries_ITU_R_709_2, "ColorPrimaries");
    I.setProp(kVTCompressionPropertyKey_TransferFunction, kCVImageBufferTransferFunction_ITU_R_709_2, "TransferFunction");
    I.setProp(kVTCompressionPropertyKey_YCbCrMatrix,      kCVImageBufferYCbCrMatrix_ITU_R_709_2, "YCbCrMatrix");
    st = VTCompressionSessionPrepareToEncodeFrames(I.session);
    if (st != noErr) return fail("PrepareToEncodeFrames failed (" + std::to_string(st) + ")");

    CFStringRef encName = nullptr;
    if (VTSessionCopyProperty(I.session, kVTCompressionPropertyKey_EncoderID, nullptr, &encName) == noErr && encName) {
        char buf[128] = {};
        CFStringGetCString(encName, buf, sizeof buf, kCFStringEncodingUTF8);
        qInfo("VtVideoEncoder: %ux%u @ %.3f fps, %u kbps, %s, GOP %u — %s", spec.width, spec.height, fps,
              spec.bitrateKbps, spec.profile == H264Profile::Main ? "Main" : "Baseline", spec.gopFrames, buf);
        CFRelease(encName);
    }
    I.submitted = 0; I.encoded = 0; I.keyframes = 0; I.bytes = 0; I.errors = 0;
    I.open.store(true, std::memory_order_release);
    return true;
}

void VtVideoEncoder::close()
{
    Impl &I = *m_impl;
    if (!I.session) return;
    I.open.store(false, std::memory_order_release);
    VTCompressionSessionCompleteFrames(I.session, kCMTimeInvalid);   // drains in flight; callbacks fire before it returns
    VTCompressionSessionInvalidate(I.session);
    CFRelease(I.session);
    I.session = nullptr;
    I.cb = nullptr;
}

bool VtVideoEncoder::prepareSurfaces(uint32_t width, uint32_t height, std::string *error)
{
    Impl &I = *m_impl;
    if (I.pool && I.poolW == width && I.poolH == height) return true;
    releaseSurfaces();
    // NV12 video range, IOSurface-backed, Metal compatible: the renderer
    // wraps the two planes as textures and writes them in place.
    NSDictionary *attrs = @{
        (id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (id)kCVPixelBufferWidthKey:  @(width),
        (id)kCVPixelBufferHeightKey: @(height),
        (id)kCVPixelBufferIOSurfacePropertiesKey: @{},
        (id)kCVPixelBufferMetalCompatibilityKey: @YES,
    };
    NSDictionary *poolAttrs = @{ (id)kCVPixelBufferPoolMinimumBufferCountKey: @4 };
    const CVReturn rc = CVPixelBufferPoolCreate(kCFAllocatorDefault, (__bridge CFDictionaryRef)poolAttrs,
                                                (__bridge CFDictionaryRef)attrs, &I.pool);
    if (rc != kCVReturnSuccess || !I.pool) {
        const std::string msg = "CVPixelBufferPoolCreate failed (" + std::to_string(rc) + ")";
        if (error) *error = msg;
        qWarning("VtVideoEncoder: %s", msg.c_str());
        I.pool = nullptr;
        return false;
    }
    I.poolW = width; I.poolH = height;
    return true;
}

void VtVideoEncoder::releaseSurfaces()
{
    Impl &I = *m_impl;
    if (I.pool) { CVPixelBufferPoolRelease(I.pool); I.pool = nullptr; }
    I.poolW = I.poolH = 0;
}

void *VtVideoEncoder::acquireSurface()
{
    Impl &I = *m_impl;
    if (!I.pool) return nullptr;
    CVPixelBufferRef pix = nullptr;
    // Bounded: the pool allocates on demand; cap the live set so a stalled
    // encoder cannot grow it without end.
    NSDictionary *aux = @{ (id)kCVPixelBufferPoolAllocationThresholdKey: @8 };
    const CVReturn rc = CVPixelBufferPoolCreatePixelBufferWithAuxAttributes(
        kCFAllocatorDefault, I.pool, (__bridge CFDictionaryRef)aux, &pix);
    if (rc != kCVReturnSuccess || !pix) {
        if (rc != kCVReturnWouldExceedAllocationThreshold) {
            qWarning("VtVideoEncoder: pixel buffer from pool failed (%d)", rc);
        }
        return nullptr;
    }
    return pix;   // +1 for the caller
}

void VtVideoEncoder::retainSurface(void *surface)  { if (surface) CFRetain(static_cast<CVPixelBufferRef>(surface)); }
void VtVideoEncoder::releaseSurface(void *surface) { if (surface) CFRelease(static_cast<CVPixelBufferRef>(surface)); }

bool VtVideoEncoder::encode(void *surface, int64_t pts90k, int64_t dur90k, bool forceKeyframe)
{
    Impl &I = *m_impl;
    if (!I.open.load(std::memory_order_acquire) || !surface) return false;
    NSDictionary *frameProps = forceKeyframe ? @{ (id)kVTEncodeFrameOptionKey_ForceKeyFrame: @YES } : nil;
    const OSStatus st = VTCompressionSessionEncodeFrame(
        I.session, static_cast<CVPixelBufferRef>(surface), CMTimeMake(pts90k, 90000),
        dur90k > 0 ? CMTimeMake(dur90k, 90000) : kCMTimeInvalid,
        (__bridge CFDictionaryRef)frameProps, nullptr, nullptr);
    if (st != noErr) {
        I.errors.fetch_add(1, std::memory_order_relaxed);
        if (I.errors.load() <= 3) qWarning("VtVideoEncoder: EncodeFrame failed (%d)", static_cast<int>(st));
        return false;
    }
    I.submitted.fetch_add(1, std::memory_order_relaxed);
    return true;
}

VideoEncoder::Stats VtVideoEncoder::stats() const
{
    const Impl &I = *m_impl;
    return { I.submitted.load(), I.encoded.load(), I.keyframes.load(), I.bytes.load(), I.errors.load() };
}

std::unique_ptr<VideoEncoder> makePlatformVideoEncoder() { return std::make_unique<VtVideoEncoder>(); }

} // namespace qcv::output
