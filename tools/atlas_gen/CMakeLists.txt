# atlas_gen (C++) — offline host tool that bakes the MSDF font atlas + font.msdf.
#
# Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
#
# This is a HOST build (desktop), not part of the Android app. It links the vendored
# FreeType (for glyph outlines/metrics) and the msdfgen library (for the MSDF), both
# already present in the tree — no vcpkg / system packages required.
#
#   cmake -S . -B build
#   cmake --build build --config Release
#   build/Release/atlas_gen  <roman.otf> <bold.otf> <italic.otf> <math.otf> out.msdf out.rgba
cmake_minimum_required(VERSION 3.15)
project(atlas_gen CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Normalise the in-tree dependency locations (relative to this file). Both live
# in this same engine repo: the vendored FreeType submodule and vendored msdfgen.
get_filename_component(FREETYPE_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../app/src/main/freetype" ABSOLUTE)
get_filename_component(MSDFGEN_DIR
    "${CMAKE_CURRENT_SOURCE_DIR}/../../app/src/main/msdfgen" ABSOLUTE)

# ── FreeType: build the vendored copy with no external codec deps. ──
set(FT_DISABLE_ZLIB     ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2    ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG      ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI   ON CACHE BOOL "" FORCE)
add_subdirectory("${FREETYPE_DIR}" "${CMAKE_BINARY_DIR}/freetype")

# msdfgen looks for a Freetype::Freetype target; this FreeType version only exports
# the plain `freetype` target, so provide the alias before pulling msdfgen in.
if(NOT TARGET Freetype::Freetype)
    add_library(Freetype::Freetype ALIAS freetype)
endif()

# ── msdfgen: core + ext (FreeType glyph loading), everything else off. ──
set(MSDFGEN_CORE_ONLY       OFF CACHE BOOL "" FORCE)
set(MSDFGEN_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_VCPKG       OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_OPENMP      OFF CACHE BOOL "" FORCE)
set(MSDFGEN_USE_SKIA        OFF CACHE BOOL "" FORCE)  # default ON; would need find_package(skia)
set(MSDFGEN_DISABLE_SVG     ON  CACHE BOOL "" FORCE)  # no tinyxml2
set(MSDFGEN_DISABLE_PNG     ON  CACHE BOOL "" FORCE)  # no libpng
set(MSDFGEN_INSTALL         OFF CACHE BOOL "" FORCE)
set(MSDFGEN_DYNAMIC_RUNTIME ON  CACHE BOOL "" FORCE)  # match FreeType + this exe (/MD), avoids LNK2038
set(BUILD_SHARED_LIBS       OFF CACHE BOOL "" FORCE)
add_subdirectory("${MSDFGEN_DIR}" "${CMAKE_BINARY_DIR}/msdfgen")

add_executable(atlas_gen
    atlas_gen.cc
    math_table.cc)

# msdfgen-core/ext export their root as an include dir, and freetype exports its
# include dir, so linking is enough for <msdfgen.h>, <msdfgen-ext.h>, <ft2build.h>.
target_link_libraries(atlas_gen PRIVATE
    msdfgen::msdfgen-core
    msdfgen::msdfgen-ext
    freetype)

if(MSVC)
    target_compile_options(atlas_gen PRIVATE /utf-8)
endif()
