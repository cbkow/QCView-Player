# probe-ocio-pins — the per-clip scene chain's routing rules on
# OCIOConfigManager (colour plan stage 2). See main.cpp.

add_executable(probe-ocio-pins
    main.cpp
)

target_link_libraries(probe-ocio-pins
    PRIVATE
        qcv_color
)

target_compile_features(probe-ocio-pins PRIVATE cxx_std_20)
