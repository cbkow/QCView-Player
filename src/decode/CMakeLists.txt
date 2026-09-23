# decode subsystem — VideoDecoder + ScrubDecoder + FrameIndex.
#
# Phase 1.8.1: VideoDecoder skeleton, software decode + CPU upload.
# Phase 1.8.2a: VideoToolbox attach with CPU readback.
# Phase 1.8.2b: zero-copy via CVPixelBuffer (FrameHandle::Metal).
# Phase 1.8.2c+: hwaccel zero-copy on Linux/Windows; ScrubDecoder;
#                FrameIndex.

qt_add_library(qcv_decode STATIC
    video_decoder.cpp
    video_decoder.h
    frame_handle.h
    frame_index.cpp
    read_ahead.cpp
    read_ahead.h
    frame_index.h
    scrub_decoder.cpp
    scrub_decoder.h
    rgb_range.h
    thread_policy.h
    hw_routing.h
    sws_threaded.h
    stream_extent.h
    seek_compat.h
    sws_rgba_image.h
    live_source.h
    qcbae/host_bridge_url.h
    live_stream_decoder.cpp
    live_stream_decoder.h
    scrub_frame_cache.cpp
    scrub_frame_cache.h
    decoder_cleanup_queue.cpp
    decoder_cleanup_queue.h
    timecode_formatter.cpp
    timecode_formatter.h
    image_loader.h
    image_loader.cpp
    png_image_loader.h
    png_image_loader.cpp
    tiff_image_loader.h
    tiff_image_loader.cpp
    jpeg_image_loader.h
    jpeg_image_loader.cpp
    exr_image_loader.h
    exr_image_loader.cpp
    memory_mapped_istream.h
    memory_mapped_istream.cpp
    simple_lru.h
    image_sequence_cache.h
    image_sequence_cache.cpp
    video_image_loader.h
    video_image_loader.cpp
)

# QCBridgeAE After Effects / Premiere live source. The vendored ring is
# POSIX shared memory on macOS and a named file mapping on Windows
# (QCBridgeAE phase A5, 2026-09-23); the same source builds on both.
# QCV_HAS_HOST_BRIDGE gates the qcbae:// routing and menu items.
target_sources(qcv_decode PRIVATE
    host_bridge_source.cpp
    host_bridge_source.h
    qcbae/frame_desc.h
    qcbae/shared_ring.cpp
    qcbae/shared_ring.h
)
target_compile_definitions(qcv_decode PUBLIC QCV_HAS_HOST_BRIDGE=1)

if(APPLE)
    target_sources(qcv_decode PRIVATE
        frame_handle.mm
    )
    target_link_libraries(qcv_decode PRIVATE
        "-framework CoreVideo"
    )
else()
    # Phase F.2.0 — non-Apple FrameHandle implementation. Provides the
    # cross-platform methods (cpu / cpuShared / operator= / reset) plus
    # inert stubs for the CoreVideo extern "C" callbacks so the Windows /
    # Linux link succeeds without a CVPixelBuffer dependency.
    target_sources(qcv_decode PRIVATE
        frame_handle_nonapple.cpp
    )
endif()

target_include_directories(qcv_decode
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# Static libpng / libtiff need zlib explicitly (the shared Homebrew builds
# carried it implicitly).
find_package(ZLIB REQUIRED)

target_link_libraries(qcv_decode
    PUBLIC
        Qt6::Core
        Qt6::Gui
        PkgConfig::FFMPEG
    PRIVATE
        PNG::PNG
        TIFF::TIFF
        JPEG::JPEG
        # Static libpng / libtiff pull in zlib themselves; with the shared
        # Homebrew builds it came along implicitly.
        ZLIB::ZLIB
        OpenEXR::OpenEXR
        Imath::Imath
)

# Phase F.2.4 — Vulkan link on non-Apple. qcv_decode owns the
# decode-only VulkanDeviceManager (decode queue family + win32 external
# memory + external semaphore extensions). The renderer-side bridge
# (D3D11VulkanDecodeBridge in qcv_render) accesses Vulkan handles
# through opaque void* getters so the qcv_render link to Vulkan stays
# decode-bridge-scoped, not "Vulkan-render" scoped.
if(NOT APPLE)
    # PUBLIC: VulkanDeviceManager + FrameHandle::Vulkan (F.2.4.2)
    # expose Vulkan types in their headers, so any qcv_decode
    # dependent (qcv_render, qcview) needs the Vulkan include path.
    target_link_libraries(qcv_decode PUBLIC Vulkan::Vulkan)
    target_sources(qcv_decode PRIVATE
        # Phase F.2.4.1 — decode-only VulkanDeviceManager. Lifted from
        # phase-e5-end's render-side device manager with surface /
        # swapchain / VMA stripped out; FFmpeg hybrid extension list
        # and features2 chain preserved verbatim so the device can be
        # shared with FFmpeg via AVVulkanDeviceContext in F.2.4.2.
        vulkan/vulkan_device_manager.h
        vulkan/vulkan_device_manager.cpp
        # Phase F.2.12.a — public helper that wraps VulkanDeviceManager
        # as an AVVulkanDeviceContext. Lifted out of video_decoder.cpp
        # so DualVideoDecoder can reuse the exact same handoff logic.
        vulkan_hw_device_ctx.h
        vulkan_hw_device_ctx.cpp
        d3d11va_hw_device_ctx.h
        d3d11va_hw_device_ctx.cpp
    )
    if(WIN32)
        # vkGetMemoryWin32HandleKHR / vkGetSemaphoreWin32HandleKHR live
        # behind this platform gate. PUBLIC so the same define is
        # active wherever vulkan.h is reached from a qcv_decode header.
        target_compile_definitions(qcv_decode PUBLIC
            VK_USE_PLATFORM_WIN32_KHR
        )
    endif()
endif()
