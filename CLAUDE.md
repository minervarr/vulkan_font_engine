# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Init submodules (first time)
git submodule update --init --recursive

# Android demo (Gradle project is self-contained in platform/android/)
cd platform/android
./gradlew assembleDebug      # debug APK
./gradlew assembleRelease    # release APK
./install.bat                # install on connected device
./gradlew clean              # clean build artifacts

# Host tools (Windows: vswhere -> vcvars64 -> Ninja -> cl)
pwsh tools/atlas_gen/build.ps1
pwsh tools/min_text_size/build.ps1
pwsh tools/msdf_readability/build.ps1

# Tests: CPU-side reference port of the tiling/coverage compute shaders,
# checked against hand-verified geometric ground truth (no GPU needed).
# Exits nonzero on any real failure.
pwsh tools/coverage_test/build.ps1
```

**Prerequisites:**
- Android NDK 29.0.14206865 (set `ANDROID_NDK_HOME`) — demo app only
- CMake 3.22.1+
- FreeType and msdfgen — vendored as submodules under `third_party/` (init submodules first)
- Slang compiler (`slangc`) from the Vulkan SDK — resolved from `$VULKAN_SDK` (override with `-DVFE_SLANGC=...`); required for recompiling shaders
- `tools/coverage_test` is the only test suite (host-only, no framework/deps beyond the STL) — everything else is manual/visual (the Android demo)

## Shader Compilation

Shaders are written in **Slang** in the repo-root `shaders_src/` and compiled to SPIR-V via `cmake/VfeShaders.cmake` (`vfe_compile_slang()`). The Android demo compiles them into `platform/android/app/src/main/assets/shaders/` (packaged into the APK). Six shaders:

- `tiling.slang` — compute: assigns curves to 16×16 tiles
- `coverage.slang` — compute: rasterizes curves per tile into a coverage image
- `composite_vert/frag.slang` — screen-quad composite of the coverage image
- `msdf_vert/frag.slang` — MSDF glyph-quad text pipeline

To recompile manually: `slangc <shader>.slang -target spirv -o <output>.spv`

## Architecture

One platform-agnostic engine core plus a thin Android demo backend:

```
core/                      # vk_font_core STATIC lib — no platform SDK includes
  asset_reader.hh          # THE platform seam: AssetReader (+ FileByteReader for desktop)
  font.* glyphs.* msdf.*   # FreeType wrapper / fallback glyphs / MSDF atlas + OpenType MATH
  gpu_util.*               # shared Vk helpers (find memory type, load shader module)
  curve_rasterizer.*       # CurveRasterizer: tiling+coverage compute rasterizer
  msdf_renderer.*          # MsdfTextRenderer: MSDF glyph-quad graphics pipeline
  log.hh utf8.hh           # logging shim, UTF-8 decoding
cmake/VfeShaders.cmake     # reusable slangc→SPIR-V compile function (vfe_compile_slang)
shaders_src/               # shared Slang sources (6 font shaders)
platform/
  android/                 # self-contained Gradle demo: gradlew, build.gradle,
                           #   settings.gradle, install/logcat/screenshot .bat,
                           #   CMake entry, main/app/demo .cc, android_platform.*
                           #   (AndroidAssetReader over AAssetManager), app/ module
third_party/
  freetype/  msdfgen/      # submodules — the only vendored copies
tools/                     # offline host tools (desktop, no Vulkan/windowing)
  atlas_gen/               # bakes font.msdf + atlas.rgba (OpenType MATH cracked by hand)
  min_text_size/           # min device-pixel size a font renders at (emits header for hosts)
  msdf_readability/        # smallest readable size of an MSDF bake vs FreeType reference
  coverage_test/           # CPU-side reference port of tiling.slang/coverage.slang, tested
                           #   against hand-verified geometry — the engine's only test suite
