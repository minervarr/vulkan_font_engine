#include "msdf.hh"

#include "utf8.hh"
#include "log.hh"

#include <atomic>
#include <cstring>
#include <thread>
#include <utility>

#define LOGI(...) VFE_LOGI("Msdf", __VA_ARGS__)
#define LOGE(...) VFE_LOGE("Msdf", __VA_ARGS__)

namespace {
std::vector<uint8_t> readAsset(AssetReader& reader, const char* path) {
  std::vector<uint8_t> buf;
  if (!reader.read(path, buf) || buf.empty()) {
    LOGE("cannot open asset %s", path);
    buf.clear();
  }
  return buf;
}

// Little-endian readers over a byte cursor.
uint32_t rdU32(const uint8_t*& p) { uint32_t v; std::memcpy(&v, p, 4); p += 4; return v; }
float    rdF32(const uint8_t*& p) { float v;    std::memcpy(&v, p, 4); p += 4; return v; }

// Fan `work(i)` for i in [0, count) across hardware threads. Used for the
// per-cell msdfgen::generateMSDF raster loops: each cell's distance field is
// independent computation and each writes a disjoint atlas rect, so the only
// requirement on callers is that `work` touches no shared mutable state
// beyond those disjoint pixels. FreeType glyph loading must NOT go through
// here (FT_Face is not thread-safe) — load shapes serially first, then bulk-
// rasterize. std::thread rather than std::execution because the NDK's libc++
// doesn't ship the parallel algorithms (this file also builds for Android).
template <typename Fn>
void parallelForCells(size_t count, const Fn& work) {
  unsigned hw = std::thread::hardware_concurrency();
  size_t nThreads = hw ? hw : 4;
  if (nThreads > count) nThreads = count;
  if (nThreads <= 1) {
    for (size_t i = 0; i < count; i++) work(i);
    return;
  }
  std::atomic<size_t> next{0};
  std::vector<std::thread> pool;
  pool.reserve(nThreads);
  for (size_t t = 0; t < nThreads; t++) {
    pool.emplace_back([&]() {
      for (size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < count; )
        work(i);
    });
  }
  for (auto& th : pool) th.join();
}
}  // namespace

// font.msdf v2/v3 (magic 0x4644534D): multi-font, GID-addressed, with the
// OpenType MATH payload. See tools/atlas_gen/atlas_gen.cc for the writer; keep
// the field order in lock-step. v3 == v2 byte-for-byte except the version word
// and the atlas sheet's alpha channel: v2's alpha is a constant 255, v3's is a
// TRUE single-channel SDF (MTSDF, same as the runtime v9 cache) — consumers
// gate alpha use on isMtsdf(). Glyphs are keyed by (fontId<<24)|gid; codepoints
// reach text glyphs (font 0) through the fast/hash maps and math glyphs (font 2)
// through mathCmap_.
static constexpr uint32_t kFontText = 0;
static constexpr uint32_t kFontMath = 2;

