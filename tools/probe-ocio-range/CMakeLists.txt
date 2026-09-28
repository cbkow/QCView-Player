# probe-ocio-range — sweeps every Input × Display × View of an OCIO config
# with super-white, sub-black and out-of-gamut values and reports NaN and
# clamping per Input. See main.cpp.

add_executable(probe-ocio-range
    main.cpp
)

target_link_libraries(probe-ocio-range
    PRIVATE
        PkgConfig::OPENCOLORIO
)

target_compile_features(probe-ocio-range PRIVATE cxx_std_20)
