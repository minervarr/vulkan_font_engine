#pragma once
#include "asset_reader.hh"
#include "glyph_raster.hh"
#include "gpos_kern.hh"
#include "text_font.hh"

#include <cstdint>
#include <memory>
#include <set>
#include <unordered_map>
#include <vector>

// ── A per-size rasterized glyph cache ───────────────────────────────────────
//
// The other TextFont. Where MsdfFont bakes one distance field per glyph and
// lets the shader rescale it, this bakes actual coverage at each size the UI
// draws at. See text_font.hh for why a UI with a handful of fixed sizes and a
// multi-script library wants the second thing.
//
// Three properties are load-bearing:
//
// 1. **Cells are keyed (style, sizePx, codepoint), with sizePx an INTEGER.**
//    Quantizing at the key is what stops two sizes a hundredth of a pixel
//    apart from each baking their own copy of the library. It also pairs with
//    the whole-pixel pen snapping below: a cell drawn at the size it was baked
//    at, on a pixel boundary, is exact.
//
// 2. **layout() is const and never bakes.** Canvas holds a `const TextFont*`
//    and draws from the render thread; baking there would mean growing the
//    atlas mid-frame while the GPU reads it. Baking happens only in
//    ensureGlyphs(), which the app calls when the library or the UI scale
//    changes, followed by a Renderer::initMsdf() re-upload.
//
// 3. **There is no disk cache, on purpose.** Rasterizing is 10-50us a glyph
//    against MTSDF's 1-10ms, so the few thousand cells this app needs cost
//    well under a second at startup. MTSDF's cache exists to avoid that cost;
//    without the cost there is no cache, no version word, no fingerprinted
//    filename, and no way to serve a stale bake.

class RasterFont : public TextFont {
 public:
  // Atlas sheet width. Height grows in shelves as glyphs are added, and is
  // held under the 4096 every Vulkan implementation guarantees.
  static constexpr uint32_t kAtlasW   = 4096;
  static constexpr uint32_t kAtlasMaxH = 4096;

  // ── Setup ────────────────────────────────────────────────────────────────

  // The default face. Must be called first: it supplies the vertical metrics
  // every size derives from.
  bool open(AssetReader& reader, const char* fontPath);

  // A named face (Bold/Italic/Math). Only the codepoints it genuinely covers
  // are registered for it; anything else falls through to the default face,
  // matching how Canvas::textStyled() already behaves on a keyForStyle() miss.
  bool addStyle(AssetReader& reader, const char* fontPath, FontStyle style);

  // Script-coverage faces (CJK/Hangul/Kana) and the icon font, consulted in
  // registration order for codepoints the default face lacks. This is the
  // raster counterpart of MsdfFont::bakeCodepoints()'s fallback chain, and it
  // feeds the DEFAULT face's table for the same reason: styled text then picks
  // the glyphs up for free.
  bool addFallback(AssetReader& reader, const char* fontPath);

  // A face that claims its own codepoints AHEAD of the primary and every
  // style — the icon font, and anything else mapped into the Private Use
  // Area.
  //
  // This is not symmetry for its own sake. New Computer Modern maps U+E000
  // onward to its own private-use ligature glyphs (f_b, f_f_h, f_f_j, ...),
  // so an icon font registered as an ordinary fallback never gets asked: the
  // primary face answers first and the transport bar draws "ffh" where the
  // next-track icon should be. PUA assignments are by definition private, so
  // two faces disagreeing about them is normal and the consumer has to say
  // which one it means.
  bool addOverride(AssetReader& reader, const char* fontPath);

  bool hasStyle(FontStyle style) const;

  // ── Baking ───────────────────────────────────────────────────────────────

  // Bake every registered face's coverage of `cps`, at every size in
  // `sizesPx` plus every size requested by a previous call. Already-present
  // cells are skipped, so calling this again after a rescan costs only what is
  // genuinely new. Returns the number of cells added.
  //
  // The caller must re-run Renderer::initMsdf() afterwards if this returns
  // nonzero — the atlas it grew is still only on the CPU.
  int ensureGlyphs(const std::vector<uint32_t>& cps,
                   const std::vector<int>& sizesPx);

  // ── Misses, and why they are the mechanism rather than a safety net ──────
  //
  // A per-size cache has to know its sizes, and the app does not fully know
  // them: the four type roles are enumerable, but icon boxes are derived from
  // layout geometry, and a second window draws at its own scale. Guessing a
  // size ladder wide enough to cover all of it would bake thousands of cells
  // nothing ever draws.
  //
  // So layout() RECORDS what it was asked for and did not have, instead of
  // silently drawing nothing. The host bakes the misses after the frame and
  // re-uploads; the glyph is missing for exactly one frame the first time it
  // appears at a new size, and never again. Scale changes, a newly opened
  // window and a redesigned icon box all resolve themselves the same way,
  // with no size list to keep in sync.
  //
  // Recording happens on the draw path, so it is const — see misses_. Layout
  // and baking must not run concurrently; this app draws both windows from one
  // thread.
  bool hasMisses() const { return !misses_.empty(); }

  // Bake everything recorded, clear the record, and report how many cells were
  // added. Codepoints no registered face can serve are remembered as such so
  // they are not retried every frame forever.
  int bakeMisses();

  size_t cellCount() const { return cells_.size(); }

  // ── TextFont ─────────────────────────────────────────────────────────────

