# min_text_size (C++) — offline host tool that computes the minimum device-pixel
# size a font can be drawn at before its thinnest stroke drops below one
# rasterizable pixel. Pure font-outline geometry, no rendering comparison.
#
# Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
#
# HOST build (desktop), same pattern as ../atlas_gen and ../msdf_readability —
# links the vendored FreeType + msdfgen already present in the tree, no vcpkg
# / system packages, no Vulkan/windowing.
#
#   cmake -S . -B build
#   cmake --build build --config Release
#   build/min_text_size <font.otf> [pixelCriterion] [rasterEmPx]
#   build/min_text_size --emit-header <out.h> <pixelCriterion> <rasterEmPx> <font1.otf> [font2.otf ...]
cmake_minimum_required(VERSION 3.15)
project(min_text_size CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

get_filename_component(FREETYPE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../app/src/main/freetype" ABSOLUTE)
get_filename_component(MSDFGEN_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../app/src/main/msdfgen" ABSOLUTE)

set(FT_DISABLE_ZLIB     ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2    ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI   ON CACHE BOOL "" FORCE)
add_subdirectory("${FREETYPE_DIR}" "${CMAKE_BINARY_DIR}/freetype")

if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()

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

add_executable(min_text_size min_text_size.cc)

target_link_libraries(min_text_size PRIVATE
    msdfgen::msdfgen-core
    msdfgen::msdfgen-ext
    freetype)

if(MSVC)
    target_compile_options(min_text_size PRIVATE /utf-8)
endif()