bool MsdfFont::load(AssetReader& reader, const char* metricsPath,
                    const char* atlasPath) {
  std::vector<uint8_t> meta = readAsset(reader, metricsPath);
  if (meta.size() < 40) { LOGE("metrics too small"); return false; }

  const uint8_t* p = meta.data();
  uint32_t magic = rdU32(p);
  if (magic != 0x4644534D) { LOGE("bad magic %08x", magic); return false; }
  uint32_t version = rdU32(p);
  if (version != 2 && version != 3) {
    LOGE("unsupported font.msdf version %u (want 2 or 3)", version);
    return false;
  }
  isMtsdf_ = (version == 3);  // v3: sheet alpha is a true SDF (MTSDF)
  atlasW_        = rdU32(p);
  atlasH_        = rdU32(p);
  distanceRange_ = rdF32(p);
  sizePxEm_      = rdF32(p);
  lineHeight_    = rdF32(p);
  ascender_      = rdF32(p);
  descender_     = rdF32(p);

  // MathConstants block.
  uint32_t cn = rdU32(p);
  for (uint32_t i = 0; i < cn; i++) { float v = rdF32(p); if (i < 42) mc_.v[i] = v; }

  // Glyphs (by key).
  uint32_t gcount = rdU32(p);
  for (uint32_t i = 0; i < gcount; i++) {
    uint32_t key = rdU32(p);
    MsdfGlyph g;
    g.advance  = rdF32(p);
    g.italic   = rdF32(p);
    g.hasGlyph = rdU32(p) != 0;
    g.planeL = rdF32(p); g.planeT = rdF32(p); g.planeR = rdF32(p); g.planeB = rdF32(p);
    g.atlasL = rdF32(p); g.atlasT = rdF32(p); g.atlasR = rdF32(p); g.atlasB = rdF32(p);
    byKey_[key] = g;
  }

  // Cmap entry points → resolve codepoints. Every entry feeds its style's map
  // (styleCmap_[style]); the Roman face additionally feeds the fast/hash path used
  // by the default Canvas::text, and the Math face fills any codepoint the text
  // faces lack (π, θ, …) so default text never renders a blank box.
  uint32_t ccount = rdU32(p);
  for (uint32_t i = 0; i < ccount; i++) {
    uint32_t cp   = rdU32(p);
    uint32_t font = rdU32(p);
    uint32_t key  = rdU32(p);
    if (font < (uint32_t)kFontStyleCount) styleCmap_[font][cp] = key;

    if (font == kFontText) {
      auto it = byKey_.find(key);
      if (it != byKey_.end()) {
        glyphs_[cp] = it->second;
        if (cp < kFastCount) { fast_[cp] = it->second; fastHas_[cp] = true; }
      }
    } else if (font == kFontMath) {
      // Fallback for the default codepoint path: only fills genuine gaps (Roman
      // entries overwrite unconditionally above, so text always wins on conflict).
      if (glyphs_.find(cp) == glyphs_.end()) {
        auto it = byKey_.find(key);
        if (it != byKey_.end()) {
          glyphs_[cp] = it->second;
          if (cp < kFastCount) { fast_[cp] = it->second; fastHas_[cp] = true; }
        }
      }
    }
  }

  // Vertical constructions (radicals, delimiters, big operators).
  uint32_t vcount = rdU32(p);
  for (uint32_t i = 0; i < vcount; i++) {
    MathConstruction mc;
    mc.baseKey = rdU32(p);
    mc.italic  = rdF32(p);
    uint32_t nv = rdU32(p);
    uint32_t np = rdU32(p);
    mc.variants.reserve(nv);
    for (uint32_t v = 0; v < nv; v++) {
      MathVariant mv;
      mv.key = rdU32(p);
      mv.advance = rdF32(p);
      mc.variants.push_back(mv);
    }
    mc.parts.reserve(np);
    for (uint32_t pp = 0; pp < np; pp++) {
      MathPart part;
      part.key   = rdU32(p);
      part.start = rdF32(p);
      part.end   = rdF32(p);
      part.full  = rdF32(p);
      part.ext   = rdU32(p) != 0;
      mc.parts.push_back(part);
    }
    constr_[mc.baseKey] = std::move(mc);
  }
  hasMath_ = (cn > 0 || vcount > 0);

  atlas_ = readAsset(reader, atlasPath);
  if (atlas_.size() != (size_t)atlasW_ * atlasH_ * 4) {
    LOGE("atlas size %zu != %ux%u*4", atlas_.size(), atlasW_, atlasH_);
    atlas_.clear();
    return false;
  }
  LOGI("MSDF v2: %u glyphs, %u cmap, %u constructions, math=%d, atlas %ux%u",
       gcount, ccount, vcount, (int)hasMath_, atlasW_, atlasH_);
  return true;
}

float MsdfFont::layoutByKey(uint32_t key, float penX, float baselineY, float sizePx,
                            GlyphQuad& q) const {
  q.draw = false;
  const MsdfGlyph* g = glyphByKey(key);
  if (!g) return penX;
  const MsdfGlyph& gl = *g;
  if (!gl.hasGlyph) return penX + gl.advance * sizePx;
  q.x0 = penX + gl.planeL * sizePx;
  q.x1 = penX + gl.planeR * sizePx;
  q.y0 = baselineY + gl.planeT * sizePx;
  q.y1 = baselineY + gl.planeB * sizePx;
  q.u0 = gl.atlasL / atlasW_; q.u1 = gl.atlasR / atlasW_;
  q.v0 = gl.atlasT / atlasH_; q.v1 = gl.atlasB / atlasH_;
  q.draw = true;
  return penX + gl.advance * sizePx;
}

VStretch MsdfFont::buildVStretch(const MathConstruction& c, float targetEm) const {
  VStretch out;
  // 1) Smallest pre-built variant that already reaches the target (seam-free).
  for (const MathVariant& v : c.variants) {
    if (v.advance >= targetEm) { out.single = true; out.key = v.key; out.heightEm = v.advance; return out; }
  }
  // 2) Assemble from parts (bottom→top), repeating extenders until tall enough.
  //    Overlap each joint by minConnectorOverlap (valid: ≥ min, ≤ the joint max),
  //    which gives the most height per part.
  if (!c.parts.empty()) {
    const float ov = mc_.minConnectorOverlap();
    auto build = [&](int reps) {
      VStretch r; r.single = false; r.parts.clear();
      float cursorTop = 0.0f; bool first = true;
      for (const MathPart& part : c.parts) {
        int count = part.ext ? reps : 1;
        for (int i = 0; i < count; i++) {
          float bottom = first ? 0.0f : (cursorTop - ov);
          r.parts.push_back({part.key, bottom, part.full});
          cursorTop = bottom + part.full;
          first = false;
        }
      }
      r.heightEm = cursorTop;
      return r;
    };
    VStretch r = build(0);            // minimal form: non-extenders only
    int reps = 0, guard = 0;
    while (r.heightEm < targetEm && guard++ < 512) { reps++; r = build(reps); }
    if (!r.parts.empty()) return r;
  }
  // 3) Fallback: largest variant, or the base glyph at face size.
  if (!c.variants.empty()) {
    out.single = true; out.key = c.variants.back().key; out.heightEm = c.variants.back().advance;
  } else {
    out.single = true; out.key = c.baseKey; out.heightEm = targetEm;
  }
  return out;
}

