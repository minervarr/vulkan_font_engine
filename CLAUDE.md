# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build Commands

```bash
# Build debug APK
./gradlew assembleDebug

# Build release APK
./gradlew assembleRelease

# Install on connected device
./gradlew installDebug

# Clean build artifacts
./gradlew clean
```

**Prerequisites:**
- Android NDK 29.0.14206865 (set `ANDROID_NDK_HOME`)
- CMake 3.22.1+
- FreeType source — set `FREETYPE_SOURCE_DIR` in `CMakeLists.txt` (defaults to sibling directory `../whisper_cpp_ui_android/app/src/main/cpp/freetype-2.13.3/`)
- Slang compiler (`slangc.exe`) from Vulkan SDK 1.4.341.1 — required for recompiling shaders
- No tests to run; this is a demo app with no test framework

## Shader Compilation

Shaders are written in **Slang** (in `app/src/main/shaders_src/`) and compiled to SPIR-V (into `app/src/main/assets/shaders/`). CMake invokes `slangc.exe` automatically before the native library build. To recompile a shader manually:

```bash
slangc.exe <shader>.slang -target spirv -o <output>.spv
```

Shaders:
- `tiling.slang` — compute shader: assigns curves to 16×16 tiles
- `coverage.slang` — compute shader: rasterizes curves per tile into coverage image
- `composite_vert.slang` / `composite_frag.slang` — screen-quad vertex/fragment shaders for final composite

## Architecture

This app renders text entirely on the GPU using Vulkan compute shaders. FreeType decomposes font glyphs into cubic Bézier curves; those curves are uploaded to the GPU; a two-pass compute pipeline rasterizes them.

### Component Map

| File | Role |
|------|------|
| `main.cc` | Android NDK `android_main()` entry; event loop, window lifecycle, input |
| `app.hh/cc` | Vulkan bootstrap (instance → surface → device → swapchain → pipeline → render pass); orchestrates Renderer, Font, Demo |
| `renderer.hh/cc` | Manages GPU buffers and descriptor sets; dispatches tiling + coverage compute shaders; writes to output image |
| `font.hh/cc` | Wraps FreeType; converts glyphs to `CurveRecord` structs (cubic Béziers + metadata) |
| `glyphs.hh/cc` | Hardcoded fallback glyphs (digits/letters) used when FreeType is unavailable |
| `demo.hh/cc` | Builds the scene's curve list from font + screen dimensions; rebuilds on resize |

### Rendering Pipeline

1. **CPU side**: FreeType → `Font::emit()` → list of `CurveRecord` (max 8 192, 20 floats each)
2. **Tiling pass** (`tiling.slang`): Each curve writes itself into the tiles (16×16 px) it overlaps
3. **Coverage pass** (`coverage.slang`): Each tile integrates its curves to produce a coverage value
4. **Composite pass**: Fullscreen quad samples the coverage image and writes to the swapchain

### Key Constants (renderer.hh)
- Max curves: 8 192
- Floats per curve: 20
- Tile size: 16×16 pixels

## Project Configuration

- **App ID**: `io.nava.vkfont`
- **Native library**: `vk_font` (loaded by `NativeActivity`)
- **Min SDK**: 26 (Android 8.0); **Compile SDK**: 37
- **ABIs**: `arm64-v8a`, `x86_64`
- **C++ standard**: C++17, `-O2`
- Lint check `ExpiredTargetSdkVersion` is suppressed in `build.gradle`
