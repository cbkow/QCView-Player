# probe-linear-stage — CPU proof of the split OCIO chain (round trip vs the
# unsplit chain) and of the highlight knee. See main.cpp.

add_executable(probe-linear-stage
    main.cpp
)

target_link_libraries(probe-linear-stage
    PRIVATE
        qcv_color
)

target_compile_features(probe-linear-stage PRIVATE cxx_std_20)
