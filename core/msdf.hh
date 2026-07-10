#pragma once
#include "asset_reader.hh"
#include <cstdint>
#include <unordered_map>
#include <vector>
#include <string_view>

// MSDF font: loads a compact metrics blob (font.msdf) + a raw RGBA atlas
// (atlas.rgba) produced offline by msdf-atlas-gen. Lays out text into textured
// quads that a single MSDF shader rasterises — replacing the per-pixel Bézier
// coverage pass for text, which is what made scrolling/typing expensive.
//
// Vertex layout emitted per glyph (6 verts = 2 triangles), 8 floats each:
//   pos.x pos.y  uv.x uv.y  r g b a   (pos in screen px, uv in [0,1])
struct MsdfGlyph {
  float advance = 0.0f;
  float italic  = 0.0f;  // math italic correction (em); 0 for text glyphs
  bool  hasGlyph = false;
  float planeL = 0, planeT = 0, planeR = 0, planeB = 0;  // em units (yOrigin top)
  float atlasL = 0, atlasT = 0, atlasR = 0, atlasB = 0;  // atlas pixels
};

// ── OpenType MATH data (font.msdf v2) ──────────────────────────────────────
// Baked offline by atlas_gen from the math font's MATH table. All lengths are in
// EM units (multiply by sizePx). Glyphs are addressed by `key` = (fontId<<24)|gid;
// fontId 0 = text face, 2 = math face. Variant/assembly part glyphs have no
// codepoint and are reached only through a MathConstruction.

// MathConstants subset, 42 values in the baker's fixed order. Named accessors map
// to the index so callers read intent, not magic numbers.
struct MathConstants {
  float v[42] = {};
  // scaling
  float scriptPercentScaleDown()       const { return v[0]; }
  float scriptScriptPercentScaleDown() const { return v[1]; }
  float displayOperatorMinHeight()     const { return v[2]; }
  float axisHeight()                   const { return v[3]; }
  // scripts
  float subscriptShiftDown()           const { return v[5]; }
  float subscriptTopMax()              const { return v[6]; }
  float superscriptShiftUp()           const { return v[7]; }
  float superscriptShiftUpCramped()    const { return v[8]; }
  float superscriptBottomMin()         const { return v[9]; }
  float subSuperscriptGapMin()         const { return v[10]; }
  float spaceAfterScript()             const { return v[11]; }
  // fractions
  float fractionNumeratorShiftUp()              const { return v[22]; }
  float fractionNumeratorDisplayStyleShiftUp()  const { return v[23]; }
  float fractionDenominatorShiftDown()          const { return v[24]; }
  float fractionDenominatorDisplayStyleShiftDown() const { return v[25]; }
  float fractionNumeratorGapMin()               const { return v[26]; }
  float fractionNumDisplayStyleGapMin()         const { return v[27]; }
  float fractionRuleThickness()                 const { return v[28]; }
  float fractionDenominatorGapMin()             const { return v[29]; }
  float fractionDenomDisplayStyleGapMin()       const { return v[30]; }
  // bars
  float overbarVerticalGap()    const { return v[31]; }
  float overbarRuleThickness()  const { return v[32]; }
  float overbarExtraAscender()  const { return v[33]; }
  // radicals
  float radicalVerticalGap()             const { return v[34]; }
  float radicalDisplayStyleVerticalGap() const { return v[35]; }
  float radicalRuleThickness()           const { return v[36]; }
  float radicalExtraAscender()           const { return v[37]; }
  float radicalKernBeforeDegree()        const { return v[38]; }
  float radicalKernAfterDegree()         const { return v[39]; }
  float radicalDegreeBottomRaisePercent() const { return v[40]; }
  // assembly
  float minConnectorOverlap()            const { return v[41]; }
};

// Named font styles baked into the one atlas; the id is also the high byte of a
// glyph key = (style<<24)|gid. Roman is the default text face; Math carries the
// variables (math-italic), Greek, operators, delimiters and radicals.
enum class FontStyle : uint8_t { Roman = 0, Bold = 1, Math = 2, Italic = 3 };
inline constexpr int kFontStyleCount = 4;

struct MathVariant { uint32_t key = 0; float advance = 0.0f; };          // em
struct MathPart    { uint32_t key = 0; float start = 0, end = 0, full = 0; bool ext = false; }; // em
struct MathConstruction {
  uint32_t baseKey = 0;
  float    italic  = 0.0f;
  std::vector<MathVariant> variants;  // ascending advance (pre-built sizes)
  std::vector<MathPart>    parts;     // bottom→top assembly recipe
};

// One part placed in a stretched vertical assembly: glyph `key` whose bottom sits
// `bottomEm` above the assembly's bottom (em), occupying `fullEm` of height.
struct PlacedPart { uint32_t key = 0; float bottomEm = 0; float fullEm = 0; };
// Result of stretching a growable glyph to a target height. `single` → draw the
// one variant glyph `key`; else stack `parts` bottom→top. `heightEm` is the
// achieved height (≥ target when possible).
struct VStretch {
  float heightEm = 0;
  bool  single   = true;
  uint32_t key   = 0;
  std::vector<PlacedPart> parts;
};

