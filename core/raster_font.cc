#include "raster_font.hh"

#include "log.hh"
#include "utf8.hh"


#include <chrono>
#include <cmath>
#include <cstring>

namespace {

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

  styles_[(int)FontStyle::Roman] = std::move(f);
  return true;
}

bool RasterFont::addStyle(AssetReader& reader, const char* fontPath,
                          FontStyle style) {
  auto f = std::make_unique<Face>();
  if (!loadFace(reader, fontPath, f->raster, f->kern)) return false;
  styles_[(int)style] = std::move(f);
  return true;
}

bool RasterFont::addFallback(AssetReader& reader, const char* fontPath,
                             FontStyle style) {
  auto f = std::make_unique<Face>();
  if (!loadFace(reader, fontPath, f->raster, f->kern)) return false;
  fallbacks_[(int)style].push_back(std::move(f));
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
  auto covers = [cp](const Face* f) {
    return f && f->raster.isOpen() && f->raster.hasCodepoint(cp);
  };
  auto firstIn = [&](const std::vector<std::unique_ptr<Face>>& v) -> const Face* {
    for (const auto& f : v)
      if (covers(f.get())) return f.get();
    return nullptr;
  };

  // Overrides first — see addOverride() for the PUA collision this exists for.
  if (const Face* f = firstIn(overrides_)) return f;

  // THE STYLE'S OWN FACE BEFORE ITS FALLBACKS, always. Every bundled CJK face
  // also carries Latin, Cyrillic and Greek, so reversing these two lines would
  // hand Latin text to Fandol Song and quietly replace the app's typeface.
  if (covers(styles_[(int)style].get())) return styles_[(int)style].get();
  if (const Face* f = firstIn(fallbacks_[(int)style])) return f;

  // Then the default face and its chain, so a style with no cut for this
  // script degrades to the regular weight rather than to nothing.
  if (style != FontStyle::Roman) {
    if (covers(styles_[(int)FontStyle::Roman].get()))
      return styles_[(int)FontStyle::Roman].get();
    if (const Face* f = firstIn(fallbacks_[(int)FontStyle::Roman])) return f;
  }
  return nullptr;
}

bool RasterFont::hasCodepoint(uint32_t cp) const {
  return faceFor(FontStyle::Roman, cp) != nullptr;
}

uint32_t RasterFont::keyForStyle(FontStyle s, uint32_t cp) const {
  if (s == FontStyle::Roman) return glyphKey(s, cp);

  // A styled key is worth having only when this style resolves to a DIFFERENT
  // face than Roman would. Otherwise return 0, which sends the codepoint down
  // Canvas's default-face path (canvas.cc) and shares the Roman cell.
  //
  // This is not a micro-optimization, it is what keeps the atlas honest.
  // Italic and Mono have no CJK counterpart — these scripts have no italic
  // tradition, and faking one by skewing is worse than not having it — so they
  // resolve to the very same regular face Roman does. Without this test, every
  // CJK glyph would be baked a second and third time, byte-identical, under
  // the Italic and Math keys.
  const Face* styled = faceFor(s, cp);
  if (!styled || styled == faceFor(FontStyle::Roman, cp)) return 0;
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

bool RasterFont::packInto(int w, int h, vfe::ShelfPacker::Slot& out) {
  const uint32_t gw = (uint32_t)w, gh = (uint32_t)h;

  // "Too big for any page" says something about the glyph and is worth
  // reporting; "this page is full" is routine and simply opens another. Only
  // the first can fail now, which is the point of paging.
  if (!packer_.couldFit(gw, gh)) {
    VFE_LOGE("Raster", "Raster: glyph %ux%u exceeds a %ux%u page — not baked",
             gw, gh, packer_.pageW(), packer_.pageH());
    return false;
  }
  if (!packer_.place(gw, gh, out)) {
    VFE_LOGE("Raster", "Raster: cannot place a %ux%u glyph", gw, gh);
    return false;
  }

  // Make the backing store cover every page the packer has opened. Pages are
  // whole and fixed-size — they are array layers, which must all match — so
  // this grows a page at a time rather than a shelf at a time.
  const size_t need = (size_t)packer_.pageCount() * kPageW * kPageH;
  if (atlas_.size() < need) atlas_.resize(need, 0);
  dirtyPages_.insert(out.page);   // the caller is about to write into it
  return true;
}

// Byte offset of (page, x, y) in the page-major backing store.
size_t RasterFont::texelOffset(uint32_t page, uint32_t x, uint32_t y) const {
  return (size_t)page * kPageW * kPageH + (size_t)y * kPageW + x;
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
    vfe::ShelfPacker::Slot slot;
    if (!packInto(g.w, g.h, slot)) return false;
    c.hasGlyph = true;
    c.page   = slot.page;
    c.atlasX = slot.x;
    c.atlasY = slot.y;
    c.w = g.w;
    c.h = g.h;
    for (int y = 0; y < g.h; ++y) {
      std::memcpy(atlas_.data() + texelOffset(c.page, c.atlasX, c.atlasY + y),
                  g.cov.data() + (size_t)y * g.w, (size_t)g.w);
    }
  }
  // else: whitespace. Recorded with hasGlyph=false so layout advances the pen
  // without emitting a quad — the same contract MsdfGlyph::hasGlyph carries.

  cells_.emplace(cellKey(style, sizePx, cp), c);
  return true;
}

