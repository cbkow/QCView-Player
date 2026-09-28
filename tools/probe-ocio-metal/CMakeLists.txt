# probe-ocio-metal — GPU check of MetalOcioRenderer's split chain against
# the CPU reference. macOS only. See main.mm.

add_executable(probe-ocio-metal
    main.mm
    ${CMAKE_SOURCE_DIR}/src/render/metal/metal_ocio_renderer.mm
    ${CMAKE_SOURCE_DIR}/src/render/metal/metal_device_manager.mm
)

target_include_directories(probe-ocio-metal PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_compile_options(probe-ocio-metal PRIVATE -fobjc-arc)

target_link_libraries(probe-ocio-metal
    PRIVATE
        qcv_color
        Qt6::Gui
        "-framework Metal"
        "-framework QuartzCore"
        "-framework AppKit"
)

target_compile_features(probe-ocio-metal PRIVATE cxx_std_20)
