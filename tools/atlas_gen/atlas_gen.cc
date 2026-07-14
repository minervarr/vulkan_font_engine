// atlas_gen — bake fonts into a true multi-channel MSDF atlas WITH OpenType MATH.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
//
// C++ port of the original Rust `atlas_gen` (libs/regen_atlas). Glyph outlines and
// metrics come from FreeType (via msdfgen-ext); `msdfgen` (Viktor Chlumský's own
// C++ library — the algorithm `fdsm` was a Rust port of) produces a real 3-channel
// MSDF per glyph. MSDF is scale-independent, so a COMPACT atlas (small EM) stays
// razor-crisp from tiny button labels to the huge calculator display. We deliberately
// reuse the exact msdfgen call sequence the app already renders with at runtime
// (normalize → orientContours → edgeColoringSimple → generateMSDF, row-flipped),
// which is why CFF/OTF winding comes out correct without fdsm's correct_sign pass.
//
// Like the Rust original this bakes MULTIPLE faces BY GLYPH-ID into one shared sheet
// and cracks open the OpenType `MATH` table of the math font (see math_table.hh):
//   • MathConstants  — the ~50 TeX-grade layout dimens.
//   • MathVariants   — for each growable glyph the pre-built bigger variants AND the
//     GlyphAssembly recipe (parts + connector overlaps + the extender flag).
//   • Per-glyph italic correction.
// Variant + assembly part glyphs have NO codepoint, so the whole baker is GID-
// addressed; we walk the construction closure and bake every referenced part.
//
// The bespoke `font.msdf` binary format (v2, magic 0x4644534D, version word = 2) is
// read by the C++ MsdfFont loader (vulkan_font_engine/.../msdf.cc); the field order
// here must stay byte-for-byte in lock-step with that reader.

// FreeType must precede msdfgen-ext.h so adoptFreetypeFont / readFreetypeOutline
// (guarded by #ifdef FT_LOAD_DEFAULT) are visible.
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

#include <msdfgen.h>
#include <msdfgen-ext.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "math_table.hh"

