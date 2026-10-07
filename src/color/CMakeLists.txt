# color subsystem — OCIOConfigManager + (later) the Pass 2 OCIO
# display pipeline.
#
# Phase 2.1: OCIOConfigManager only. No rendering integration yet.
# Phase 2.2+: OCIO → QShaderBaker → QRhiGraphicsPipeline (Guide 14 §II D7).

qt_add_library(qcv_color STATIC
    ocio_config_manager.cpp
    ocio_config_manager.h
    ocio_chain_builder.cpp
    ocio_chain_builder.h
    ocio_lut_baker.cpp
    ocio_lut_baker.h
    preset_manager.cpp
    preset_manager.h
    # minColor engine (OCIO-free): the vendored minColorAE core + QCView's
    # chain / engine around it. See mincolor/VENDORED.md.
    mincolor_chain.cpp
    mincolor_chain.h
    mincolor_engine.cpp
    mincolor_engine.h
    mincolor/opendrt_presets.cpp
    mincolor/mincolor_agx.cpp
)

# The kernel sources as text, for the renderers to concatenate into their
# MSL / HLSL kernels at runtime (the same files the C++ twin compiles).
qt_add_resources(qcv_color "mincolor_kernels"
    PREFIX "/mincolor"
    BASE "${CMAKE_CURRENT_SOURCE_DIR}/mincolor"
    FILES
        mincolor/opendrt_shim_msl.h
        mincolor/opendrt_shim_hlsl.h
        mincolor/opendrt_params.h
        mincolor/opendrt_kernel.h
        mincolor/mincolor_knee.h
        mincolor/mincolor_agx.h
)

target_include_directories(qcv_color
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(qcv_color
    PUBLIC
        Qt6::Core
        # Phase 7.5 A.2.7: ocio_chain_builder.h exposes
        # OCIO::ConstGpuShaderDescRcPtr in its return type. Anything
        # linking qcv_color needs OCIO headers reachable.
        PkgConfig::OPENCOLORIO
)
