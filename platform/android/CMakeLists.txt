cmake_minimum_required(VERSION 3.22.1)
project(vk_font_android)

set(REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Platform-agnostic font engine (font/glyphs/msdf + curve_rasterizer/
# msdf_renderer + vendored FreeType/msdfgen from third_party/) — builds
# vk_font_core, shared with desktop consumers (e.g. vk_canvas).
add_subdirectory(${REPO_ROOT}/core ${CMAKE_BINARY_DIR}/vk_font_core)

# Slang shader compilation (shared sources at repo root; compiled .spv land in
# the Gradle module's assets/ so AGP packages them into the APK).
include(${REPO_ROOT}/cmake/VfeShaders.cmake)
vfe_compile_slang(compile_shaders
    ${CMAKE_CURRENT_SOURCE_DIR}/app/src/main/assets/shaders
    ${REPO_ROOT}/shaders_src
    composite_vert
    composite_frag
    tiling
    coverage
    msdf_vert
    msdf_frag
)

set(NATIVE_APP_GLUE ${ANDROID_NDK}/sources/android/native_app_glue)

# Android platform shell (window/event loop, AssetReader seam impl) +
# Android-only demo scene. The font/glyph/msdf/renderer logic itself lives in
# vk_font_core (core/).
add_library(vk_font SHARED
    main.cc
    app.cc
    demo.cc
    android_platform.cc
    ${NATIVE_APP_GLUE}/android_native_app_glue.c
)

add_dependencies(vk_font compile_shaders)

target_include_directories(vk_font PRIVATE
    ${NATIVE_APP_GLUE}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_options(vk_font PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>
    -O2
)

target_link_libraries(vk_font
    vk_font_core
    android
    log
    vulkan
)

# Keep the glue's NativeActivity entry point alive against linker GC.
target_link_options(vk_font PRIVATE -u ANativeActivity_onCreate)
