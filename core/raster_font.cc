#include "raster_font.hh"

#include "log.hh"
#include "utf8.hh"


#include <cmath>
#include <cstring>

namespace {

// Padding between packed cells. One transparent pixel is enough to stop the
// linear sampler reaching a neighbour when a quad lands a hair off its texel
// grid, and costs about 8% of the sheet at these cell sizes.
constexpr int kPad = 1;

// How much taller to make the sheet when a shelf runs out. Growing in chunks
// rather than per-glyph keeps the reallocation count down; the sheet is
// trimmed to nothing beyond what was used only in the sense that atlasH_ is
// the high-water mark, which is what gets uploaded.
constexpr uint32_t kGrowRows = 256;

bool loadFace(AssetReader& reader, const char* path, vfe::RasterFace& face,
              vfe::KernTable& kern) {
  std::vector<uint8_t> buf;
  if (!reader.read(path, buf) || buf.empty()) {
    VFE_LOGE("Raster", "Raster: cannot read font %s", path);
    return false;
  }
  if (!face.openFromMemory(buf.data(), buf.size())) {
    VFE_LOGE("Raster", "Raster: cannot open font %s", path);
    return false;
  }
  // Not an error when absent: some faces genuinely have no GPOS kern data and
  // simply lay out unkerned. See gpos_kern.hh.
  vfe::parseGposKernPairs(buf.data(), buf.size(), kern);
  return true;
}

// Every codepoint the face has a real glyph for, over the ranges this UI can
// encounter. Enumerating the cmap directly would be better, but FreeType's
// iterator is not exposed through RasterFace and a bounded probe is enough:
// coverage is only used to decide which face serves which codepoint, and
// ensureGlyphs() is driven by the codepoints the caller actually needs.
void probeCoverage(const vfe::RasterFace& face, std::vector<uint32_t>& out) {
  out.clear();
  // Latin, Latin-1 Supplement, Latin Extended-A/B, punctuation, Greek,
  // Cyrillic — the ranges the primary and styled faces are expected to cover.
  static const uint32_t kRanges[][2] = {
    {0x0020, 0x024F}, {0x0370, 0x04FF}, {0x2000, 0x206F}, {0x20A0, 0x20BF},
    {0x2100, 0x214F}, {0x2190, 0x21FF}, {0x2200, 0x22FF},
  };
  for (const auto& r : kRanges)
    for (uint32_t cp = r[0]; cp <= r[1]; ++cp)
      if (face.hasCodepoint(cp)) out.push_back(cp);
}

}  // namespace

// ── Setup ───────────────────────────────────────────────────────────────────

bool RasterFont::open(AssetReader& reader, const char* fontPath) {
  auto f = std::make_unique<Face>();
  if (!loadFace(reader, fontPath, f->raster, f->kern)) return false;

  // Vertical metrics as EM fractions, taken once from the DESIGN units.
  // Deriving them from verticalMetrics() at some probe size would bake that
  // size's grid-fitting rounding into every other size — see designMetrics().
  float asc = 0, desc = 0, lh = 0;
  if (f->raster.designMetrics(asc, desc, lh)) {
    ascenderEm_   = asc;
    descenderEm_  = desc;
    lineHeightEm_ = lh;
  }

  probeCoverage(f->raster, f->cps);
  styles_[(int)FontStyle::Roman] = std::move(f);
  return true;
}

bool RasterFont::addStyle(AssetReader& reader, const char* fontPath,
                          FontStyle style) {
  auto f = std::make_unique<Face>();
  if (!loadFace(reader, fontPath, f->raster, f->kern)) return false;
  probeCoverage(f->raster, f->cps);
  styles_[(int)style] = std::move(f);
  return true;
}