namespace {

// Density knobs. EM (atlas texels per em) is the sharpness lever; RANGE is always
// EM/10 so the distance-field margin stays 0.1em (mathlayout's glyphPadEm depends on
// it). AW is the sheet width — it must grow with EM so the packed sheet stays under
// 4096 (the min-spec Vulkan maxImageDimension2D) in BOTH dimensions. EM and AW can be
// overridden at runtime via the ATLAS_EM / ATLAS_AW env vars for experimentation.
double EM = 100.0;  // was 80; ↑25% density → crisper corners on the big high-DPI display.
double RANGE = 10.0;  // = EM/10, set in initParams().
int AW = 3072;      // widened from 2048 to keep the EM=100 sheet < 4096 tall.
constexpr unsigned long long SEED = 69441337420ULL;  // edge-colouring RNG seed

void initParams() {
  if (const char* e = std::getenv("ATLAS_EM")) { double v = std::atof(e); if (v > 0) EM = v; }
  if (const char* e = std::getenv("ATLAS_AW")) { int v = std::atoi(e); if (v > 0) AW = v; }
  RANGE = EM * 0.1;  // keep RANGE/EM == 0.1 regardless of EM
}
// v3 = MTSDF: the sheet's alpha channel carries a TRUE single-channel SDF
// (was a constant 255 in v2), enabling the fragment shader's small-size
// speckle cleanup (msdf_frag.slang's push.mtsdf blend) for pre-baked atlases
// too, matching the runtime cache's v8+ MTSDF bake. Field order is unchanged
// from v2 — only the version word and the alpha bytes differ.
constexpr uint32_t VERSION = 3;  // font.msdf format version

// Font (== named style) ids. One atlas, glyphs keyed by (font_id, gid) via key().
constexpr uint32_t F_TEXT = 0;    // lmroman regular — UI text, digits, function names (upright); the default
constexpr uint32_t F_BOLD = 1;    // lmroman bold    — emphasis / headers
constexpr uint32_t F_MATH = 2;    // latinmodern-math — variables (math-italic), Greek, operators, delimiters, radicals
constexpr uint32_t F_ITALIC = 3;  // lmroman italic  — italic prose
constexpr uint32_t TEXT_FONTS[3] = {F_TEXT, F_BOLD, F_ITALIC};

inline uint32_t key(uint32_t fontId, uint16_t gid) {
  return (fontId << 24) | static_cast<uint32_t>(gid);
}

struct FontAsset {
  uint32_t id;
  FT_Face face;
  msdfgen::FontHandle* fh;
  double upm;
};

struct Glyph {
  uint32_t key;
  float advance;       // em
  float italic;        // em (math italic correction; 0 for text)
  bool has;
  float plane[4];      // em units, y-down (top negative)
  float atlas[4];      // atlas pixels, y from top
  int w, h;
  std::vector<uint8_t> rgba;  // w*h*4, top-first (alpha = true SDF, MTSDF)
};

Glyph metricsOnly(uint32_t k, double advance) {
  Glyph g{};
  g.key = k;
  g.advance = static_cast<float>(advance);
  g.has = false;
  return g;
}

// Rasterise one glyph (by GID) of `f` into an MTSDF cell (RGB = multi-channel
// field, alpha = true single-channel SDF). The atlas rect is
// assigned later by the shelf packer. Mirrors msdf.cc::MsdfFont::generate per-cell.
Glyph bakeGlyph(const FontAsset& f, uint32_t k, uint16_t gid) {
  msdfgen::Shape shape;
  double advanceFu = 0.0;
  if (!msdfgen::loadGlyph(shape, f.fh, msdfgen::GlyphIndex(gid),
                          msdfgen::FONT_SCALING_NONE, &advanceFu)) {
    return metricsOnly(k, advanceFu / f.upm);
  }
  double advance = advanceFu / f.upm;

  shape.normalize();
  // CFF/PostScript-flavoured OTFs wind outer contours opposite to TrueType; msdfgen's
  // inside-positive convention assumes the latter, so orient before colouring/gen.
  shape.orientContours();
  msdfgen::edgeColoringSimple(shape, 3.0, SEED);

  msdfgen::Shape::Bounds b = shape.getBounds();
  if (!(b.l < b.r && b.b < b.t)) return metricsOnly(k, advance);  // space / empty

  const double scale = EM / f.upm;  // texels per font unit (== Rust 1/shrinkage)
  int w = static_cast<int>(std::ceil((b.r - b.l) * scale + 2.0 * RANGE));
  int h = static_cast<int>(std::ceil((b.t - b.b) * scale + 2.0 * RANGE));
  if (w <= 0 || h <= 0) return metricsOnly(k, advance);

  msdfgen::Bitmap<float, 4> msdf(w, h);
  msdfgen::Vector2 translate(-b.l + RANGE / scale, -b.b + RANGE / scale);
  msdfgen::generateMTSDF(msdf, shape, RANGE / scale, msdfgen::Vector2(scale), translate);

  // msdfgen output is y-up (font convention) → flip rows so the atlas is top-first.
  Glyph g{};
  g.key = k;
  g.advance = static_cast<float>(advance);
  g.italic = 0.0f;
  g.has = true;
  g.w = w;
  g.h = h;
  g.rgba.resize(static_cast<size_t>(w) * h * 4);
  for (int j = 0; j < h; j++) {
    int src = h - 1 - j;
    for (int i = 0; i < w; i++) {
      auto px = msdf(i, src);
      size_t o = (static_cast<size_t>(j) * w + i) * 4;
      g.rgba[o] = msdfgen::pixelFloatToByte(px[0]);
      g.rgba[o + 1] = msdfgen::pixelFloatToByte(px[1]);
      g.rgba[o + 2] = msdfgen::pixelFloatToByte(px[2]);
      g.rgba[o + 3] = msdfgen::pixelFloatToByte(px[3]);
    }
  }

  double planeL = b.l / f.upm - RANGE / EM;
  // Anchor the plane to the BOTTOM edge, matching the raster translate
  // (-b.b + RANGE/scale) above: the ceil() slack in h lands at the top of
  // the cell, so a top-anchored planeT shifted every glyph's ink up by its
  // per-glyph fractional remainder (visible baseline jitter). Mirrors the
  // identical fix in core/msdf.cc.
  double planeB = -(b.b / f.upm) + RANGE / EM;
  double planeR = planeL + w / EM;
  double planeT = planeB - h / EM;
  g.plane[0] = static_cast<float>(planeL);
  g.plane[1] = static_cast<float>(planeT);
  g.plane[2] = static_cast<float>(planeR);
  g.plane[3] = static_cast<float>(planeB);
  return g;
}

// Bake glyph (fontId, gid) if not already present; returns its key. Deduped so the
// assembly closure never bakes the same extender twice.
uint32_t ensure(FontAsset* fonts, std::vector<Glyph>& glyphs,
                std::unordered_map<uint32_t, size_t>& index, uint32_t fontId, uint16_t gid) {
  uint32_t k = key(fontId, gid);
  if (index.count(k)) return k;
  Glyph g = bakeGlyph(fonts[fontId], k, gid);
  index[k] = glyphs.size();
  glyphs.push_back(std::move(g));
  return k;
}

struct CmapEntry { uint32_t cp, font, key; };

// Look up `cp` in font `fontId`; if present, bake it and record the cmap entry.
bool addCp(FontAsset* fonts, std::vector<Glyph>& glyphs,
           std::unordered_map<uint32_t, size_t>& index, std::vector<CmapEntry>& cmap,
           uint32_t fontId, uint32_t cp) {
  msdfgen::GlyphIndex gi;
  msdfgen::getGlyphIndex(gi, fonts[fontId].fh, cp);
  unsigned idx = gi.getIndex();
  if (idx == 0) return false;  // FT_Get_Char_Index → 0 means .notdef / missing
  uint32_t k = ensure(fonts, glyphs, index, fontId, static_cast<uint16_t>(idx));
  cmap.push_back({cp, fontId, k});
  return true;
}

// Math-italic codepoint for an ASCII letter (variables). The Mathematical
// Alphanumeric block has one hole — italic 'h' lives at U+210E (ℎ).
uint32_t mathItalicCp(char c) {
  if (c == 'h') return 0x210E;
  if (c >= 'a' && c <= 'z') return 0x1D44E + (c - 'a');
  if (c >= 'A' && c <= 'Z') return 0x1D434 + (c - 'A');
  return static_cast<uint32_t>(c);
}

// Out-mirrors of the Rust ConstructionOut for diagnostics + serialisation.
struct VariantOut { uint32_t key; float advance; };
struct PartOut { uint32_t key; float start, end, full; bool ext; };
struct ConstructionOut {
  uint32_t baseKey;
  float italic;
  std::vector<VariantOut> variants;
  std::vector<PartOut> parts;
};

// ── Little-endian byte writers (match Rust to_le_bytes; x86/ARM are LE). ──
void putU32(std::vector<uint8_t>& b, uint32_t v) {
  b.push_back(v & 0xFF); b.push_back((v >> 8) & 0xFF);
  b.push_back((v >> 16) & 0xFF); b.push_back((v >> 24) & 0xFF);
}
void putF32(std::vector<uint8_t>& b, float f) {
  uint32_t v; std::memcpy(&v, &f, 4); putU32(b, v);
}

std::vector<uint8_t> readFile(const char* path) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { std::fprintf(stderr, "cannot read %s\n", path); std::exit(1); }
  f.seekg(0, std::ios::end);
  std::streamoff n = f.tellg();
  f.seekg(0, std::ios::beg);
  std::vector<uint8_t> b(static_cast<size_t>(n));
  if (n > 0) f.read(reinterpret_cast<char*>(b.data()), n);
  return b;
}