```

Rules of the structure:
- **Core never includes platform SDK headers.** Platform needs go through `core/asset_reader.hh` (`AssetReader::read`) — the engine's only seam; it never creates instances/surfaces/swapchains (hosts pass `VkDevice`/`VkPhysicalDevice` in). Android implements the seam in `platform/android/android_platform.{hh,cc}`; desktop hosts/tools can use the bundled `FileByteReader`.
- The two GPU units are independent classes: `CurveRasterizer` (curve compute path) and `MsdfTextRenderer` (MSDF text path), sharing only the `gpu_util` free functions. Vulkan handles are private; hosts talk to the narrow public API.
- The platform folder owns its CMake entry: `platform/android/CMakeLists.txt` (reached via `platform/android/app/build.gradle` → `externalNativeBuild.cmake.path "../CMakeLists.txt"`). It `add_subdirectory`s `core/`; there is no root CMakeLists.
- Consumers (e.g. vk_canvas) `add_subdirectory(<engine>/core)` and link `vk_font_core`, which PUBLIC-exposes the headers and the FreeType/msdfgen link chain.

## MSDF atlas pipeline (producer + consumer, both in this repo)

Two independent atlas paths exist — don't confuse them:

- **Offline-baked pair** (`font.msdf` + `atlas.rgba`, v3 binary): produced by
  `tools/atlas_gen/`, read by `MsdfFont::load()` (which accepts v2 or v3).
  v3 is MTSDF — alpha carries a true single-channel SDF, same as the runtime
  cache; v2 was RGB-only with constant-255 alpha. `isMtsdf()` reports true
  only for v3. Used by hosts that ship a pre-baked atlas (the Android demo's
  assets).
  - `pwsh tools/atlas_gen/build.ps1` → `tools/atlas_gen/build/atlas_gen.exe`; feed it
    4 OTFs (roman/bold/italic/math — the Latin Modern set is bundled in
    `tools/atlas_gen/fonts/`, GUST Font License) and it emits `font.msdf` + `atlas.rgba`.
    Density via `ATLAS_EM`/`ATLAS_AW` env vars.
  - The writer (`atlas_gen.cc`) and reader (`msdf.cc`) must stay byte-for-byte in sync
    on the format — bump the version word if it ever changes (v2→v3 changed only
    the version word and the meaning of the sheet's alpha bytes).

- **Runtime-generated single-file cache** (magic `'MSDF'`, currently **v9**):
  produced and consumed entirely by `msdf.cc` — `MsdfFont::generate(reader, fontPath,
  cachePath)` rasterizes via msdfgen on first run and `saveCache()`s the result;
  subsequent runs `loadCache()` it back (`generate()` tries the cache first,
  falls back to a fresh bake on any format mismatch — different version word,
  wrong file, etc.). `addStyle()`/`bakeCodepoints()` append more faces/fallback
  glyphs into the SAME atlas and must be re-saved by the caller if they added
  anything (`hasStyle()`/`hasCodepoint()` let a caller skip a redundant rebake
  after a cache hit that already covered it).
  - **v9 = v8 + bottom-anchored plane metadata and a denser bake** (EM 40→96,
    sheet width 2048→4096, packing fail-fasts if the sheet exceeds 4096px
    tall): plane boxes are derived from the raster's own bottom anchor
    (`-bounds.b`), so the per-glyph `ceil()` slack in the cell height no
    longer shifts ink upward relative to the metadata (was visible as
    baseline jitter between adjacent letters). The same anchoring fix is
    applied in `tools/atlas_gen` — the offline pair stays format v2 (byte
    layout unchanged, plane values corrected), so shipped `font.msdf`/
    `atlas.rgba` pairs must be regenerated.
  - **v8 = MTSDF**: generated via `msdfgen::generateMTSDF` (not `generateMSDF`) into
    a `Bitmap<float,4>` — RGB is the standard multi-channel field (median-of-3 for
    sharp corners), and **alpha now carries a true single-channel SDF** (previously
    a wasted constant 255). `MsdfFont::isMtsdf()` reports which kind is loaded —
    true for a v8/v9 cache, a fresh `generate()` bake, or an offline v3 pair;
    false only for a legacy v2 `load()` (whose alpha really is constant).
    Consumers gate any alpha-channel use on this flag. Renderer-side:
    `MsdfTextRenderer::createResources` snapshots it per weight (`mtsdf_[w]`)
    and `draw()` passes it as the MSDF push constant's `mtsdf` word through to
    `msdf_frag.slang`, which blends the primary
    `median(rgb)` edge toward the true SDF once the field spans under ~2 screen
    pixels — exactly where the median develops channel-conflict speckles on small
    text; the true SDF has none (its only tradeoff, slightly rounded corners, is
    invisible at that size).
  - **Memory**: `MsdfFont::releaseAtlasPixels()` frees the CPU-side atlas byte
    buffer once a `Renderer::initMsdf()` upload has consumed it (the atlas can be
    ~8 MB and is otherwise held for the process lifetime for no reason once it's
    on the GPU). `ensureAtlasLoaded(cachePath)` re-hydrates just the pixel bytes
    from the cache before any `addStyle()`/`bakeCodepoints()` call — appending to
    a released (empty) buffer would silently drop every existing glyph.

## Rendering Pipeline (curve path)

1. **CPU side**: FreeType → `Font::emitGlyph/emitString` → list of `CurveRecord` (max 8 192, 20 floats each)
2. **Tiling pass** (`tiling.slang`): each curve writes itself into the 16×16 px tiles it overlaps
3. **Coverage pass** (`coverage.slang`): each tile integrates its curves into a coverage value
4. **Composite pass**: fullscreen quad samples the coverage image into the swapchain

Key constants live on `CurveRasterizer` (`core/curve_rasterizer.hh`): `MAX_CURVES` 8 192, `CURVE_FLOATS` 20, `TILE_SIZE` 16, `MAX_WINDING_PER_TILE` 256 (must match `shaders_src/tiling.slang`/`coverage.slang`'s `MAX_PER_WIND_TILE` — see that file's comment: this is a per-tile-row-cell capacity on winding/glyph-fill curves, and silently drops registrations past it, which can flip fill parity for a busy scanline; `tools/coverage_test` proves this and documents that raising the cap mitigates but doesn't eliminate the risk).

## Project Configuration (Android demo)

- **App ID**: `io.nava.vkfont`
- **Native library**: `vk_font` (loaded by `NativeActivity`)
- **Min SDK**: 26 (Android 8.0); **Compile SDK**: 37
- **ABIs**: `arm64-v8a`, `x86_64`
- **C++ standard**: C++17, `-O2`
- Lint check `ExpiredTargetSdkVersion` is suppressed in `build.gradle`