bool RasterFont::valid() const { return hasStyle(FontStyle::Roman); }

void RasterFont::reserveSolidTexel() {
  if (solidOk_) return;
  vfe::ShelfPacker::Slot slot;
  if (!packInto((int)kSolidPx, (int)kSolidPx, slot)) return;
  for (uint32_t r = 0; r < kSolidPx; ++r)
    std::memset(atlas_.data() + texelOffset(slot.page, slot.x, slot.y + r),
                0xFF, kSolidPx);
  solidX_ = slot.x;
  solidY_ = slot.y;
  solidOk_ = true;   // always page 0: reserved before any glyph
}

bool RasterFont::solidTexel(float& u, float& v) const {
  if (!solidOk_) return false;
  // The centre of the 2x2 block: a texel CORNER, so a linear fetch averages
  // four opaque texels and never reaches the padding around them.
  u = (float)(solidX_ + kSolidPx * 0.5f) / (float)kPageW;
  v = (float)(solidY_ + kSolidPx * 0.5f) / (float)kPageH;
  return true;
}

void RasterFont::takeDirtyPages(std::vector<uint32_t>& out) const {
  out.assign(dirtyPages_.begin(), dirtyPages_.end());
  dirtyPages_.clear();
}

void RasterFont::reset() {
  cells_.clear();
  sizes_.clear();
  misses_.clear();
  unservable_.clear();
  atlas_.clear();
  packer_.reset();
  dirtyPages_.clear();
  solidOk_ = false;
  solidX_ = solidY_ = 0;
}

