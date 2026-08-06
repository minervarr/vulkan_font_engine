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

// A laid-out glyph quad in screen px + atlas UVs normalised WITHIN ITS PAGE.
// draw=false for blanks (space) or missing glyphs.
struct GlyphQuad {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
  // Which atlas page (array layer) the cell lives on. A font with a single
  // sheet leaves this 0 and never thinks about it again.
  uint32_t page = 0;
  bool  draw = false;
};

enum class FontStyle : uint8_t { Roman = 0, Bold = 1, Math = 2, Italic = 3 };
inline constexpr int kFontStyleCount = 4;

// How the shader must read a cell. Passed through the MSDF pipeline's existing
// push constant; see shaders_src/msdf_frag.slang.
enum class TextMode : uint32_t { Mtsdf = 0, Raster = 1 };

class TextFont {
 public:
  // x, y, u, v, r, g, b, a, page. The page rides along per vertex rather than
  // as a per-draw push constant because the whole frame's text is ONE draw
  // call with one flat vertex buffer — batching it by page would mean
  // inventing batching that does not exist.
  static constexpr int FLOATS_PER_VERT = 9;
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
  // An always-opaque texel in this font's atlas, if it has one.
  //
  // Canvas::quadMsdfRect() draws a solid rectangle through the text pipeline
  // and needs a texel that samples to full coverage. It used to find one by
  // laying out 'I' at a HARDCODED 100 px and taking the middle of the glyph —
  // which is a reasonable trick for a distance field baked once for all sizes,
  // and a trap for a per-size cache: it requests a 100 px cell that nothing
  // else will ever draw, and (before the miss path stopped adopting sizes)
  // dragged the entire codepoint set into a 100 px bake behind it.
  //
  // A font that can reserve a solid texel says so here. MsdfFont does not, and
  // keeps the 'I' probe, where it costs nothing.
  virtual bool solidTexel(float& u, float& v) const { (void)u; (void)v; return false; }

  virtual bool valid() const = 0;
  virtual const std::vector<uint8_t>& atlas() const = 0;
  virtual uint32_t atlasW() const = 0;
  virtual uint32_t atlasH() const = 0;

  // Bytes per atlas texel: 4 (MSDF, RGBA) or 1 (raster, coverage). Chooses the
  // Vulkan format in MsdfTextRenderer::createResources.
  virtual uint32_t atlasChannels() const = 0;

  // How many pages atlas() holds, laid out one after another (page-major), all
  // atlasW() x atlasH(). They become the layers of one 2D array image, so the
  // consumer binds a single descriptor whatever this returns.
  //
  // One sheet is the ordinary case and stays the default: a distance-field
  // atlas is baked once for every size and does not grow with resolution, so
  // MsdfFont has never needed a second page and does not implement this.
  virtual uint32_t atlasPages() const { return 1; }

  // True when the atlas pixels live only on the GPU and atlas() is empty.
  // The consumer then allocates the image and uploads nothing — something
  // else writes it. See GlyphBaker.
  virtual bool atlasOnGpu() const { return false; }

  // Pages whose bytes have changed since this was last called, and clears the
  // record. The consumer re-uploads exactly these instead of the whole atlas.
  //
  // The default reports EVERY page, which is always correct and is what a font
  // that does not track its own writes should say. RasterFont tracks precisely,
  // because it is the one that grows: a lazily-baked glyph used to cost a
  // vkDeviceWaitIdle, a teardown of the image, view, sampler, descriptor pool
  // and PIPELINE, and a re-upload of every byte — for a handful of new cells on
  // one page, in the middle of a frame.
  virtual void takeDirtyPages(std::vector<uint32_t>& out) const {
    out.clear();
    for (uint32_t p = 0; p < atlasPages(); ++p) out.push_back(p);
  }
  virtual TextMode textMode() const = 0;

  // Distance-field range in atlas texels. Meaningless for raster (0) — the
  // shader ignores it in that mode.
  virtual float distanceRange() const = 0;
  // True when the atlas alpha carries a real single-channel SDF. Always false
  // for raster.
  virtual bool isMtsdf() const = 0;
};