bool RasterFont::addFallback(AssetReader& reader, const char* fontPath) {
  auto f = std::make_unique<Face>();
  if (!loadFace(reader, fontPath, f->raster, f->kern)) return false;
  // No coverage probe: a fallback is consulted per codepoint on demand, and
  // probing a CJK face over its tens of thousands of codepoints would cost
  // more than the bake it is meant to inform.
  fallbacks_.push_back(std::move(f));
  return true;
}

bool RasterFont::addOverride(AssetReader& reader, const char* fontPath) {
  auto f = std::make_unique<Face>();
  if (!loadFace(reader, fontPath, f->raster, f->kern)) return false;
  overrides_.push_back(std::move(f));
  return true;
}

bool RasterFont::hasStyle(FontStyle style) const {
  const Face* f = styles_[(int)style].get();
  return f && f->raster.isOpen();
}

// ── Face selection ──────────────────────────────────────────────────────────

const RasterFont::Face* RasterFont::faceFor(FontStyle style, uint32_t cp) const {
  // Overrides first — see addOverride() for the PUA collision this exists for.
  for (const auto& f : overrides_)
    if (f->raster.isOpen() && f->raster.hasCodepoint(cp)) return f.get();

  if (const Face* f = styles_[(int)style].get())
    if (f->raster.isOpen() && f->raster.hasCodepoint(cp)) return f;

  if (style != FontStyle::Roman)
    if (const Face* f = styles_[(int)FontStyle::Roman].get())
      if (f->raster.isOpen() && f->raster.hasCodepoint(cp)) return f;

  for (const auto& f : fallbacks_)
    if (f->raster.isOpen() && f->raster.hasCodepoint(cp)) return f.get();

  return nullptr;
}

bool RasterFont::hasCodepoint(uint32_t cp) const {
  return faceFor(FontStyle::Roman, cp) != nullptr;
}

uint32_t RasterFont::keyForStyle(FontStyle s, uint32_t cp) const {
  // A codepoint an override claims has exactly one rendering, whatever style
  // asked for it. Returning 0 sends it down Canvas's default-face path, so it
  // is baked once instead of once per style.
  for (const auto& f : overrides_)
    if (f->raster.isOpen() && f->raster.hasCodepoint(cp)) return 0;

  const Face* f = styles_[(int)s].get();
  if (!f || !f->raster.isOpen() || !f->raster.hasCodepoint(cp)) return 0;
  return glyphKey(s, cp);
}

float RasterFont::kernEmStyled(FontStyle s, uint32_t prevCp, uint32_t cp) const {
  if (!prevCp || !cp) return 0.0f;
  auto lookup = [&](const vfe::KernTable& t) -> float {
    if (t.empty()) return 0.0f;
    auto it = t.find(vfe::kernKey(prevCp, cp));
    return it == t.end() ? 0.0f : it->second;
  };
  if (const Face* f = styles_[(int)s].get())
    if (!f->kern.empty()) return lookup(f->kern);
  if (const Face* f = styles_[(int)FontStyle::Roman].get()) return lookup(f->kern);
  return 0.0f;
}

// ── Packing and baking ──────────────────────────────────────────────────────

bool RasterFont::packInto(int w, int h, uint32_t& outX, uint32_t& outY) {
  const uint32_t needW = (uint32_t)w + kPad;
  const uint32_t needH = (uint32_t)h + kPad;

  if (shelfX_ + needW > atlasW_) {   // close this shelf, start the next
    shelfY_ += shelfH_;
    shelfX_  = 0;
    shelfH_  = 0;
  }
  if (needH > shelfH_) shelfH_ = needH;

  while (shelfY_ + shelfH_ > atlasH_) {
    const uint32_t grown = atlasH_ + kGrowRows;
    if (grown > kAtlasMaxH) {
      VFE_LOGE("Raster", "Raster: atlas full at %ux%u — glyph %dx%d does not fit",
           atlasW_, atlasH_, w, h);
      return false;
    }
    atlasH_ = grown;
    atlas_.resize((size_t)atlasW_ * atlasH_, 0);
  }

  outX = shelfX_;
  outY = shelfY_;
  shelfX_ += needW;
  return true;
}

