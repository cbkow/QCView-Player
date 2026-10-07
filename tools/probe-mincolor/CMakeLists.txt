# probe-mincolor — CPU proof of the minColor engine: the vendored core
# through QCView's chain resolver on known values. See main.cpp.

add_executable(probe-mincolor
    main.cpp
)

target_link_libraries(probe-mincolor
    PRIVATE
        qcv_color
)

target_compile_features(probe-mincolor PRIVATE cxx_std_20)
