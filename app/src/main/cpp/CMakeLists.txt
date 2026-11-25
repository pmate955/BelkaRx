cmake_minimum_required(VERSION 3.22.1)

project("belkarx")

add_library(${CMAKE_PROJECT_NAME} SHARED
    native-lib.cpp
    ColorScales.cpp
    DspHelpers.cpp
    RenderHelpers.cpp
    GlWaterfall.cpp)

target_link_libraries(${CMAKE_PROJECT_NAME}
    android
    log
    jnigraphics
    EGL
    GLESv3)

# Optimize the native DSP/render code in every build type. Debug builds otherwise compile with
# -O0, which makes the FFT ~10-30x slower. Added after the build-type flags, so it wins; -g stays.
target_compile_options(${CMAKE_PROJECT_NAME} PRIVATE -O2)