// A laid-out glyph quad in screen px + normalised atlas UVs. draw=false for
// blanks (space) or missing glyphs.
struct GlyphQuad {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
  bool  draw = false;
};

class MsdfFont {
 public:
  static constexpr int FLOATS_PER_VERT = 8;
  static constexpr int VERTS_PER_GLYPH = 6;

  bool load(AssetReader& reader, const char* metricsPath, const char* atlasPath);
  bool generate(AssetReader& reader, const char* fontPath, const char* cachePath = nullptr);
  // Bakes an additional face into the SAME shared atlas (appended as new rows,
  // existing glyphs untouched) and registers it under `style`, the same
  // (byKey_ / styleCmap_) contract Canvas::textStyled() already reads via
  // keyForStyle()/glyphByKey() — see msdf.cc for details. Must be called
  // after generate() (which must run first: it sets distanceRange_/sizePxEm_
  // and creates the base atlas_ this appends to) and before the Renderer
  // uploads the atlas texture (Renderer::initMsdf), or the added rows won't
  // be on the GPU. Persisted by loadCache()/saveCache() alongside the base font.
  bool addStyle(AssetReader& reader, const char* fontPath, FontStyle style);

  // Bakes glyphs for specific codepoints from an external font into the
  // SAME shared default-face table (glyphs_/fast_) that generate() itself
  // populates — not a styled slot, so Canvas::textStyled()'s existing
  // "keyForStyle() miss -> fall back to the default face" path (see
  // vk_canvas/core/canvas.cc) picks these up for Bold/Italic/Math text too,
  // with no Canvas changes needed. Used to add script coverage (Cyrillic,
  // Greek, CJK, …) the primary font (Latin Modern) doesn't have, without
  // re-baking the whole atlas. Codepoints already resolvable with a real
  // glyph are skipped (so calling this with several fallback fonts in
  // priority order — try font A's coverage, then font B for whatever A
  // didn't have — costs nothing extra for codepoints an earlier call
  // already covered). Appends new atlas rows exactly like addStyle();
  // caller must re-run Renderer::initMsdf() afterward to push the grown
  // atlas to the GPU (see player_window.cpp). Returns how many codepoints
  // were newly baked (0 if none of `codepoints` had a glyph in this font,
  // or all were already covered).
  int bakeCodepoints(AssetReader& reader, const char* fontPath,
                     const std::vector<uint32_t>& codepoints);
  // True if addStyle() (or a loaded cache that already baked it) has content
  // for this style — lets a caller skip a redundant addStyle() rebake after a
  // cache hit that already included it.
  bool hasStyle(FontStyle style) const { return !styleCmap_[static_cast<int>(style)].empty(); }
  bool loadCache(const char* cachePath);
  bool saveCache(const char* cachePath);
  // Valid = metrics usable for measuring/layout. Deliberately NOT tied to the
  // CPU atlas pixels being resident: releaseAtlasPixels() frees those after
  // the GPU upload, and layout/measure only needs the glyph tables.
  bool valid() const { return !glyphs_.empty() && atlasW_ > 0 && atlasH_ > 0; }

  // The atlas pixel buffer is only needed twice: to upload the texture to the
  // GPU, and as the append target when baking more glyphs. Between those
  // moments it's dead weight (~8 MB for a fully-baked atlas) duplicating the
  // GPU copy in host RAM — release it once uploaded, and re-hydrate from the
  // disk cache before any re-bake (addStyle/bakeCodepoints APPEND to it).
  void releaseAtlasPixels() { atlas_.clear(); atlas_.shrink_to_fit(); }
  bool atlasResident() const { return !atlas_.empty(); }
  // Re-reads ONLY the raw atlas bytes from a saveCache() file into atlas_
  // (metrics stay as-is — they never left). No-op if already resident.
  bool ensureAtlasLoaded(const char* cachePath);

  uint32_t atlasW() const { return atlasW_; }
  uint32_t atlasH() const { return atlasH_; }
  // True when the atlas's alpha channel carries a real single-channel SDF
  // (runtime generate()/v8 cache — see generateMTSDF in msdf.cc). False for
  // pre-baked load() atlases whose alpha is a constant 255; renderers gate
  // their small-size median→trueSDF blend on this so those stay pixel-exact.
  bool isMtsdf() const { return isMtsdf_; }
  const std::vector<uint8_t>& atlas() const { return atlas_; }
  float distanceRange() const { return distanceRange_; }
  // The MSDF distance-field margin baked into every glyph's plane box, in EM. The
  // true ink edges are plane{L,T} + pad and plane{R,B} − pad. Needed for pixel-
  // exact joins (e.g. attaching the radical vinculum to the surd's true peak).
  float glyphPadEm() const { return sizePxEm_ > 0.0f ? distanceRange_ / sizePxEm_ : 0.0f; }