#include <msdfgen.h>
#include <msdfgen-ext.h>
#include <cmath>

#include <fstream>

bool MsdfFont::loadCache(const char* cachePath) {
  if (!cachePath) return false;
  std::ifstream f(cachePath, std::ios::binary);
  if (!f) return false;

  uint32_t magic = 0;
  f.read(reinterpret_cast<char*>(&magic), sizeof(magic));
  if (magic != 0x4D534446) return false; // 'MSDF'
  uint32_t version = 0;
  f.read(reinterpret_cast<char*>(&version), sizeof(version));
  if (version != 9) return false;  // v9: bottom-anchored planes, EM 40->96, AW 4096; v8 was MTSDF

  f.read(reinterpret_cast<char*>(&atlasW_), sizeof(atlasW_));
  f.read(reinterpret_cast<char*>(&atlasH_), sizeof(atlasH_));
  f.read(reinterpret_cast<char*>(&sizePxEm_), sizeof(sizePxEm_));
  f.read(reinterpret_cast<char*>(&distanceRange_), sizeof(distanceRange_));

  uint32_t atlasSize = 0;
  f.read(reinterpret_cast<char*>(&atlasSize), sizeof(atlasSize));
  atlas_.resize(atlasSize);
  if (atlasSize > 0) {
    f.read(reinterpret_cast<char*>(atlas_.data()), atlasSize);
  }

  uint32_t glyphCount = 0;
  f.read(reinterpret_cast<char*>(&glyphCount), sizeof(glyphCount));
  for (uint32_t i = 0; i < glyphCount; ++i) {
    uint32_t cp;
    MsdfGlyph g;
    f.read(reinterpret_cast<char*>(&cp), sizeof(cp));
    f.read(reinterpret_cast<char*>(&g), sizeof(g));
    glyphs_[cp] = g;
    if (cp < kFastCount) {
      fast_[cp] = g;
      fastHas_[cp] = true;
    }
  }

  // addStyle()'s data: byKey_ (all styles) + styleCmap_ per style.
  uint32_t byKeyCount = 0;
  f.read(reinterpret_cast<char*>(&byKeyCount), sizeof(byKeyCount));
  for (uint32_t i = 0; i < byKeyCount; ++i) {
    uint32_t key;
    MsdfGlyph g;
    f.read(reinterpret_cast<char*>(&key), sizeof(key));
    f.read(reinterpret_cast<char*>(&g), sizeof(g));
    byKey_[key] = g;
  }
  for (int s = 0; s < kFontStyleCount; ++s) {
    uint32_t cmapCount = 0;
    f.read(reinterpret_cast<char*>(&cmapCount), sizeof(cmapCount));
    for (uint32_t i = 0; i < cmapCount; ++i) {
      uint32_t cp, key;
      f.read(reinterpret_cast<char*>(&cp), sizeof(cp));
      f.read(reinterpret_cast<char*>(&key), sizeof(key));
      styleCmap_[s][cp] = key;
    }
  }

  isMtsdf_ = f.good();  // v8 cache => MTSDF atlas (alpha = true SDF)
  return f.good();
}

bool MsdfFont::ensureAtlasLoaded(const char* cachePath) {
  if (!atlas_.empty()) return true;
  if (!cachePath) return false;
  std::ifstream f(cachePath, std::ios::binary);
  if (!f) return false;

  // Same header walk as loadCache(), but only the atlas block is consumed —
  // the glyph tables are still resident from the original load/generate.
  uint32_t magic = 0, version = 0;
  f.read(reinterpret_cast<char*>(&magic), sizeof(magic));
  f.read(reinterpret_cast<char*>(&version), sizeof(version));
  if (magic != 0x4D534446 || version != 9) return false;

  uint32_t w = 0, h = 0;
  float em = 0, dr = 0;
  f.read(reinterpret_cast<char*>(&w), sizeof(w));
  f.read(reinterpret_cast<char*>(&h), sizeof(h));
  f.read(reinterpret_cast<char*>(&em), sizeof(em));
  f.read(reinterpret_cast<char*>(&dr), sizeof(dr));
  // The cache must describe the atlas this font's resident metrics point
  // into — a stale/foreign cache would give glyphs wrong UVs.
  if (w != atlasW_ || h != atlasH_) return false;

  uint32_t atlasSize = 0;
  f.read(reinterpret_cast<char*>(&atlasSize), sizeof(atlasSize));
  if (atlasSize != atlasW_ * atlasH_ * 4u) return false;
  atlas_.resize(atlasSize);
  f.read(reinterpret_cast<char*>(atlas_.data()), atlasSize);
  if (!f.good()) { atlas_.clear(); return false; }
  return true;
}