bool RasterFont::bakeCell(const Face& f, FontStyle style, int sizePx,
                          uint32_t cp) {
  vfe::RasterGlyph g;
  if (!f.raster.render(cp, sizePx, g)) return false;

  Cell c;
  c.advance  = g.advance;
  c.bearingX = g.bearingX;
  c.bearingY = g.bearingY;

  if (g.w > 0 && g.h > 0) {
    if (!packInto(g.w, g.h, c.atlasX, c.atlasY)) return false;
    c.hasGlyph = true;
    c.w = g.w;
    c.h = g.h;
    for (int y = 0; y < g.h; ++y) {
      std::memcpy(atlas_.data() + (size_t)(c.atlasY + y) * atlasW_ + c.atlasX,
                  g.cov.data() + (size_t)y * g.w, (size_t)g.w);
    }
  }
  // else: whitespace. Recorded with hasGlyph=false so layout advances the pen
  // without emitting a quad — the same contract MsdfGlyph::hasGlyph carries.

  cells_.emplace(cellKey(style, sizePx, cp), c);
  return true;
}

int RasterFont::ensureGlyphs(const std::vector<uint32_t>& cps,
                             const std::vector<int>& sizesPx) {
  for (int s : sizesPx)
    if (s > 0) sizes_.insert(s);
  if (sizes_.empty() || !styles_[(int)FontStyle::Roman]) return 0;

  // Cells are laid out in the sheet as they are baked, so an all-sizes pass
  // over one codepoint keeps that codepoint's variants near each other. That
  // is not required for correctness; it just keeps a shelf from mixing a 19px
  // Latin cell with a 44px CJK one and wasting the height difference.
  int added = 0;
  for (int style = 0; style < kFontStyleCount; ++style) {
    const Face* sf = styles_[style].get();
    if (!sf || !sf->raster.isOpen()) continue;

    for (uint32_t cp : cps) {
      // Which face actually serves this codepoint in this style. For a styled
      // face that does not cover it, keyForStyle() returns 0 and Canvas falls
      // back to the default face — so there is nothing to bake here.
      if ((FontStyle)style != FontStyle::Roman) {
        if (!sf->raster.hasCodepoint(cp)) continue;
      }
      const Face* use = faceFor((FontStyle)style, cp);
      if (!use) continue;

      for (int sz : sizes_) {
        if (cells_.count(cellKey((FontStyle)style, sz, cp))) continue;
        if (bakeCell(*use, (FontStyle)style, sz, cp)) added++;
      }
    }
  }

  if (added) {
    VFE_LOGI("Raster", "Raster: baked %d cells (%zu total) at %u sizes — atlas %ux%u (%.1f MB)",
         added, cells_.size(), (unsigned)sizes_.size(), atlasW_, atlasH_,
         (double)atlas_.size() / (1024.0 * 1024.0));
  }
  return added;
}

int RasterFont::bakeMisses() {
  if (misses_.empty()) return 0;

  std::set<uint64_t> todo;
  todo.swap(misses_);

  int added = 0;
  for (uint64_t k : todo) {
    if (cells_.count(k)) continue;                  // ensureGlyphs got there first
    const FontStyle style = (FontStyle)(uint8_t)(k >> 56);
    const int       sz    = (int)((k >> 32) & 0xFFFFFFFFu);
    const uint32_t  cp    = (uint32_t)(k & 0xFFFFFFFFu);

    const Face* use = faceFor(style, cp);
    if (!use || sz <= 0) { unservable_.insert(k); continue; }
    if (bakeCell(*use, style, sz, cp)) {
      sizes_.insert(sz);   // so a later ensureGlyphs covers this size too
      added++;
    } else {
      unservable_.insert(k);
    }
  }

  if (added) {
    VFE_LOGI("Raster", "baked %d missed cells (%zu total) — atlas %ux%u (%.1f MB)",
             added, cells_.size(), atlasW_, atlasH_,
             (double)atlas_.size() / (1024.0 * 1024.0));
  }
  return added;
}

