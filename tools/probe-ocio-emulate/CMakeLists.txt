# probe-ocio-emulate — host-display emulation in the chain builder and the
# host-display pin on OCIOConfigManager (2026-10-27). See main.cpp.

add_executable(probe-ocio-emulate
    main.cpp
)

target_link_libraries(probe-ocio-emulate
    PRIVATE
        qcv_color
)

target_compile_features(probe-ocio-emulate PRIVATE cxx_std_20)
