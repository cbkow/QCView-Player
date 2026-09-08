# bench-decode — decode-throughput bench for the app's FFmpeg paths.
#
# 2026-09-08 — sw vs Vulkan-hwaccel (FFmpeg-managed pool, pre-I.E) vs
# Vulkan-hwaccel with the app-owned cached pool (Phase I.E), on the
# same shared VkDevice and helpers the app uses. Optional readback
# (D3D11VA-style cost model) and decoder reopen every K frames
# (playlist-boundary model). Windows-only today: the Vulkan device
# helper is gated on Q_OS_WIN. See the header comment in main.cpp.

add_executable(bench-decode
    main.cpp
)

target_link_libraries(bench-decode
    PRIVATE
        qcv_decode
        PkgConfig::FFMPEG
        Qt6::Core
)

target_include_directories(bench-decode PRIVATE ${CMAKE_SOURCE_DIR}/src/decode)

target_compile_features(bench-decode PRIVATE cxx_std_20)