// ── Layout ──────────────────────────────────────────────────────────────────
//
// The pen stays in float so kerned advances accumulate exactly; only the
// POSITION a quad is emitted at is rounded. That is what keeps textWidth()
// and the emitters in agreement — they add the same numbers in the same
// order, and the rounding never feeds back into the pen.

float RasterFont::layoutByKey(uint32_t key, float penX, float baselineY,
                              float sizePx, GlyphQuad& q) const {
  q.draw = false;
  if (key == 0) return penX;

  const FontStyle style = keyStyle(key);
  const uint32_t  cp    = keyCp(key);
  const Cell* c = find(style, quantize(sizePx), cp);
  if (!c) return penX;

  if (c->hasGlyph) {
    const float x = std::round(penX)      + (float)c->bearingX;
    const float y = std::round(baselineY) - (float)c->bearingY;
    q.x0 = x;             q.y0 = y;
    q.x1 = x + (float)c->w; q.y1 = y + (float)c->h;
    q.u0 = (float)c->atlasX / (float)atlasW_;
    q.v0 = (float)c->atlasY / (float)atlasH_;
    q.u1 = (float)(c->atlasX + c->w) / (float)atlasW_;
    q.v1 = (float)(c->atlasY + c->h) / (float)atlasH_;
    q.draw = true;
  }
  return penX + c->advance;
}

float RasterFont::layout(uint32_t cp, float penX, float baselineY, float sizePx,
                         GlyphQuad& q, uint32_t prevCp) const {
  penX += kernEmStyled(FontStyle::Roman, prevCp, cp) * sizePx;
  return layoutByKey(glyphKey(FontStyle::Roman, cp), penX, baselineY, sizePx, q);
}

float RasterFont::advanceKey(uint32_t key, float sizePx) const {
  if (key == 0) return 0.0f;
  const Cell* c = find(keyStyle(key), quantize(sizePx), keyCp(key));
  return c ? c->advance : 0.0f;
}

float RasterFont::advance(uint32_t cp, float sizePx) const {
  return advanceKey(glyphKey(FontStyle::Roman, cp), sizePx);
}

float RasterFont::textWidth(std::string_view s, float sizePx) const {
  float w = 0.0f;
  uint32_t prev = 0;
  for (size_t i = 0; i < s.size(); ) {
    const uint32_t cp = utf8::nextCodepoint(s, i);
    w += kernEmStyled(FontStyle::Roman, prev, cp) * sizePx;
    w += advance(cp, sizePx);
    prev = cp;
  }
  return w;
}

float RasterFont::emitGlyph(std::vector<float>& out, uint32_t cp, float penX,
                            float baselineY, float sizePx,
                            float r, float g, float b, float a,
                            uint32_t prevCp) const {
  GlyphQuad q;
  const float nextX = layout(cp, penX, baselineY, sizePx, q, prevCp);
  if (!q.draw) return nextX;

  const float v[VERTS_PER_GLYPH][FLOATS_PER_VERT] = {
    { q.x0, q.y0, q.u0, q.v0, r, g, b, a },
    { q.x1, q.y0, q.u1, q.v0, r, g, b, a },
    { q.x1, q.y1, q.u1, q.v1, r, g, b, a },
    { q.x0, q.y0, q.u0, q.v0, r, g, b, a },
    { q.x1, q.y1, q.u1, q.v1, r, g, b, a },
    { q.x0, q.y1, q.u0, q.v1, r, g, b, a },
  };
  out.insert(out.end(), &v[0][0], &v[0][0] + VERTS_PER_GLYPH * FLOATS_PER_VERT);
  return nextX;
}
