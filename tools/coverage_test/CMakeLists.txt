# coverage_test (C++) — CPU-side reference port of the tiling/coverage
# compute shaders' winding-fill rasterization, tested against hand-verified
# geometric ground truth. See coverage_test.cc's header comment for what
# exactly this tests and why.
#
# Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
#
# HOST build (desktop), same pattern as ../atlas_gen — but unlike the other
# tools/ programs, this one has NO FreeType/msdfgen dependency: it only
# exercises curve-record math and STL containers, no font/atlas I/O.
#
#   cmake -S . -B build
#   cmake --build build --config Release
#   build/coverage_test   (exit 0 = all pass, 1 = at least one failure)
cmake_minimum_required(VERSION 3.15)
project(coverage_test CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_executable(coverage_test coverage_test.cc)

if(MSVC)
    target_compile_options(coverage_test PRIVATE /utf-8)
endif()