bool MsdfFont::saveCache(const char* cachePath) {
  if (!cachePath) return false;
  std::ofstream f(cachePath, std::ios::binary | std::ios::trunc);
  if (!f) return false;

  uint32_t magic = 0x4D534446;
  uint32_t version = 9;  // v9: bottom-anchored planes, EM 40->96, AW 4096
  f.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
  f.write(reinterpret_cast<const char*>(&version), sizeof(version));
  f.write(reinterpret_cast<const char*>(&atlasW_), sizeof(atlasW_));
  f.write(reinterpret_cast<const char*>(&atlasH_), sizeof(atlasH_));
  f.write(reinterpret_cast<const char*>(&sizePxEm_), sizeof(sizePxEm_));
  f.write(reinterpret_cast<const char*>(&distanceRange_), sizeof(distanceRange_));

  uint32_t atlasSize = atlas_.size();
  f.write(reinterpret_cast<const char*>(&atlasSize), sizeof(atlasSize));
  if (atlasSize > 0) {
    f.write(reinterpret_cast<const char*>(atlas_.data()), atlasSize);
  }

  uint32_t glyphCount = glyphs_.size();
  f.write(reinterpret_cast<const char*>(&glyphCount), sizeof(glyphCount));
  for (const auto& pair : glyphs_) {
    uint32_t cp = pair.first;
    MsdfGlyph g = pair.second;
    f.write(reinterpret_cast<const char*>(&cp), sizeof(cp));
    f.write(reinterpret_cast<const char*>(&g), sizeof(g));
  }

  uint32_t byKeyCount = byKey_.size();
  f.write(reinterpret_cast<const char*>(&byKeyCount), sizeof(byKeyCount));
  for (const auto& pair : byKey_) {
    uint32_t key = pair.first;
    MsdfGlyph g = pair.second;
    f.write(reinterpret_cast<const char*>(&key), sizeof(key));
    f.write(reinterpret_cast<const char*>(&g), sizeof(g));
  }
  for (int s = 0; s < kFontStyleCount; ++s) {
    uint32_t cmapCount = styleCmap_[s].size();
    f.write(reinterpret_cast<const char*>(&cmapCount), sizeof(cmapCount));
    for (const auto& pair : styleCmap_[s]) {
      uint32_t cp = pair.first, key = pair.second;
      f.write(reinterpret_cast<const char*>(&cp), sizeof(cp));
      f.write(reinterpret_cast<const char*>(&key), sizeof(key));
    }
  }

  return f.good();
}