  // True if `cp` already resolves to a real (non-blank) glyph in the shared
  // default-face table — i.e. bakeCodepoints() doesn't need to be asked for
  // it again. Used to find which codepoints in a freshly-rescanned library
  // are genuinely new before spending a bake+GPU-reupload cycle on them.
  bool hasCodepoint(uint32_t cp) const {
    const MsdfGlyph* g = lookup(cp);
    return g && g->hasGlyph;
  }

  float advance(uint32_t cp, float sizePx) const;
  float textWidth(std::string_view s, float sizePx) const;
  float lineHeight(float sizePx) const { return lineHeight_ * sizePx; }
  float ascender(float sizePx) const { return -ascender_ * sizePx; }

  // Append one glyph's quad at pen (penX, baselineY). Returns advanced penX.
  float emitGlyph(std::vector<float>& out, uint32_t cp, float penX,
                  float baselineY, float sizePx,
                  float r, float g, float b, float a) const;

  // Lay out one glyph (no emission) so the caller can clip the quad. Returns
  // the advanced pen X; fills q with geometry (q.draw == false to skip).
  float layout(uint32_t cp, float penX, float baselineY, float sizePx,
               GlyphQuad& q) const;

  // ── OpenType MATH (font.msdf v2) ─────────────────────────────────────────
  bool hasMath() const { return hasMath_; }
  const MathConstants& mathConstants() const { return mc_; }

  // Glyph addressed by key = (fontId<<24)|gid (math glyphs + variant/assembly
  // parts that have no codepoint). nullptr if absent.
  const MsdfGlyph* glyphByKey(uint32_t key) const {
    auto it = byKey_.find(key);
    return it == byKey_.end() ? nullptr : &it->second;
  }
  // Resolve a codepoint to a glyph key in a given style (Roman/Bold/Italic/Math).
  // Returns 0 if that style doesn't cover the codepoint.
  uint32_t keyForStyle(FontStyle s, uint32_t cp) const {
    const auto& m = styleCmap_[static_cast<int>(s)];
    auto it = m.find(cp);
    return it == m.end() ? 0 : it->second;
  }
  // Convenience: math-face key (math-italic variables, Greek, operators…).
  uint32_t mathKey(uint32_t cp) const { return keyForStyle(FontStyle::Math, cp); }
  // Vertical construction (variants + assembly) for a growable base glyph key, or
  // nullptr if that glyph doesn't stretch.
  const MathConstruction* construction(uint32_t baseKey) const {
    auto it = constr_.find(baseKey);
    return it == constr_.end() ? nullptr : &it->second;
  }

  // Lay out one glyph addressed by key (no emission): fills q with the screen
  // quad + UVs so the caller (Canvas) can clip/rotate it like text. Returns the
  // advanced penX. Used for math-italic atoms and stretched-assembly parts.
  float layoutByKey(uint32_t key, float penX, float baselineY, float sizePx,
                    GlyphQuad& q) const;
  // Advance width (em·sizePx) of a key glyph.
  float advanceKey(uint32_t key, float sizePx) const {
    const MsdfGlyph* g = glyphByKey(key);
    return g ? g->advance * sizePx : 0.0f;
  }

  // Choose how to render a growable glyph at `targetEm` height: the smallest
  // pre-built variant that reaches it, else a part assembly with enough extenders
  // (connector overlaps ≥ minConnectorOverlap). Pure metrics; no emission.
  VStretch buildVStretch(const MathConstruction& c, float targetEm) const;

 private:
  // Glyph lookup is the font engine's hottest path (every char of every text
  // measure + emit). Codepoints 0x00–0xFF (ASCII + Latin-1 Supplement — the
  // overwhelmingly common case) resolve through a flat array; rarer codepoints
  // fall back to the hash map.
  static constexpr uint32_t kFastCount = 0x100;
  const MsdfGlyph* lookup(uint32_t cp) const {
    if (cp < kFastCount) return fastHas_[cp] ? &fast_[cp] : nullptr;
    auto it = glyphs_.find(cp);
    return it == glyphs_.end() ? nullptr : &it->second;
  }

  std::unordered_map<uint32_t, MsdfGlyph> glyphs_;
  MsdfGlyph fast_[kFastCount];
  bool      fastHas_[kFastCount] = {};
  std::vector<uint8_t> atlas_;
  uint32_t atlasW_ = 0, atlasH_ = 0;
  bool isMtsdf_ = false;   // see isMtsdf()
  float distanceRange_ = 4.0f, sizePxEm_ = 40.0f;
  float lineHeight_ = 1.2f, ascender_ = 0.0f, descender_ = 0.0f;

  // ── MATH (v2) ──
  bool hasMath_ = false;
  MathConstants mc_;
  std::unordered_map<uint32_t, MsdfGlyph>       byKey_;     // (font<<24)|gid → glyph
  std::unordered_map<uint32_t, uint32_t>        styleCmap_[kFontStyleCount]; // codepoint → key, per style
  std::unordered_map<uint32_t, MathConstruction> constr_;   // base key → vertical construction
};
