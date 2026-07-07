cmake_minimum_required(VERSION 3.22.1)
project(vk_font_android)

set(NATIVE_APP_GLUE ${ANDROID_NDK}/sources/android/native_app_glue)

# Platform-agnostic font engine (font.cc/glyphs.cc/msdf.cc/renderer.cc + vendored
# FreeType/msdfgen from third_party/) — builds vk_font_core, shared with desktop
# consumers (e.g. vk_canvas). See ../../../core/CMakeLists.txt.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../core ${CMAKE_BINARY_DIR}/vk_font_core)

# Slang shader compilation
set(SLANGC         "C:/VulkanSDK/1.4.341.1/Bin/slangc.exe")
set(SHADER_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders_src)
set(SHADER_OUT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/assets/shaders)

set(SHADERS
    composite_vert
    composite_frag
    tiling
    coverage
)

foreach(SHADER ${SHADERS})
    add_custom_command(
        OUTPUT  ${SHADER_OUT_DIR}/${SHADER}.spv
        COMMAND ${SLANGC} ${SHADER_SRC_DIR}/${SHADER}.slang
                          -o ${SHADER_OUT_DIR}/${SHADER}.spv
                          -target spirv
        DEPENDS ${SHADER_SRC_DIR}/${SHADER}.slang
        COMMENT "Compiling ${SHADER}.slang -> ${SHADER}.spv"
    )
    list(APPEND SPIRV_OUTPUTS ${SHADER_OUT_DIR}/${SHADER}.spv)
endforeach()

add_custom_target(compile_shaders ALL DEPENDS ${SPIRV_OUTPUTS})

# Android platform shell (window/event loop) + Android-only demo scene. The
# font/glyph/msdf/renderer logic itself lives in vk_font_core (core/).
add_library(vk_font SHARED
    cpp/main.cc
    cpp/app.cc
    cpp/demo.cc
    ${NATIVE_APP_GLUE}/android_native_app_glue.c
)

add_dependencies(vk_font compile_shaders)

target_include_directories(vk_font PRIVATE
    ${NATIVE_APP_GLUE}
    cpp/
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