bool MsdfFont::generate(AssetReader& reader, const char* fontPath, const char* cachePath) {
  if (cachePath && loadCache(cachePath)) {
    LOGI("MsdfFont::generate loaded from cache: %s", cachePath);
    return true;
  }

  LOGI("MsdfFont::generate starting for %s", fontPath);
  std::vector<uint8_t> fontData = readAsset(reader, fontPath);
  if (fontData.empty()) { LOGI("readAsset failed"); return false; }

  msdfgen::FreetypeHandle* ft = msdfgen::initializeFreetype();
  if (!ft) { LOGI("initializeFreetype failed"); return false; }
  msdfgen::FontHandle* fontHandle = msdfgen::loadFontData(ft, fontData.data(), fontData.size());
  if (!fontHandle) { LOGI("loadFontData failed"); msdfgen::deinitializeFreetype(ft); return false; }
  LOGI("Font loaded successfully, parsing metrics");
  msdfgen::FontMetrics metrics;
  msdfgen::getFontMetrics(metrics, fontHandle);
  LOGI("Metrics: emSize=%f", metrics.emSize);
  
  // Atlas texels per em. The fragment shader (msdf_frag.slang) takes a single
  // bilinear tap per pixel — correct for magnification (MSDF's whole point),
  // but a high EM baked for a use case that shows big magnified text (e.g.
  // the offline atlas_gen baker's calculator display) becomes a MINIFICATION
  // problem here: this app only ever draws 11-18px UI text, so EM=100 meant
  // every glyph was sampled at ~6-9x minification, which a single bilinear
  // tap can't represent without shimmering/aliasing ("low quality" text,
  // worse the more of it is on screen at once). 96 balances both ends for
  // hosts that show large magnified text (a maximized desktop window draws
  // ~120px lines, which at the old EM=40 was ~3x past the baked detail —
  // visibly blocky curves): small 11-18px UI text sits at a manageable
  // ~5-8x minification (the MTSDF alpha/true-SDF blend in msdf_frag.slang
  // handles that regime), while 120px text is only mild ~1.25x magnification.
  sizePxEm_ = 96.0f;
  distanceRange_ = sizePxEm_ * 0.1f;  // matches atlas_gen's RANGE=EM/10 convention
  lineHeight_ = metrics.lineHeight / metrics.emSize;
  ascender_ = metrics.ascenderY / metrics.emSize;
  descender_ = metrics.descenderY / metrics.emSize;

  std::vector<uint32_t> chars;
  for (uint32_t c = 0x20; c <= 0x7E; c++) chars.push_back(c);
  for (uint32_t c = 0xA1; c <= 0xFF; c++) chars.push_back(c);
  uint32_t extras[] = {0x152, 0x153, 0x178, 0x20AC, 0x2212, 0x221A, 0x03C0, 0x03B8};
  for (uint32_t c : extras) chars.push_back(c);

  LOGI("Building %zu cells", chars.size());

  struct Cell { uint32_t cp; msdfgen::Shape shape; msdfgen::Shape::Bounds bounds; int w, h, ax, ay; double advance; };
  std::vector<Cell> cells;

  for (uint32_t cp : chars) {
    msdfgen::Shape shape;
    double advance = 0;
    if (!msdfgen::loadGlyph(shape, fontHandle, cp, &advance)) {
      MsdfGlyph g; g.advance = advance / metrics.emSize; g.hasGlyph = false;
      glyphs_[cp] = g;
      if (cp < kFastCount) fast_[cp] = g, fastHas_[cp] = true;
      continue;
    }
    shape.normalize();
    // CFF/PostScript-flavoured OTFs wind their outer contours opposite to
    // TrueType, which is the orientation msdfgen's inside-positive convention
    // assumes. Without this the whole field is sign-inverted (transparent glyph
    // on an opaque/white cell). orientContours() scanline-detects each contour's
    // true orientation and reverses the ones that disagree. Must run before
    // edgeColoringSimple() (which colours edges by corner, sensitive to order).
    shape.orientContours();
    msdfgen::edgeColoringSimple(shape, 3.0);
    msdfgen::Shape::Bounds bounds = shape.getBounds();
    double scale = sizePxEm_ / metrics.emSize;
    int w = 0, h = 0;
    if (bounds.l < bounds.r && bounds.b < bounds.t) {
      w = std::ceil((bounds.r - bounds.l) * scale + 2.0 * distanceRange_);
      h = std::ceil((bounds.t - bounds.b) * scale + 2.0 * distanceRange_);
    } else {
      bounds.l = bounds.b = bounds.r = bounds.t = 0;
    }
    cells.push_back({cp, shape, bounds, w, h, 0, 0, advance});
  }

  // 4096 is the minimum maxImageDimension2D every Vulkan device guarantees —
  // the sheet must never exceed it in either dimension.
  int curX = 0, curY = 0, rowH = 0, AW = 4096;
  for (auto& c : cells) {
    if (curX + c.w > AW) { curX = 0; curY += rowH + 2; rowH = 0; }
    c.ax = curX; c.ay = curY;
    curX += c.w + 2;
    if (c.h > rowH) rowH = c.h;
  }
  atlasW_ = AW;
  atlasH_ = curY + rowH;
  if (atlasH_ > 4096) {
    LOGE("MSDF atlas height %u exceeds the 4096 guaranteed texture limit", atlasH_);
    msdfgen::destroyFont(fontHandle);
    msdfgen::deinitializeFreetype(ft);
    return false;
  }
  atlas_.resize(atlasW_ * atlasH_ * 4, 0);

  // Raster phase runs across all cores (see parallelForCells): each cell
  // computes its own distance field and writes only its own atlas rect.
  // Metadata insertion (the glyphs_/fast_ maps) stays serial below.
  parallelForCells(cells.size(), [&](size_t ci) {
    auto& c = cells[ci];
    if (c.w <= 0 || c.h <= 0) return;
    msdfgen::Bitmap<float, 4> msdf(c.w, c.h);
    double scale = sizePxEm_ / metrics.emSize;
    msdfgen::Vector2 translate(-c.bounds.l + distanceRange_/scale, -c.bounds.b + distanceRange_/scale);
    msdfgen::generateMTSDF(msdf, c.shape, distanceRange_ / scale, msdfgen::Vector2(scale), translate);

    for (int y = 0; y < c.h; y++) {
      for (int x = 0; x < c.w; x++) {
        int dst = ((c.ay + y) * atlasW_ + (c.ax + x)) * 4;
        auto px = msdf(x, c.h - 1 - y); // msdfgen is Y-up, vulkan texture Y-down
        atlas_[dst]   = msdfgen::pixelFloatToByte(px[0]);
        atlas_[dst+1] = msdfgen::pixelFloatToByte(px[1]);
        atlas_[dst+2] = msdfgen::pixelFloatToByte(px[2]);
        // MTSDF: alpha carries the TRUE single-channel SDF (was a wasted
        // constant 255). The shader blends toward it when glyphs are
        // minified below atlas resolution, where median(RGB) speckles.
        atlas_[dst+3] = msdfgen::pixelFloatToByte(px[3]);
      }
    }
  });

  for (auto& c : cells) {
    MsdfGlyph g;
    g.advance = c.advance / metrics.emSize;
    g.hasGlyph = true;
    g.planeL = c.bounds.l / metrics.emSize - distanceRange_ / sizePxEm_;
    // Anchor the plane to the BOTTOM edge, matching the raster translate
    // (-bounds.b + range/scale) above: the ceil() slack in c.h lands at the
    // top of the cell, so deriving planeB from planeT (top-anchored) shifted
    // every glyph's ink up by its per-glyph fractional remainder — visible
    // baseline jitter between adjacent letters.
    g.planeB = -c.bounds.b / metrics.emSize + distanceRange_ / sizePxEm_;
    g.planeR = g.planeL + c.w / sizePxEm_;
    g.planeT = g.planeB - c.h / sizePxEm_;
    g.atlasL = c.ax; g.atlasT = c.ay; g.atlasR = c.ax + c.w; g.atlasB = c.ay + c.h;
    glyphs_[c.cp] = g;
    if (c.cp < kFastCount) fast_[c.cp] = g, fastHas_[c.cp] = true;
  }
  
  if (cachePath) {
    saveCache(cachePath);
  }

  msdfgen::destroyFont(fontHandle);
  msdfgen::deinitializeFreetype(ft);
  isMtsdf_ = true;  // freshly baked via generateMTSDF
  LOGI("Dynamic MTSDF atlas generated: %dx%d", atlasW_, atlasH_);
  return true;
}

