# msdf_readability (C++) — offline host tool that measures the smallest
# on-screen size an MSDF atlas bake (font + EM + distance range) still
# renders acceptably at, by comparing a CPU simulation of the real fragment
# shader against FreeType's own native antialiased rendering.
#
# Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
#
# HOST build (desktop), same pattern as ../atlas_gen — links the vendored
# FreeType + msdfgen already present in the tree, no vcpkg / system packages,
# no Vulkan/windowing (see msdf_readability.cc's header comment for why this
# stays a pure CPU tool).
#
#   cmake -S . -B build
#   cmake --build build --config Release
#   build/msdf_readability <font.otf> [em] [threshold] [minSize] [maxSize] [outDir]
cmake_minimum_required(VERSION 3.15)
project(msdf_readability CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(FREETYPE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/freetype" ABSOLUTE)
get_filename_component(MSDFGEN_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/msdfgen" ABSOLUTE)

# ── FreeType: build the vendored copy with no external codec deps. ──
set(FT_DISABLE_ZLIB     ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2    ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI   ON CACHE BOOL "" FORCE)
add_subdirectory("${FREETYPE_DIR}" "${CMAKE_BINARY_DIR}/freetype")

if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()

# ── msdfgen: core + ext (FreeType glyph loading), everything else off. ──
set(MSDFGEN_CORE_ONLY       OFF CACHE BOOL "" FORCE)
set(MSDFGEN_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_VCPKG       OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_OPENMP      OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_SKIA        OFF CACHE BOOL "" FORCE)
set(MSDFGEN_DISABLE_SVG     ON  CACHE BOOL "" FORCE)
set(MSDFGEN_DISABLE_PNG     ON  CACHE BOOL "" FORCE)
set(MSDFGEN_INSTALL         OFF CACHE BOOL "" FORCE)
set(MSDFGEN_DYNAMIC_RUNTIME ON  CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS       OFF CACHE BOOL "" FORCE)
add_subdirectory("${MSDFGEN_DIR}" "${CMAKE_BINARY_DIR}/msdfgen")

add_executable(msdf_readability msdf_readability.cc)

target_link_libraries(msdf_readability PRIVATE
    msdfgen::msdfgen-core
    msdfgen::msdfgen-ext
    freetype)

if(MSVC)
    target_compile_options(msdf_readability PRIVATE /utf-8)
endif()
