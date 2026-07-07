# Platform-agnostic font engine, built as the vk_font_core static library.
# No platform SDK headers here — Android/desktop asset loading goes through the
# AssetLoader callback (asset_loader.hh), never AAssetManager directly.

set(THIRD_PARTY_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party)

set(FT_DISABLE_ZLIB      ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2     ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG       ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ  ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI    ON CACHE BOOL "" FORCE)
add_subdirectory(${THIRD_PARTY_DIR}/freetype ${CMAKE_BINARY_DIR}/freetype)

# msdfgen looks for a Freetype::Freetype target; this FreeType only exports the
# plain `freetype` target, so alias it before pulling msdfgen in.
if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()

set(MSDFGEN_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_VCPKG        OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_SKIA         OFF CACHE BOOL "" FORCE)
set(MSDFGEN_DISABLE_SVG      ON  CACHE BOOL "" FORCE)
set(MSDFGEN_DISABLE_PNG      ON  CACHE BOOL "" FORCE)
set(MSDFGEN_INSTALL          OFF CACHE BOOL "" FORCE)
# Match the /MD (dynamic CRT) the rest of this build uses — CMake's default for
# MSVC — so msdfgen's static libs don't LNK2038 against consumers built /MD.
set(MSDFGEN_DYNAMIC_RUNTIME  ON  CACHE BOOL "" FORCE)
add_subdirectory(${THIRD_PARTY_DIR}/msdfgen ${CMAKE_BINARY_DIR}/msdfgen)

add_library(vk_font_core STATIC
    font.cc
    glyphs.cc
    msdf.cc
    renderer.cc
)

target_include_directories(vk_font_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_features(vk_font_core PUBLIC cxx_std_17)
if(MSVC)
    target_compile_options(vk_font_core PRIVATE /utf-8)
else()
    target_compile_options(vk_font_core PRIVATE -O2)
endif()

# msdfgen-core linked explicitly (msdfgen-ext links it only PRIVATE): its PUBLIC
# `MSDFGEN_PUBLIC=` define makes msdfgen/core/base.h skip the generated
# msdfgen-config.h, which only exists under MSDFGEN_INSTALL.
target_link_libraries(vk_font_core PUBLIC
    freetype
    msdfgen::msdfgen-ext
    msdfgen::msdfgen-core
)

if(ANDROID)
    target_link_libraries(vk_font_core PUBLIC android log)
else()
    find_package(Vulkan REQUIRED)
    target_link_libraries(vk_font_core PUBLIC Vulkan::Vulkan)
endif()