bool MsdfFont::addStyle(AssetReader& reader, const char* fontPath, FontStyle style) {
  if (atlas_.empty() || atlasW_ == 0) { LOGE("addStyle: call generate() first"); return false; }

  std::vector<uint8_t> fontData = readAsset(reader, fontPath);
  if (fontData.empty()) return false;

  msdfgen::FreetypeHandle* ft = msdfgen::initializeFreetype();
  if (!ft) return false;
  msdfgen::FontHandle* fontHandle = msdfgen::loadFontData(ft, fontData.data(), fontData.size());
  if (!fontHandle) { msdfgen::deinitializeFreetype(ft); return false; }
  msdfgen::FontMetrics metrics;
  msdfgen::getFontMetrics(metrics, fontHandle);

  std::vector<uint32_t> chars;
  for (uint32_t c = 0x20; c <= 0x7E; c++) chars.push_back(c);
  for (uint32_t c = 0xA1; c <= 0xFF; c++) chars.push_back(c);
  uint32_t extras[] = {0x152, 0x153, 0x178, 0x20AC, 0x2212, 0x221A, 0x03C0, 0x03B8};
  for (uint32_t c : extras) chars.push_back(c);

  struct Cell { uint32_t cp; msdfgen::Shape shape; msdfgen::Shape::Bounds bounds; int w, h, ax, ay; double advance; };
  std::vector<Cell> cells;

  const uint32_t styleId = static_cast<uint32_t>(style);
  double scale = sizePxEm_ / metrics.emSize;

  for (uint32_t cp : chars) {
    msdfgen::Shape shape;
    double advance = 0;
    if (!msdfgen::loadGlyph(shape, fontHandle, cp, &advance)) {
      MsdfGlyph g; g.advance = advance / metrics.emSize; g.hasGlyph = false;
      uint32_t key = (styleId << 24) | cp;
      byKey_[key] = g;
      styleCmap_[styleId][cp] = key;
      continue;
    }
    shape.normalize();
    shape.orientContours();
    msdfgen::edgeColoringSimple(shape, 3.0);
    msdfgen::Shape::Bounds bounds = shape.getBounds();
    int w = 0, h = 0;
    if (bounds.l < bounds.r && bounds.b < bounds.t) {
      w = std::ceil((bounds.r - bounds.l) * scale + 2.0 * distanceRange_);
      h = std::ceil((bounds.t - bounds.b) * scale + 2.0 * distanceRange_);
    } else {
      bounds.l = bounds.b = bounds.r = bounds.t = 0;
    }
    cells.push_back({cp, shape, bounds, w, h, 0, 0, advance});
  }

  // Pack the new cells into fresh rows appended below the existing atlas —
  // existing glyphs (any prior style, including Roman) keep their absolute
  // atlasL/T/R/B pixel coords, which stay valid since row-major data below
  // atlasH_ is untouched by the resize() below.
  int curX = 0, curY = atlasH_, rowH = 0;
  for (auto& c : cells) {
    if (curX + c.w > (int)atlasW_) { curX = 0; curY += rowH + 2; rowH = 0; }
    c.ax = curX; c.ay = curY;
    curX += c.w + 2;
    if (c.h > rowH) rowH = c.h;
  }
  uint32_t newAtlasH = curY + rowH;
  if (newAtlasH > 4096) {
    LOGE("MSDF atlas height %u exceeds the 4096 guaranteed texture limit", newAtlasH);
    msdfgen::destroyFont(fontHandle);
    msdfgen::deinitializeFreetype(ft);
    return false;
  }
  atlas_.resize((size_t)atlasW_ * newAtlasH * 4, 0);

  // Same parallel raster / serial metadata split as generate().
  parallelForCells(cells.size(), [&](size_t ci) {
    auto& c = cells[ci];
    if (c.w <= 0 || c.h <= 0) return;
    msdfgen::Bitmap<float, 4> msdf(c.w, c.h);
    msdfgen::Vector2 translate(-c.bounds.l + distanceRange_/scale, -c.bounds.b + distanceRange_/scale);
    msdfgen::generateMTSDF(msdf, c.shape, distanceRange_ / scale, msdfgen::Vector2(scale), translate);
    for (int y = 0; y < c.h; y++) {
      for (int x = 0; x < c.w; x++) {
        int dst = ((c.ay + y) * (int)atlasW_ + (c.ax + x)) * 4;
        auto px = msdf(x, c.h - 1 - y);
        atlas_[dst]   = msdfgen::pixelFloatToByte(px[0]);
        atlas_[dst+1] = msdfgen::pixelFloatToByte(px[1]);
        atlas_[dst+2] = msdfgen::pixelFloatToByte(px[2]);
        // MTSDF: alpha carries the TRUE single-channel SDF (was a wasted
        // constant 255). The shader blends toward it when glyphs are
        // minified below atlas resolution, where median(RGB) speckles.
        atlas_[dst+3] = msdfgen::pixelFloatToByte(px[3]);
      }
    }
  });

  for (auto& c : cells) {
    MsdfGlyph g;
    g.advance = c.advance / metrics.emSize;
    g.hasGlyph = true;
    g.planeL = c.bounds.l / metrics.emSize - distanceRange_ / sizePxEm_;
    // Bottom-anchored to match the raster translate — see generate().
    g.planeB = -c.bounds.b / metrics.emSize + distanceRange_ / sizePxEm_;
    g.planeR = g.planeL + c.w / sizePxEm_;
    g.planeT = g.planeB - c.h / sizePxEm_;
    g.atlasL = c.ax; g.atlasT = c.ay; g.atlasR = c.ax + c.w; g.atlasB = c.ay + c.h;
    uint32_t key = (styleId << 24) | c.cp;
    byKey_[key] = g;
    styleCmap_[styleId][c.cp] = key;
  }

  atlasH_ = newAtlasH;

  msdfgen::destroyFont(fontHandle);
  msdfgen::deinitializeFreetype(ft);
  LOGI("MSDF style %u added: atlas now %ux%u", styleId, atlasW_, atlasH_);
  return true;
}

