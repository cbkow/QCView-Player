# output subsystem — the stream output (plan A): the paced sink, the
# platform's hardware H.264 encoder behind VideoEncoder, and the
# libavformat muxer (MPEG-TS over SRT; FLV over RTMP with A3).
#
# The renderer-facing interfaces (IGpuFrameSink, OutputTarget) stay
# header-only in src/render so qcv_render needs no link to this library;
# qcv_window owns the sink through OutputBus.

add_library(qcv_output STATIC
    aac_encoder.cpp
    aac_encoder.h
    stream_sink.cpp
    stream_sink.h
    stream_muxer.cpp
    stream_muxer.h
    video_encoder.h
)

if(APPLE)
    target_sources(qcv_output PRIVATE
        vt_video_encoder.h
        vt_video_encoder.mm
    )
    set_source_files_properties(vt_video_encoder.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc")
    target_link_libraries(qcv_output PRIVATE
        "-framework VideoToolbox"
        "-framework CoreMedia"
        "-framework CoreVideo"
    )
elseif(WIN32)
    # Media Foundation on the renderer's D3D11 device (plan A2). The device
    # comes from qcv_render's D3D11DeviceManager; qcv_render links nothing
    # of ours back, so the dependency is one way.
    target_sources(qcv_output PRIVATE
        mf_video_encoder.h
        mf_video_encoder.cpp
    )
    target_link_libraries(qcv_output PRIVATE
        qcv_render
        mfplat.lib
        mfuuid.lib
        d3d11.lib
        dxguid.lib
        ole32.lib
        oleaut32.lib
    )
else()
    target_sources(qcv_output PRIVATE
        no_video_encoder.cpp
    )
endif()

target_include_directories(qcv_output
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(qcv_output
    PUBLIC
        Qt6::Core          # qInfo/qWarning
        qcv_decode         # qcbae::OutputRing::now_ns, FFmpeg (PUBLIC there)
)

# The vendored FFmpeg's headers first, as qcv_render does.
if(TARGET PkgConfig::FFMPEG)
    target_include_directories(qcv_output BEFORE PRIVATE
        $<TARGET_PROPERTY:PkgConfig::FFMPEG,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