  float layout(uint32_t cp, float penX, float baselineY, float sizePx,
               GlyphQuad& q, uint32_t prevCp = 0) const override;
  float layoutByKey(uint32_t key, float penX, float baselineY, float sizePx,
                    GlyphQuad& q) const override;
  float advance(uint32_t cp, float sizePx) const override;
  float advanceKey(uint32_t key, float sizePx) const override;
  float textWidth(std::string_view s, float sizePx) const override;
  float lineHeight(float sizePx) const override { return lineHeightEm_ * sizePx; }
  float ascender(float sizePx) const override   { return ascenderEm_   * sizePx; }

  uint32_t keyForStyle(FontStyle s, uint32_t cp) const override;
  float kernEmStyled(FontStyle s, uint32_t prevCp, uint32_t cp) const override;
  bool  hasCodepoint(uint32_t cp) const override;

  bool valid() const override { return !cells_.empty() && atlasH_ > 0; }
  const std::vector<uint8_t>& atlas() const override { return atlas_; }
  uint32_t atlasW() const override { return atlasW_; }
  uint32_t atlasH() const override { return atlasH_; }
  uint32_t atlasChannels() const override { return 1; }   // 8-bit coverage
  TextMode textMode() const override { return TextMode::Raster; }
  float distanceRange() const override { return 0.0f; }   // no field to range
  bool  isMtsdf() const override { return false; }

  // Emit one glyph's quad at the pen. Mirrors MsdfFont::emitGlyph so the
  // Canvas text path is identical for both fonts.
  float emitGlyph(std::vector<float>& out, uint32_t cp, float penX,
                  float baselineY, float sizePx,
                  float r, float g, float b, float a,
                  uint32_t prevCp = 0) const;

 private:
  // A glyph baked at one size, placed in the sheet. Sizes are in whole pixels
  // because the rasterizer works in whole pixels.
  struct Cell {
    bool  hasGlyph = false;   // false = whitespace or missing: advance only
    float advance  = 0.0f;    // px, at this cell's size
    int   bearingX = 0, bearingY = 0;
    int   w = 0, h = 0;
    uint32_t atlasX = 0, atlasY = 0;
  };

  // A face plus what it covers and how it kerns.
  struct Face {
    vfe::RasterFace     raster;
    vfe::KernTable      kern;
    std::vector<uint32_t> cps;   // registered coverage, for ensureGlyphs
  };

  // key = style<<56 | sizePx<<32 | codepoint. The pieces never collide:
  // codepoints stop at 0x10FFFF and a UI size fits in a few bits of the
  // middle word.
  static uint64_t cellKey(FontStyle s, int sizePx, uint32_t cp) {
    return ((uint64_t)(uint8_t)s << 56) | ((uint64_t)(uint32_t)sizePx << 32) | cp;
  }
  // The glyph key Canvas passes back through layoutByKey(). Style is offset by
  // one so a real key is never 0, which keyForStyle() uses to mean "uncovered".
  static uint32_t glyphKey(FontStyle s, uint32_t cp) {
    return ((uint32_t)((uint8_t)s + 1) << 24) | (cp & 0x00FFFFFFu);
  }
  static FontStyle keyStyle(uint32_t key) {
    return (FontStyle)(uint8_t)((key >> 24) - 1);
  }
  static uint32_t keyCp(uint32_t key) { return key & 0x00FFFFFFu; }

  // The one place a float size becomes a cell size. Everything that touches
  // the cache goes through it, so measurement and drawing cannot disagree
  // about which bake they mean.
  static int quantize(float sizePx) {
    int px = (int)(sizePx + 0.5f);
    return px < 1 ? 1 : px;
  }

  const Cell* find(FontStyle s, int sizePx, uint32_t cp) const {
    const uint64_t k = cellKey(s, sizePx, cp);
    auto it = cells_.find(k);
    if (it != cells_.end()) return &it->second;
    if (!unservable_.count(k)) misses_.insert(k);
    return nullptr;
  }

  // Which face serves `cp` for `style`: the style's own face if it covers it,
  // else the default face, else the fallbacks in order. nullptr if nothing has
  // it. Also reports which kern table applies.
  const Face* faceFor(FontStyle style, uint32_t cp) const;

  bool bakeCell(const Face& f, FontStyle style, int sizePx, uint32_t cp);
  bool packInto(int w, int h, uint32_t& outX, uint32_t& outY);

  std::unique_ptr<Face> styles_[kFontStyleCount];
  std::vector<std::unique_ptr<Face>> overrides_;   // consulted before styles_
  std::vector<std::unique_ptr<Face>> fallbacks_;   // consulted after

  std::unordered_map<uint64_t, Cell> cells_;
  std::set<int> sizes_;              // every size ever requested

  // Cell keys asked for and absent (mutable: recorded from the const draw
  // path — see hasMisses()), and the ones no face can ever supply, so a
  // genuinely uncoverable codepoint costs one lookup rather than a bake
  // attempt every frame.
  mutable std::set<uint64_t> misses_;
  std::set<uint64_t>         unservable_;

  std::vector<uint8_t> atlas_;       // R8 coverage, atlasW_ * atlasH_
  uint32_t atlasW_ = kAtlasW, atlasH_ = 0;
  uint32_t shelfX_ = 0, shelfY_ = 0, shelfH_ = 0;

  float ascenderEm_ = 0.0f, descenderEm_ = 0.0f, lineHeightEm_ = 1.2f;
};