int MsdfFont::bakeCodepoints(AssetReader& reader, const char* fontPath,
                             const std::vector<uint32_t>& codepoints) {
  if (atlas_.empty() || atlasW_ == 0) { LOGE("bakeCodepoints: call generate() first"); return 0; }

  std::vector<uint32_t> needed;
  for (uint32_t cp : codepoints)
    if (!hasCodepoint(cp)) needed.push_back(cp);
  if (needed.empty()) return 0;

  std::vector<uint8_t> fontData = readAsset(reader, fontPath);
  if (fontData.empty()) return 0;

  msdfgen::FreetypeHandle* ft = msdfgen::initializeFreetype();
  if (!ft) return 0;
  msdfgen::FontHandle* fontHandle = msdfgen::loadFontData(ft, fontData.data(), fontData.size());
  if (!fontHandle) { msdfgen::deinitializeFreetype(ft); return 0; }
  msdfgen::FontMetrics metrics;
  msdfgen::getFontMetrics(metrics, fontHandle);
  double scale = sizePxEm_ / metrics.emSize;

  struct Cell { uint32_t cp; msdfgen::Shape shape; msdfgen::Shape::Bounds bounds; int w, h; double advance; };
  std::vector<Cell> cells;

  for (uint32_t cp : needed) {
    // msdfgen::loadGlyph(unicode) resolves via FT_Get_Char_Index, then
    // unconditionally FT_Load_Glyphs whatever index comes back -- including
    // index 0 (.notdef) when the codepoint isn't in this font's cmap at all.
    // FT_Load_Glyph(face, 0, ...) never errors, so loadGlyph(unicode) would
    // return true with the font's own .notdef shape, silently baking a wrong
    // placeholder glyph (and, via the newlyBaked>0 caller contract, stopping
    // the fallback-font search before a font that actually has the glyph
    // gets tried). Check glyph coverage explicitly first so a font that
    // doesn't have `cp` is correctly skipped, per this function's documented
    // "leave for the next fallback font" contract below.
    msdfgen::GlyphIndex gi;
    if (!msdfgen::getGlyphIndex(gi, fontHandle, cp) || gi.getIndex() == 0) continue;
    msdfgen::Shape shape;
    double advance = 0;
    if (!msdfgen::loadGlyph(shape, fontHandle, gi, &advance)) continue;  // this font doesn't have it either — leave for the next fallback font (or stays unresolved, same zero-width behavior as a missing lookup())
    shape.normalize();
    shape.orientContours();
    msdfgen::edgeColoringSimple(shape, 3.0);
    msdfgen::Shape::Bounds bounds = shape.getBounds();
    int w = 0, h = 0;
    if (bounds.l < bounds.r && bounds.b < bounds.t) {
      w = std::ceil((bounds.r - bounds.l) * scale + 2.0 * distanceRange_);
      h = std::ceil((bounds.t - bounds.b) * scale + 2.0 * distanceRange_);
    } else {
      bounds.l = bounds.b = bounds.r = bounds.t = 0;
    }
    cells.push_back({cp, shape, bounds, w, h, advance});
  }

  if (cells.empty()) {
    msdfgen::destroyFont(fontHandle);
    msdfgen::deinitializeFreetype(ft);
    return 0;
  }

  // Same row-append packing as addStyle(): new rows below the existing
  // atlas, prior glyphs' absolute atlas coords untouched.
  std::vector<int> ax(cells.size()), ay(cells.size());
  int curX = 0, curY = atlasH_, rowH = 0;
  for (size_t i = 0; i < cells.size(); i++) {
    auto& c = cells[i];
    if (curX + c.w > (int)atlasW_) { curX = 0; curY += rowH + 2; rowH = 0; }
    ax[i] = curX; ay[i] = curY;
    curX += c.w + 2;
    if (c.h > rowH) rowH = c.h;
  }
  uint32_t newAtlasH = curY + rowH;
  if (newAtlasH > 4096) {
    LOGE("MSDF atlas height %u exceeds the 4096 guaranteed texture limit", newAtlasH);
    msdfgen::destroyFont(fontHandle);
    msdfgen::deinitializeFreetype(ft);
    return 0;
  }
  atlas_.resize((size_t)atlasW_ * newAtlasH * 4, 0);

  // Same parallel raster / serial metadata split as generate().
  parallelForCells(cells.size(), [&](size_t i) {
    auto& c = cells[i];
    if (c.w <= 0 || c.h <= 0) return;
    msdfgen::Bitmap<float, 4> msdf(c.w, c.h);
    msdfgen::Vector2 translate(-c.bounds.l + distanceRange_/scale, -c.bounds.b + distanceRange_/scale);
    msdfgen::generateMTSDF(msdf, c.shape, distanceRange_ / scale, msdfgen::Vector2(scale), translate);
    for (int y = 0; y < c.h; y++) {
      for (int x = 0; x < c.w; x++) {
        int dst = ((ay[i] + y) * (int)atlasW_ + (ax[i] + x)) * 4;
        auto px = msdf(x, c.h - 1 - y);
        atlas_[dst]   = msdfgen::pixelFloatToByte(px[0]);
        atlas_[dst+1] = msdfgen::pixelFloatToByte(px[1]);
        atlas_[dst+2] = msdfgen::pixelFloatToByte(px[2]);
        // MTSDF: alpha carries the TRUE single-channel SDF (was a wasted
        // constant 255). The shader blends toward it when glyphs are
        // minified below atlas resolution, where median(RGB) speckles.
        atlas_[dst+3] = msdfgen::pixelFloatToByte(px[3]);
      }
    }
  });

  int newlyBaked = 0;
  for (size_t i = 0; i < cells.size(); i++) {
    auto& c = cells[i];
    MsdfGlyph g;
    g.advance = c.advance / metrics.emSize;
    g.hasGlyph = true;
    g.planeL = c.bounds.l / metrics.emSize - distanceRange_ / sizePxEm_;
    // Bottom-anchored to match the raster translate — see generate().
    g.planeB = -c.bounds.b / metrics.emSize + distanceRange_ / sizePxEm_;
    g.planeR = g.planeL + c.w / sizePxEm_;
    g.planeT = g.planeB - c.h / sizePxEm_;
    g.atlasL = (float)ax[i]; g.atlasT = (float)ay[i];
    g.atlasR = (float)(ax[i] + c.w); g.atlasB = (float)(ay[i] + c.h);
    glyphs_[c.cp] = g;
    if (c.cp < kFastCount) { fast_[c.cp] = g; fastHas_[c.cp] = true; }
    newlyBaked++;
  }

  atlasH_ = newAtlasH;

  msdfgen::destroyFont(fontHandle);
  msdfgen::deinitializeFreetype(ft);
  LOGI("MSDF baked %d fallback codepoints from %s: atlas now %ux%u", newlyBaked, fontPath, atlasW_, atlasH_);
  return newlyBaked;
}