void writeFile(const char* path, const std::vector<uint8_t>& b) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) { std::fprintf(stderr, "cannot write %s\n", path); std::exit(1); }
  f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
}

// Pull a raw SFNT table's bytes out of an FT_Face (used for the MATH table).
std::vector<uint8_t> loadSfntTable(FT_Face face, FT_ULong tag) {
  FT_ULong len = 0;
  if (FT_Load_Sfnt_Table(face, tag, 0, nullptr, &len) != 0 || len == 0) return {};
  std::vector<uint8_t> buf(len);
  if (FT_Load_Sfnt_Table(face, tag, 0, buf.data(), &len) != 0) return {};
  return buf;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 7) {
    std::fprintf(stderr, "usage: atlas_gen <roman.otf> <bold.otf> <italic.otf> <math.otf> <out.msdf> <out.rgba>\n");
    std::fprintf(stderr, "  roman/bold/italic : lmroman text faces (one optical size each)\n");
    std::fprintf(stderr, "  math.otf          : OpenType MATH font (latinmodern-math)\n");
    return 2;
  }
  initParams();

  FT_Library lib;
  if (FT_Init_FreeType(&lib)) { std::fprintf(stderr, "FT_Init_FreeType failed\n"); return 1; }

  // Keep font bytes alive for the lifetime of the faces (FT_New_Memory_Face borrows).
  std::vector<uint8_t> romanB = readFile(argv[1]);
  std::vector<uint8_t> boldB = readFile(argv[2]);
  std::vector<uint8_t> italicB = readFile(argv[3]);
  std::vector<uint8_t> mathB = readFile(argv[4]);

  auto mkFace = [&](std::vector<uint8_t>& bytes, const char* what) -> FT_Face {
    FT_Face f;
    if (FT_New_Memory_Face(lib, bytes.data(), static_cast<FT_Long>(bytes.size()), 0, &f)) {
      std::fprintf(stderr, "parse %s failed\n", what);
      std::exit(1);
    }
    return f;
  };
  FT_Face romanFace = mkFace(romanB, "roman font");
  FT_Face boldFace = mkFace(boldB, "bold font");
  FT_Face italicFace = mkFace(italicB, "italic font");
  FT_Face mathFace = mkFace(mathB, "math font");

  // Array index == font id: 0=roman, 1=bold, 2=math, 3=italic.
  FontAsset fonts[4];
  fonts[F_TEXT] = {F_TEXT, romanFace, msdfgen::adoptFreetypeFont(romanFace), (double)romanFace->units_per_EM};
  fonts[F_BOLD] = {F_BOLD, boldFace, msdfgen::adoptFreetypeFont(boldFace), (double)boldFace->units_per_EM};
  fonts[F_MATH] = {F_MATH, mathFace, msdfgen::adoptFreetypeFont(mathFace), (double)mathFace->units_per_EM};
  fonts[F_ITALIC] = {F_ITALIC, italicFace, msdfgen::adoptFreetypeFont(italicFace), (double)italicFace->units_per_EM};
  const double mathUpm = fonts[F_MATH].upm;

  std::vector<Glyph> glyphs;
  std::unordered_map<uint32_t, size_t> index;
  std::vector<CmapEntry> cmap;

  // ── TEXT faces (roman / bold / italic): ASCII + Latin-1 + legacy symbol extras. ──
  for (uint32_t fid : TEXT_FONTS) {
    for (uint32_t c = 0x20; c <= 0x7E; c++) addCp(fonts, glyphs, index, cmap, fid, c);
    for (uint32_t c = 0xA1; c <= 0xFF; c++) addCp(fonts, glyphs, index, cmap, fid, c);
    for (uint32_t c : {0x152u, 0x153u, 0x178u, 0x20ACu, 0x2212u, 0x221Au, 0x00D7u, 0x00F7u})
      addCp(fonts, glyphs, index, cmap, fid, c);
  }

  // ── MATH face (font 2): the math character set Bruno specified. ──
  std::vector<uint32_t> mathCps;
  for (char c = 'a'; c <= 'z'; c++) mathCps.push_back(mathItalicCp(c));
  for (char c = 'A'; c <= 'Z'; c++) mathCps.push_back(mathItalicCp(c));
  for (uint32_t c = 0x0391; c <= 0x03A9; c++) if (c != 0x03A2) mathCps.push_back(c);  // Greek upper
  for (uint32_t c = 0x03B1; c <= 0x03C9; c++) mathCps.push_back(c);                    // Greek lower
  for (uint32_t c : {0x002Bu, 0x2212u, 0x00D7u, 0x00F7u, 0x22C5u, 0x00B1u, 0x2213u, 0x2217u,
                     0x003Du, 0x2260u, 0x2264u, 0x2265u, 0x2248u, 0x2261u, 0x221Du, 0x2245u,
                     0x003Cu, 0x003Eu, 0x2192u, 0x21D2u, 0x21D4u}) mathCps.push_back(c);
  for (uint32_t c : {0x222Bu, 0x222Cu, 0x222Eu, 0x2211u, 0x220Fu, 0x2202u, 0x221Eu, 0x2207u,
                     0x2208u, 0x2209u, 0x2205u, 0x2218u}) mathCps.push_back(c);
  for (uint32_t c : {0x211Du, 0x2102u, 0x2115u, 0x2124u, 0x211Au}) mathCps.push_back(c);
  for (uint32_t c : {0x2112u, 0x2113u, 0x2110u, 0x2118u}) mathCps.push_back(c);
  for (uint32_t c = 0x1D49C; c <= 0x1D4B5; c++) mathCps.push_back(c);  // script A–Z (has holes)
  for (uint32_t c : {0x0028u, 0x0029u, 0x005Bu, 0x005Du, 0x007Bu, 0x007Du, 0x007Cu, 0x221Au,
                     0x2308u, 0x2309u, 0x230Au, 0x230Bu, 0x27E8u, 0x27E9u}) mathCps.push_back(c);

  uint32_t mathMissing = 0;
  for (uint32_t cp : mathCps) {
    if (!addCp(fonts, glyphs, index, cmap, F_MATH, cp)) {
      mathMissing++;
      std::fprintf(stderr, "skip: math U+%04X not in font\n", cp);
    }
  }

  // ── Crack open the MATH table. ──
  mathtbl::MathTable math(loadSfntTable(mathFace, 0x4D415448 /* 'MATH' */));
  if (!math.ok()) { std::fprintf(stderr, "math font has no MATH table\n"); return 1; }
  const mathtbl::Constants& mc = math.constants();

  // Constants (design units → em; percentages → fraction). Order MUST match the
  // Rust MathConstantsOut::fields() and the C++ reader.
  auto em = [&](int16_t v) { return static_cast<float>(static_cast<double>(v) / mathUpm); };
  auto pc = [&](int16_t v) { return static_cast<float>(static_cast<double>(v) / 100.0); };
  float cfields[42] = {0};
  if (mc.present) {
    cfields[0] = pc(mc.scriptPercentScaleDown);
    cfields[1] = pc(mc.scriptScriptPercentScaleDown);
    cfields[2] = static_cast<float>(static_cast<double>(mc.displayOperatorMinHeight) / mathUpm);
    cfields[3] = em(mc.axisHeight);
    cfields[4] = em(mc.accentBaseHeight);
    cfields[5] = em(mc.subscriptShiftDown);
    cfields[6] = em(mc.subscriptTopMax);
    cfields[7] = em(mc.superscriptShiftUp);
    cfields[8] = em(mc.superscriptShiftUpCramped);
    cfields[9] = em(mc.superscriptBottomMin);
    cfields[10] = em(mc.subSuperscriptGapMin);
    cfields[11] = em(mc.spaceAfterScript);
    cfields[12] = em(mc.upperLimitGapMin);
    cfields[13] = em(mc.upperLimitBaselineRiseMin);
    cfields[14] = em(mc.lowerLimitGapMin);
    cfields[15] = em(mc.lowerLimitBaselineDropMin);
    cfields[16] = em(mc.stackTopShiftUp);
    cfields[17] = em(mc.stackTopDisplayStyleShiftUp);
    cfields[18] = em(mc.stackBottomShiftDown);
    cfields[19] = em(mc.stackBottomDisplayStyleShiftDown);
    cfields[20] = em(mc.stackGapMin);
    cfields[21] = em(mc.stackDisplayStyleGapMin);
    cfields[22] = em(mc.fractionNumeratorShiftUp);
    cfields[23] = em(mc.fractionNumeratorDisplayStyleShiftUp);
    cfields[24] = em(mc.fractionDenominatorShiftDown);
    cfields[25] = em(mc.fractionDenominatorDisplayStyleShiftDown);
    cfields[26] = em(mc.fractionNumeratorGapMin);
    cfields[27] = em(mc.fractionNumDisplayStyleGapMin);
    cfields[28] = em(mc.fractionRuleThickness);
    cfields[29] = em(mc.fractionDenominatorGapMin);
    cfields[30] = em(mc.fractionDenomDisplayStyleGapMin);
    cfields[31] = em(mc.overbarVerticalGap);
    cfields[32] = em(mc.overbarRuleThickness);
    cfields[33] = em(mc.overbarExtraAscender);
    cfields[34] = em(mc.radicalVerticalGap);
    cfields[35] = em(mc.radicalDisplayStyleVerticalGap);
    cfields[36] = em(mc.radicalRuleThickness);
    cfields[37] = em(mc.radicalExtraAscender);
    cfields[38] = em(mc.radicalKernBeforeDegree);
    cfields[39] = em(mc.radicalKernAfterDegree);
    cfields[40] = pc(mc.radicalDegreeBottomRaisePercent);
  }
  cfields[41] = static_cast<float>(static_cast<double>(math.minConnectorOverlap()) / mathUpm);

  // Vertical constructions for the stretchy bases. Walk the closure: bake every
  // variant glyph and every assembly part by GID.
  const uint32_t stretchy[16] = {
      0x221A,                                              // √ radical
      0x0028, 0x0029, 0x005B, 0x005D, 0x007B, 0x007D, 0x007C,  // ( ) [ ] { } |
      0x2308, 0x2309, 0x230A, 0x230B,                      // ⌈ ⌉ ⌊ ⌋
      0x222B, 0x222E, 0x2211, 0x220F,                      // ∫ ∮ ∑ ∏
  };
  std::vector<ConstructionOut> constructions;
  for (uint32_t cp : stretchy) {
    msdfgen::GlyphIndex gi;
    msdfgen::getGlyphIndex(gi, fonts[F_MATH].fh, cp);
    unsigned baseGid = gi.getIndex();
    if (baseGid == 0) continue;
    auto constr = math.verticalConstruction(static_cast<uint16_t>(baseGid));
    if (!constr) continue;  // not growable in this font

    ConstructionOut out;
    out.baseKey = ensure(fonts, glyphs, index, F_MATH, static_cast<uint16_t>(baseGid));
    out.italic = 0.0f;
    for (const mathtbl::Variant& v : constr->variants) {
      uint32_t k = ensure(fonts, glyphs, index, F_MATH, v.glyph);
      out.variants.push_back({k, static_cast<float>(static_cast<double>(v.advanceMeasurement) / mathUpm)});
    }
    if (constr->hasAssembly) {
      out.italic = em(constr->italicsCorrection);
      for (const mathtbl::Part& p : constr->parts) {
        uint32_t k = ensure(fonts, glyphs, index, F_MATH, p.glyph);
        out.parts.push_back({k,
                             static_cast<float>(static_cast<double>(p.startConnectorLength) / mathUpm),
                             static_cast<float>(static_cast<double>(p.endConnectorLength) / mathUpm),
                             static_cast<float>(static_cast<double>(p.fullAdvance) / mathUpm),
                             p.extender});
      }
    }
    constructions.push_back(std::move(out));
  }

  // Per-glyph italic correction for every baked MATH glyph.
  for (Glyph& g : glyphs) {
    if ((g.key >> 24) != F_MATH) continue;
    uint16_t gid = static_cast<uint16_t>(g.key & 0xFFFF);
    if (auto v = math.italicCorrection(gid)) g.italic = em(*v);
  }

  // ── Shelf-pack glyph cells into an AW-wide atlas. ──
  int curX = 0, curY = 0, rowH = 0;
  for (Glyph& g : glyphs) {
    if (!g.has) continue;
    if (curX + g.w > AW) { curX = 0; curY += rowH + 2; rowH = 0; }
    g.atlas[0] = (float)curX; g.atlas[1] = (float)curY;
    g.atlas[2] = (float)(curX + g.w); g.atlas[3] = (float)(curY + g.h);
    curX += g.w + 2;
    if (g.h > rowH) rowH = g.h;
  }
  int ah = curY + rowH;

  std::vector<uint8_t> atlas(static_cast<size_t>(AW) * ah * 4, 0);
  for (const Glyph& g : glyphs) {
    if (!g.has) continue;
    int ox = static_cast<int>(g.atlas[0]);
    int oy = static_cast<int>(g.atlas[1]);
    for (int j = 0; j < g.h; j++) {
      for (int i = 0; i < g.w; i++) {
        size_t s = (static_cast<size_t>(j) * g.w + i) * 4;
        size_t p = (static_cast<size_t>(oy + j) * AW + (ox + i)) * 4;
        atlas[p] = g.rgba[s];
        atlas[p + 1] = g.rgba[s + 1];
        atlas[p + 2] = g.rgba[s + 2];
        atlas[p + 3] = g.rgba[s + 3];
      }
    }
  }
  writeFile(argv[6], atlas);

  // ── Metrics + font.msdf v3 header. ──
  // Global text metrics come from the TEXT face (font 0) — the line model the UI uses.
  // Read hhea directly so it matches the Rust (ttf-parser) ascender/descender/lineGap.
  const FontAsset& tf = fonts[F_TEXT];
  double tupm = tf.upm;
  TT_HoriHeader* hhea = static_cast<TT_HoriHeader*>(FT_Get_Sfnt_Table(tf.face, FT_SFNT_HHEA));
  double hAsc = hhea ? hhea->Ascender : tf.face->ascender;
  double hDesc = hhea ? hhea->Descender : tf.face->descender;
  double hGap = hhea ? hhea->Line_Gap : 0.0;
  float ascender = static_cast<float>(-(hAsc / tupm));
  float descender = static_cast<float>(-(hDesc / tupm));
  float lineHeight = static_cast<float>((hAsc - hDesc + hGap) / tupm);

  std::vector<uint8_t> buf;
  // Header.
  putU32(buf, 0x4644534D);  // magic 'MSDF'
  putU32(buf, VERSION);     // = 3 (MTSDF)
  putU32(buf, static_cast<uint32_t>(AW));
  putU32(buf, static_cast<uint32_t>(ah));
  putF32(buf, static_cast<float>(RANGE));
  putF32(buf, static_cast<float>(EM));
  putF32(buf, lineHeight);
  putF32(buf, ascender);
  putF32(buf, descender);

  // MathConstants block: count + values (fixed order).
  putU32(buf, 42);
  for (float v : cfields) putF32(buf, v);

  // Glyphs: key, advance, italic, has, plane[4], atlas[4].
  putU32(buf, static_cast<uint32_t>(glyphs.size()));
  for (const Glyph& g : glyphs) {
    putU32(buf, g.key);
    putF32(buf, g.advance);
    putF32(buf, g.italic);
    putU32(buf, g.has ? 1u : 0u);
    for (float v : g.plane) putF32(buf, v);
    for (float v : g.atlas) putF32(buf, v);
  }

  // Cmap: (codepoint, font, key) entry points.
  putU32(buf, static_cast<uint32_t>(cmap.size()));
  for (const CmapEntry& e : cmap) { putU32(buf, e.cp); putU32(buf, e.font); putU32(buf, e.key); }

  // Vertical constructions: per base — base_key, italic, variants[], parts[].
  putU32(buf, static_cast<uint32_t>(constructions.size()));
  for (const ConstructionOut& c : constructions) {
    putU32(buf, c.baseKey);
    putF32(buf, c.italic);
    putU32(buf, static_cast<uint32_t>(c.variants.size()));
    putU32(buf, static_cast<uint32_t>(c.parts.size()));
    for (const VariantOut& v : c.variants) { putU32(buf, v.key); putF32(buf, v.advance); }
    for (const PartOut& p : c.parts) {
      putU32(buf, p.key); putF32(buf, p.start); putF32(buf, p.end); putF32(buf, p.full);
      putU32(buf, p.ext ? 1u : 0u);
    }
  }

  writeFile(argv[5], buf);

  // ── Diagnostics (Phase 1 verification). ──
  size_t baked = 0;
  for (const Glyph& g : glyphs) if (g.has) baked++;
  std::printf("atlas %dx%d (%zu bytes rgba), EM=%g, %zu glyphs (%zu with outlines), %zu cmap entries\n",
              AW, ah, atlas.size(), EM, glyphs.size(), baked, cmap.size());
  std::printf("math: %u codepoints missing\n", mathMissing);
  std::printf("MathConstants: axisHeight=%.4f fracRule=%.4f radRule=%.4f radVGap=%.4f radExtraAsc=%.4f scriptScale=%.3f minOverlap=%.4f\n",
              cfields[3], cfields[28], cfields[36], cfields[34], cfields[37], cfields[0], cfields[41]);
  std::printf("vertical constructions: %zu\n", constructions.size());
  for (const ConstructionOut& c : constructions) {
    std::printf("  base gid %4u: %zu variants, %zu assembly parts (italic=%.3f)\n",
                c.baseKey & 0xFFFF, c.variants.size(), c.parts.size(), c.italic);
  }
  // Spotlight the radical — our Proof-of-Life target.
  msdfgen::GlyphIndex radGi;
  msdfgen::getGlyphIndex(radGi, fonts[F_MATH].fh, 0x221A);
  if (radGi.getIndex() != 0) {
    uint32_t rk = key(F_MATH, static_cast<uint16_t>(radGi.getIndex()));
    for (const ConstructionOut& c : constructions) {
      if (c.baseKey != rk) continue;
      std::printf("RADICAL √ (gid %u): %zu variants, %zu parts %s\n",
                  radGi.getIndex(), c.variants.size(), c.parts.size(),
                  c.parts.empty() ? "(variants-only stretch)" : "(extensible assembly)");
      for (size_t i = 0; i < c.parts.size(); i++) {
        const PartOut& p = c.parts[i];
        std::printf("    part[%zu] gid %4u ext=%d full=%.4f start=%.4f end=%.4f\n",
                    i, p.key & 0xFFFF, (int)p.ext, p.full, p.start, p.end);
      }
      break;
    }
  }

  return 0;
}
