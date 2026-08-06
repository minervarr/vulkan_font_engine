#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

// ── The text-font seam ──────────────────────────────────────────────────────
//
// Two fonts implement this, and they differ in ONE decision: what a cell of the
// atlas contains.
//
//   MsdfFont   (msdf.hh)        — a multi-channel signed distance field, baked
//                                 once at a large reference size and rescaled
//                                 to any draw size by the shader.
//   RasterFont (raster_font.hh) — actual 8-bit coverage of the outline, baked
//                                 at the size it will be drawn at.
//
// MSDF's whole appeal is that one bake serves every size, which is exactly
// right for continuously-magnified text (vk_canvas's other consumers) and
// exactly wrong for a UI with four fixed sizes and a multi-script library: the
// distance field needs a wide margin around every glyph, so a CJK cell costs
// ~115x104 px and a 4096-square sheet runs out at ~1,300 glyphs. Matrix Player
// hit that ceiling and silently stopped baking Japanese and Korean entirely.
// Rasterizing per size costs ~26 px a cell instead, which is roughly 15x the
// capacity — see docs/superpowers/specs/2026-08-06-raster-glyph-cache-design.md
// in that repo.
//
// Everything above this seam — Canvas's layout/measure calls, Renderer's atlas
// upload, the glyph-quad vertex format, the shader's gamma-correct composite —
// is shared, because none of it depends on which of the two a cell holds.

// A laid-out glyph quad in screen px + normalised atlas UVs. draw=false for
// blanks (space) or missing glyphs.
struct GlyphQuad {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
  bool  draw = false;
};

enum class FontStyle : uint8_t { Roman = 0, Bold = 1, Math = 2, Italic = 3 };
inline constexpr int kFontStyleCount = 4;

// How the shader must read a cell. Passed through the MSDF pipeline's existing
// push constant; see shaders_src/msdf_frag.slang.
enum class TextMode : uint32_t { Mtsdf = 0, Raster = 1 };

class TextFont {
 public:
  static constexpr int FLOATS_PER_VERT = 8;
  static constexpr int VERTS_PER_GLYPH = 6;

  virtual ~TextFont() = default;

  // ── What Canvas needs (layout + measurement) ──────────────────────────────
  //
  // Every one of these takes sizePx. For MSDF that scales an em-unit plane box;
  // for raster it SELECTS the cell baked at that size. Same signature, and the
  // callers cannot tell the difference — which is the point.

  virtual float layout(uint32_t cp, float penX, float baselineY, float sizePx,
                       GlyphQuad& q, uint32_t prevCp = 0) const = 0;
  virtual float layoutByKey(uint32_t key, float penX, float baselineY,
                            float sizePx, GlyphQuad& q) const = 0;
  virtual float advance(uint32_t cp, float sizePx) const = 0;
  virtual float advanceKey(uint32_t key, float sizePx) const = 0;
  virtual float textWidth(std::string_view s, float sizePx) const = 0;
  virtual float lineHeight(float sizePx) const = 0;
  virtual float ascender(float sizePx) const = 0;

  // Resolve a codepoint to a glyph key in a given style. 0 if uncovered, which
  // is the caller's signal to fall back to the default face.
  virtual uint32_t keyForStyle(FontStyle s, uint32_t cp) const = 0;

  // Horizontal adjustment (EM, negative = tighten) to apply before drawing `cp`
  // when it follows `prevCp`. Baked from GPOS — see gpos_kern.hh for why
  // FreeType cannot supply this. EVERY path that advances a pen must apply it,
  // and textWidth() must agree with the emitters EXACTLY: a measure that
  // disagrees with the draw makes every centred and right-aligned label drift.
  virtual float kernEmStyled(FontStyle s, uint32_t prevCp, uint32_t cp) const = 0;

  virtual bool hasCodepoint(uint32_t cp) const = 0;

  // ── What Renderer needs (atlas upload) ────────────────────────────────────

  // Metrics usable for measuring/layout. Deliberately NOT tied to the CPU atlas
  // pixels being resident — MsdfFont frees those after the GPU upload.
  virtual bool valid() const = 0;
  virtual const std::vector<uint8_t>& atlas() const = 0;
  virtual uint32_t atlasW() const = 0;
  virtual uint32_t atlasH() const = 0;

  // Bytes per atlas texel: 4 (MSDF, RGBA) or 1 (raster, coverage). Chooses the
  // Vulkan format in MsdfTextRenderer::createResources.
  virtual uint32_t atlasChannels() const = 0;
  virtual TextMode textMode() const = 0;

  // Distance-field range in atlas texels. Meaningless for raster (0) — the
  // shader ignores it in that mode.
  virtual float distanceRange() const = 0;
  // True when the atlas alpha carries a real single-channel SDF. Always false
  // for raster.
  virtual bool isMtsdf() const = 0;
};
