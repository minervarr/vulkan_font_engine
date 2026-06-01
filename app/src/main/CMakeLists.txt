cmake_minimum_required(VERSION 3.22.1)
project(vk_font_android)

set(NATIVE_APP_GLUE ${ANDROID_NDK}/sources/android/native_app_glue)

# FreeType source directory.
# Defaults to sibling whisper_cpp_ui_android project's thirdPartyCode_OnlyReference.
# Override with: cmake -DFREETYPE_SOURCE_DIR=/your/path/to/freetype-src
if(NOT DEFINED FREETYPE_SOURCE_DIR)
    set(FREETYPE_SOURCE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/freetype"
        CACHE PATH "Path to FreeType source directory")
endif()

set(FT_DISABLE_ZLIB      ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2     ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG       ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ  ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI    ON CACHE BOOL "" FORCE)
add_subdirectory(${FREETYPE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/freetype)

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

add_library(vk_font SHARED
    cpp/main.cc
    cpp/app.cc
    cpp/renderer.cc
    cpp/glyphs.cc
    cpp/font.cc
    cpp/demo.cc
    ${NATIVE_APP_GLUE}/android_native_app_glue.c
)

add_dependencies(vk_font compile_shaders)

target_include_directories(vk_font PRIVATE
    ${NATIVE_APP_GLUE}
    cpp/
    ${FREETYPE_SOURCE_DIR}/include
)

target_compile_options(vk_font PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=c++17>
    -O2
)

target_link_libraries(vk_font
    freetype
    android
    log
    vulkan
)