float MsdfFont::advance(uint32_t cp, float sizePx) const {
  const MsdfGlyph* g = lookup(cp);
  return g ? g->advance * sizePx : 0.0f;
}

float MsdfFont::textWidth(std::string_view s, float sizePx) const {
  float w = 0.0f;
  for (size_t i = 0; i < s.size(); ) w += advance(utf8::nextCodepoint(s, i), sizePx);
  return w;
}

float MsdfFont::layout(uint32_t cp, float penX, float baselineY, float sizePx,
                       GlyphQuad& q) const {
  q.draw = false;
  const MsdfGlyph* g = lookup(cp);
  if (!g) return penX;
  const MsdfGlyph& gl = *g;
  if (!gl.hasGlyph) return penX + gl.advance * sizePx;  // e.g. space

  // Screen-space quad (yOrigin top: plane top is above baseline → smaller y).
  q.x0 = penX + gl.planeL * sizePx;
  q.x1 = penX + gl.planeR * sizePx;
  q.y0 = baselineY + gl.planeT * sizePx;
  q.y1 = baselineY + gl.planeB * sizePx;
  q.u0 = gl.atlasL / atlasW_; q.u1 = gl.atlasR / atlasW_;
  q.v0 = gl.atlasT / atlasH_; q.v1 = gl.atlasB / atlasH_;
  q.draw = true;
  return penX + gl.advance * sizePx;
}

float MsdfFont::emitGlyph(std::vector<float>& out, uint32_t cp, float penX,
                          float baselineY, float sizePx,
                          float r, float g, float b, float a) const {
  GlyphQuad q;
  float adv = layout(cp, penX, baselineY, sizePx, q);
  if (!q.draw) return adv;
  auto vert = [&](float x, float y, float u, float v) {
    out.push_back(x); out.push_back(y); out.push_back(u); out.push_back(v);
    out.push_back(r); out.push_back(g); out.push_back(b); out.push_back(a);
  };
  vert(q.x0, q.y0, q.u0, q.v0); vert(q.x1, q.y0, q.u1, q.v0); vert(q.x1, q.y1, q.u1, q.v1);
  vert(q.x0, q.y0, q.u0, q.v0); vert(q.x1, q.y1, q.u1, q.v1); vert(q.x0, q.y1, q.u0, q.v1);
  return adv;
}
