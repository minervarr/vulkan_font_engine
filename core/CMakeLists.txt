# Platform-agnostic font engine, built as the vk_font_core static library.
# No platform SDK headers here — Android/desktop asset loading goes through the
# AssetReader seam (asset_reader.hh), never AAssetManager directly.

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
    gpos_kern.cc
    glyph_raster.cc
    raster_font.cc
    glyph_baker.cc
    gpu_util.cc
    curve_rasterizer.cc
    msdf_renderer.cc
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

# ── Pure-logic tests (Debug-only, no framework) ─────────────────────────────
# Plain assert()-based exes matching core/tests/*.cc convention. gpos_kern_test
# links gpos_kern.cc ALONE — not vk_font_core — so the parser stays checkable
# without FreeType, msdfgen or Vulkan. Don't "simplify" it into linking the
# library; that coupling is exactly what the split avoids.
if(NOT ANDROID AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    add_executable(gpos_kern_test tests/gpos_kern_test.cc gpos_kern.cc)
    target_include_directories(gpos_kern_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(MSVC)
        target_compile_options(gpos_kern_test PRIVATE /W3 /utf-8)
    else()
        target_compile_options(gpos_kern_test PRIVATE -Wall)
    endif()

    # glyph_raster_test links glyph_raster.cc + FreeType and NOTHING else — no
    # msdfgen, no Vulkan, no RasterFont. Rasterizing an outline is the one part
    # of the raster text path that can be checked against arithmetic rather
    # than against a screenshot, and keeping it linkable alone is what makes
    # that possible. Same rule as gpos_kern_test above.
    add_executable(glyph_raster_test tests/glyph_raster_test.cc glyph_raster.cc)
    target_include_directories(glyph_raster_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(glyph_raster_test PRIVATE freetype)
    if(MSVC)
        target_compile_options(glyph_raster_test PRIVATE /W3 /utf-8)
    else()
        target_compile_options(glyph_raster_test PRIVATE -Wall)
    endif()

    # area_raster_test is the quality gate for the GPU rasterizer: it measures
    # area_raster.hh's coverage against FreeType's, glyph by glyph. Same
    # linkage rule as glyph_raster_test — glyph_raster.cc + FreeType, nothing
    # else — because the whole point is to compare two rasterizers with no
    # engine, no Vulkan and no atlas in between.
    add_executable(area_raster_test tests/area_raster_test.cc glyph_raster.cc)
    target_include_directories(area_raster_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(area_raster_test PRIVATE freetype)
    if(MSVC)
        target_compile_options(area_raster_test PRIVATE /W3 /utf-8)
    else()
        target_compile_options(area_raster_test PRIVATE -Wall)
    endif()

    # raster_font_test measures the CACHE, not a rasterizer: how far a drawn
    # glyph lands from where the pen said, which is the text path's positional
    # fidelity in one number. It therefore needs the real RasterFont, so unlike
    # the two above it links raster_font.cc + gpos_kern.cc as well.
    add_executable(raster_font_test tests/raster_font_test.cc
                   raster_font.cc glyph_raster.cc gpos_kern.cc)
    target_include_directories(raster_font_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(raster_font_test PRIVATE freetype)
    if(MSVC)
        target_compile_options(raster_font_test PRIVATE /W3 /utf-8)
    else()
        target_compile_options(raster_font_test PRIVATE -Wall)
    endif()

    # cell_key_test and shelf_packer_test link NOTHING AT ALL — both headers
    # are self-contained by design, and that is what lets the two pieces the
    # raster cache is built on be checked as arithmetic instead of as a
    # screenshot. Both replaced hand-written bit and shelf bookkeeping that was
    # silently wrong; see the headers' own comments. Keep them dependency-free.
    foreach(pure_test cell_key_test shelf_packer_test)
        add_executable(${pure_test} tests/${pure_test}.cc)
        target_include_directories(${pure_test} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        if(MSVC)
            target_compile_options(${pure_test} PRIVATE /W3 /utf-8)
        else()
            target_compile_options(${pure_test} PRIVATE -Wall)
        endif()
    endforeach()
endif()