int RasterFont::ensureGlyphs(const std::vector<uint32_t>& cps,
                             const std::vector<int>& sizesPx) {
  for (int s : sizesPx)
    if (s > 0) sizes_.insert(quantize((float)s));
  if (sizes_.empty() || !styles_[(int)FontStyle::Roman]) return 0;

  reserveSolidTexel();   // first, so its position never moves
  const auto t0 = std::chrono::steady_clock::now();

  // Cells are laid out in the sheet as they are baked, so an all-sizes pass
  // over one codepoint keeps that codepoint's variants near each other. That
  // is not required for correctness; it just keeps a shelf from mixing a 19px
  // Latin cell with a 44px CJK one and wasting the height difference.
  int added = 0;
  for (int style = 0; style < kFontStyleCount; ++style) {
    const Face* sf = styles_[style].get();
    if (!sf || !sf->raster.isOpen()) continue;

    for (uint32_t cp : cps) {
      // Bake a styled cell only where keyForStyle() will actually hand one
      // out — i.e. where this style has a genuinely different face for this
      // codepoint. Everywhere else Canvas draws from the Roman cell, so
      // baking here would produce an identical copy nothing ever samples.
      if ((FontStyle)style != FontStyle::Roman &&
          keyForStyle((FontStyle)style, cp) == 0)
        continue;
      const Face* use = faceFor((FontStyle)style, cp);
      if (!use) continue;

      for (int sz : sizes_) {
        if (cells_.count(cellKey((FontStyle)style, sz, cp))) continue;
        if (bakeCell(*use, (FontStyle)style, sz, cp)) added++;
      }
    }
  }

  if (added) {
    VFE_LOGI("Raster", "Raster: baked %d cells (%zu total) at %u sizes — %u pages (%.1f MB) in %.0f ms",
         added, cells_.size(), (unsigned)sizes_.size(), packer_.pageCount(),
         (double)atlas_.size() / (1024.0 * 1024.0),
         std::chrono::duration<double, std::milli>(
             std::chrono::steady_clock::now() - t0).count());
  }
  return added;
}

// A missed size is served, NOT adopted.
//
// This used to do `sizes_.insert(sz)` on every successful bake, so that a
// later ensureGlyphs() would cover the new size too. That is backwards: the
// sizes that arrive here are the incidental ones — an icon box derived from
// layout geometry, the art window's own scale, a size seen once during a
// resize — and adopting one promotes it into the eager cross product forever.
// The whole codepoint set then gets baked at it, at every later refresh, for
// the rest of the session. The miss path already serves those sizes exactly
// when they are asked for, which is the entire reason it exists.
int RasterFont::bakeMisses() {
  if (misses_.empty()) return 0;

  std::set<uint64_t> todo;
  todo.swap(misses_);

  int added = 0;
  for (uint64_t k : todo) {
    if (cells_.count(k)) continue;                  // ensureGlyphs got there first
    const vfe::cellkey::CellFields f = vfe::cellkey::decodeCell(k);
    const FontStyle style = f.style;
    const int       sz    = (int)f.sizePx;
    const uint32_t  cp    = f.cp;

    const Face* use = faceFor(style, cp);
    if (!use || sz <= 0) { unservable_.insert(k); continue; }
    if (bakeCell(*use, style, sz, cp)) {
      added++;
    } else {
      unservable_.insert(k);
    }
  }

  if (added) {
    VFE_LOGI("Raster", "baked %d missed cells (%zu total) — %u pages (%.1f MB)",
             added, cells_.size(), packer_.pageCount(),
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
    // Normalised against the CONSTANT page size, never a growing high-water
    // mark. That is what makes a quad built earlier in the frame stay correct
    // when a later bake adds a page: existing cells never move, and the divisor
    // never changes.
    q.u0 = (float)c->atlasX / (float)kPageW;
    q.v0 = (float)c->atlasY / (float)kPageH;
    q.u1 = (float)(c->atlasX + c->w) / (float)kPageW;
    q.v1 = (float)(c->atlasY + c->h) / (float)kPageH;
    q.page = c->page;
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

  const float p = (float)q.page;
  const float v[VERTS_PER_GLYPH][FLOATS_PER_VERT] = {
    { q.x0, q.y0, q.u0, q.v0, r, g, b, a, p },
    { q.x1, q.y0, q.u1, q.v0, r, g, b, a, p },
    { q.x1, q.y1, q.u1, q.v1, r, g, b, a, p },
    { q.x0, q.y0, q.u0, q.v0, r, g, b, a, p },
    { q.x1, q.y1, q.u1, q.v1, r, g, b, a, p },
    { q.x0, q.y1, q.u0, q.v1, r, g, b, a, p },
  };
  out.insert(out.end(), &v[0][0], &v[0][0] + VERTS_PER_GLYPH * FLOATS_PER_VERT);
  return nextX;
}
