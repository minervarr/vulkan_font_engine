#include "raster_font.hh"

#include "log.hh"
#include "utf8.hh"


#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

namespace {

// `kern` may be NULL, meaning "do not parse this face's kern table".
//
// Not an optimization bolted on afterwards — it is the difference between
// parsing something and parsing something NOTHING CAN READ. kernEmStyled()
// keys off the REQUESTED STYLE, not the resolved face: it looks in
// styles_[style], then styles_[Roman], and stops. It never reaches
// fallbacks_[] or overrides_. So a kern table parsed for a fallback face is
// written once and read never.
//
// The cost was not small. Seven of the ten faces this app registers are
// fallbacks or overrides, and the two Harano Aji Mincho cuts alone take 30 ms
// and 34 ms to yield 26601 and 29660 pairs that no code path can consult —
// paid on the main thread at startup, and paid TWICE, because the art window
// keeps its own RasterFont.
//
// See addFallback()/addOverride() for the invariant this rests on and what
// would invalidate it.
bool loadFace(AssetReader& reader, const char* path, vfe::RasterFace& face,
              vfe::KernTable* kern) {
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
  if (kern) vfe::parseGposKernPairs(buf.data(), buf.size(), *kern);
  return true;
}

}  // namespace

// ── Setup ───────────────────────────────────────────────────────────────────

bool RasterFont::open(AssetReader& reader, const char* fontPath) {
  auto f = std::make_unique<Face>();
  if (!loadFace(reader, fontPath, f->raster, &f->kern)) return false;

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
  if (!loadFace(reader, fontPath, f->raster, &f->kern)) return false;
  styles_[(int)style] = std::move(f);
  return true;
}

// ── Why these two do not parse kerning ──────────────────────────────────────
//
// kernEmStyled() consults styles_[style] and then styles_[Roman], and nothing
// else. A fallback or override face is chosen by faceFor() for its COVERAGE —
// which face has a glyph for this codepoint — while the kern table is picked
// by the style that was ASKED FOR. The two are decided independently, so a
// fallback's own table is unreachable by construction, not merely unused today.
//
// THE INVARIANT: if kerning is ever made to follow faceFor() instead of the
// requested style, these tables become live and both calls below must go back
// to passing &f->kern. The stale note that used to sit on faceFor() ("Also
// reports which kern table applies") suggests that was once the intent; it
// was never wired up, and the parse was left behind.
//
// Worth knowing before anyone tries: CJK and Hangul are not pair-kerned in any
// practical sense, so wiring it up would be a near-empty win bought at exactly
// the faces that cost the most to parse.
bool RasterFont::addFallback(AssetReader& reader, const char* fontPath,
                             FontStyle style) {
  auto f = std::make_unique<Face>();
  if (!loadFace(reader, fontPath, f->raster, nullptr)) return false;
  fallbacks_[(int)style].push_back(std::move(f));
  return true;
}

bool RasterFont::addOverride(AssetReader& reader, const char* fontPath) {
  auto f = std::make_unique<Face>();
  if (!loadFace(reader, fontPath, f->raster, nullptr)) return false;
  overrides_.push_back(std::move(f));
  return true;
}

bool RasterFont::openSharedWith(const RasterFont& src) {
  if (!src.styles_[(int)FontStyle::Roman] ||
      !src.styles_[(int)FontStyle::Roman]->raster.isOpen())
    return false;

  // One face over another's bytes. Returns null if the source face is closed,
  // so a partially-built source degrades to "that face is absent" rather than
  // to a half-open one.
  auto share = [](const Face& from) -> std::unique_ptr<Face> {
    auto f = std::make_unique<Face>();
    if (!f->raster.openSharedWith(from.raster)) return nullptr;
    // The kern table is COPIED, not re-parsed. Only styles_ carry one that
    // anything reads (see addFallback), and copying ~60k pairs is far cheaper
    // than parsing GPOS again — which is the cost this whole method exists to
    // avoid.
    f->kern = from.kern;
    return f;
  };

  for (int i = 0; i < kFontStyleCount; ++i) {
    if (!src.styles_[i]) continue;
    if (auto f = share(*src.styles_[i])) styles_[i] = std::move(f);
  }
  if (!styles_[(int)FontStyle::Roman]) return false;

  // Order is preserved in both chains. It is load-bearing in fallbacks_
  // (Chinese -> Japanese -> Korean, see addFallback) and in overrides_, which
  // are consulted ahead of every style.
  for (const auto& o : src.overrides_)
    if (auto f = share(*o)) overrides_.push_back(std::move(f));
  for (int i = 0; i < kFontStyleCount; ++i)
    for (const auto& fb : src.fallbacks_[i])
      if (auto f = share(*fb)) fallbacks_[i].push_back(std::move(f));

  // Vertical metrics are EM fractions read off the face's design units, so they
  // are a property of the bytes and copy exactly.
  ascenderEm_   = src.ascenderEm_;
  descenderEm_  = src.descenderEm_;
  lineHeightEm_ = src.lineHeightEm_;
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

// The serial one-cell bake, used by the miss path.
//
// It runs the SAME two steps the batched path does — extract the outline at
// this phase, then rasterize it — rather than FreeType's scan conversion. That
// is not tidiness: a cell reached through a miss is the same cell ensureGlyphs
// would have baked, so if the two used different rasterizers the identical
// glyph would look subtly different depending on which route happened to
// produce it, and which route that is depends on window size and scroll
// position. Nothing would look broken; text would just be inconsistent with
// itself in a way no test would catch.
bool RasterFont::bakeCell(const Face& f, FontStyle style, int sizePx,
                          uint32_t cp, uint32_t phase) {
  vfe::OutlineGlyph o;
  if (!f.raster.outline(cp, sizePx, o, phase)) return false;

  Cell c;
  c.advance  = o.advance;
  c.bearingX = o.bearingX;
  c.bearingY = o.bearingY;

  if (o.w > 0 && o.h > 0) {
    std::vector<uint8_t> cov;
    vfe::areaRasterize(o.edges, o.w, o.h, cov);

    vfe::ShelfPacker::Slot slot;
    if (!packInto(o.w, o.h, slot)) return false;
    c.hasGlyph = true;
    c.page   = slot.page;
    c.atlasX = slot.x;
    c.atlasY = slot.y;
    c.w = o.w;
    c.h = o.h;
    for (int y = 0; y < o.h; ++y) {
      std::memcpy(atlas_.data() + texelOffset(c.page, c.atlasX, c.atlasY + y),
                  cov.data() + (size_t)y * o.w, (size_t)o.w);
    }
  }
  // else: whitespace. Recorded with hasGlyph=false so layout advances the pen
  // without emitting a quad — the same contract MsdfGlyph::hasGlyph carries.

  cells_.emplace(cellKey(style, sizePx, cp, phase), c);
  return true;
}

bool RasterFont::valid() const { return hasStyle(FontStyle::Roman); }

void RasterFont::reserveSolidTexel() {
  if (solidOk_) return;
  vfe::ShelfPacker::Slot slot;
  if (!packInto((int)kSolidPx, (int)kSolidPx, slot)) return;
  if (gpuBake_) {
    // Nothing to write here — the host blits it through the baker, since the
    // pixels live on the GPU.
    solidX_ = slot.x; solidY_ = slot.y; solidPage_ = slot.page;
    solidOk_ = true; solidPending_ = true;
    return;
  }
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

bool RasterFont::takeSolidCell(uint32_t& page, uint32_t& x, uint32_t& y,
                               uint32_t& n) {
  if (!solidPending_) return false;
  page = solidPage_; x = solidX_; y = solidY_; n = kSolidPx;
  solidPending_ = false;
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
  gpuCells_.clear();
  gpuBaked_ = 0;
  solidPending_ = false;
  solidOk_ = false;
  solidX_ = solidY_ = 0;
}

uint32_t RasterFont::bakeThreadCount() {
  const unsigned hw = std::thread::hardware_concurrency();
  if (hw <= 1) return 1;
  return hw > 16 ? 16u : (uint32_t)hw;   // past this the commit side dominates
}

void RasterFont::rasterizeJobs(BakeJob* jobs, size_t count) {
  if (count == 0) return;

  const uint32_t workers =
      (uint32_t)std::min<size_t>(bakeThreadCount(), count);

  // One rasterizing face per worker, per face involved. Opened lazily and
  // kept: a bake is not a one-off, and reopening a 9 MB face per batch would
  // cost more than the batch.
  auto faceFor_ = [&](const Face* f, uint32_t w) -> const vfe::RasterFace* {
    Face* mf = const_cast<Face*>(f);
    if (mf->bakeFaces.size() < workers) {
      const size_t had = mf->bakeFaces.size();
      mf->bakeFaces.resize(workers);
      for (size_t i = had; i < workers; ++i)
        if (!mf->bakeFaces[i].openSharedWith(mf->raster))
          VFE_LOGE("Raster", "cannot open a second face for the bake worker");
    }
    const vfe::RasterFace& r = mf->bakeFaces[w];
    return r.isOpen() ? &r : &mf->raster;   // fall back to serial-safe path
  };
  for (size_t i = 0; i < count; ++i)
    if (jobs[i].face) (void)faceFor_(jobs[i].face, 0);   // allocate up front

  // One accumulator per worker, reused across every glyph it rasterizes.
  std::vector<std::vector<int32_t>> scratch(workers);

  auto run = [&](uint32_t w) {
    for (size_t i = w; i < count; i += workers) {
      BakeJob& j = jobs[i];
      if (!j.face) continue;
      const vfe::RasterFace* r = &j.face->bakeFaces[w];
      if (!r->isOpen()) r = &j.face->raster;
      if (gpuBake_) {
        j.ok = r->outline(j.cp, j.sizePx, j.outline, j.phase);
        continue;
      }
      // The CPU path runs the SAME two steps the GPU one does — extract the
      // outline, then rasterize it — instead of FreeType's scan conversion.
      // That is what puts the finer flattening (1/128 px against FreeType's
      // quarter pixel) on the default path, and it also makes the two paths
      // produce the same pixels, so MATRIX_GPU_GLYPHS is a performance switch
      // rather than a look switch.
      vfe::OutlineGlyph o;
      if (!r->outline(j.cp, j.sizePx, o, j.phase)) continue;
      j.glyph.w = o.w;
      j.glyph.h = o.h;
      j.glyph.bearingX = o.bearingX;
      j.glyph.bearingY = o.bearingY;
      j.glyph.advance  = o.advance;
      if (o.w > 0 && o.h > 0)
        vfe::areaRasterize(o.edges, o.w, o.h, j.glyph.cov, scratch[w]);
      j.ok = true;
    }
  };

  // One thread means no threads: the single-core path stays exactly what it
  // was, with no spawn cost and nothing to get wrong.
  if (workers == 1) { run(0); return; }

  std::vector<std::thread> pool;
  pool.reserve(workers - 1);
  for (uint32_t w = 1; w < workers; ++w) pool.emplace_back(run, w);
  run(0);
  for (auto& t : pool) t.join();
}

// In GPU mode the cell is placed and recorded, and its pixels are produced
// later by the compute passes. Everything about the PLACEMENT is identical to
// the CPU path — same packer, same order — so the atlas layout does not depend
// on which rasterizer made it.
bool RasterFont::commitGpuJob(BakeJob& job) {
  if (!job.ok) return false;

  Cell c;
  c.advance  = job.outline.advance;
  c.bearingX = job.outline.bearingX;
  c.bearingY = job.outline.bearingY;

  if (job.outline.w > 0 && job.outline.h > 0) {
    vfe::ShelfPacker::Slot slot;
    if (!packInto(job.outline.w, job.outline.h, slot)) return false;
    c.hasGlyph = true;
    c.page = slot.page; c.atlasX = slot.x; c.atlasY = slot.y;
    c.w = job.outline.w; c.h = job.outline.h;
    GpuCell g;
    g.glyph = std::move(job.outline);
    g.page = slot.page; g.x = slot.x; g.y = slot.y;
    gpuCells_.push_back(std::move(g));
  }
  cells_.emplace(cellKey(job.style, job.sizePx, job.cp, job.phase), c);
  return true;
}

bool RasterFont::commitJob(BakeJob& job) {
  if (!job.ok) return false;

  Cell c;
  c.advance  = job.glyph.advance;
  c.bearingX = job.glyph.bearingX;
  c.bearingY = job.glyph.bearingY;

  if (job.glyph.w > 0 && job.glyph.h > 0) {
    vfe::ShelfPacker::Slot slot;
    if (!packInto(job.glyph.w, job.glyph.h, slot)) return false;
    c.hasGlyph = true;
    c.page   = slot.page;
    c.atlasX = slot.x;
    c.atlasY = slot.y;
    c.w = job.glyph.w;
    c.h = job.glyph.h;
    for (int y = 0; y < job.glyph.h; ++y)
      std::memcpy(atlas_.data() + texelOffset(c.page, c.atlasX, c.atlasY + y),
                  job.glyph.cov.data() + (size_t)y * job.glyph.w,
                  (size_t)job.glyph.w);
  }
  // else: whitespace, recorded with hasGlyph=false so layout advances the pen
  // without emitting a quad.

  cells_.emplace(cellKey(job.style, job.sizePx, job.cp, job.phase), c);
  return true;
}

int RasterFont::ensureGlyphs(const std::vector<uint32_t>& cps,
                             const std::vector<int>& sizesPx) {
  for (int s : sizesPx)
    if (s > 0) sizes_.insert(quantize((float)s));
  if (sizes_.empty() || !styles_[(int)FontStyle::Roman]) return 0;

  reserveSolidTexel();   // first, so its position never moves
  const auto t0 = std::chrono::steady_clock::now();

  // ── 1. PLAN (serial) ────────────────────────────────────────────────────
  //
  // Which cells are missing, and which face serves each. Resolving the face
  // here rather than in the worker is what keeps FreeType single-threaded per
  // face: faceFor() queries the cmap through `raster`, the workers rasterize
  // through `bakeFaces`, and the two never touch the same FT_Face.
  //
  // Job ORDER is the atlas layout. An all-sizes pass over one codepoint keeps
  // that codepoint's variants adjacent, which stops a shelf from mixing a 19px
  // Latin cell with a 44px CJK one and wasting the height difference.
  std::vector<BakeJob> jobs;
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
        // Every subpixel phase this size is baked at — one cell each, and the
        // count depends on the size (see phaseCount(): the phases earn their
        // memory on small text and stop earning it on large).
        const uint32_t phases = phaseCount(sz);
        for (uint32_t ph = 0; ph < phases; ++ph) {
          if (cells_.count(cellKey((FontStyle)style, sz, cp, ph))) continue;
          BakeJob j;
          j.face = use;
          j.style = (FontStyle)style;
          j.sizePx = sz;
          j.cp = cp;
          j.phase = ph;
          jobs.push_back(std::move(j));
        }
      }
    }
  }
  if (jobs.empty()) return 0;

  // ── 2. RASTERIZE (parallel) and 3. COMMIT (serial) ──────────────────────
  //
  // Rasterizing is ~10-50us a glyph and every glyph is independent, so it is
  // the whole cost and it parallelises perfectly. Committing is a shelf
  // placement and a memcpy — microseconds — and must stay serial and IN JOB
  // ORDER, because the packer is one allocator and the resulting layout has to
  // be reproducible for a given input.
  //
  // This is deliberately not a background thread. Baking off the frame thread
  // would let the UI draw against a half-filled atlas, i.e. text visibly
  // appearing in pieces. Making the bake fast keeps every frame complete —
  // the same reason the whole cache is eager rather than lazy.
  int added = 0;
  for (size_t base = 0; base < jobs.size(); base += kBakeBatch) {
    const size_t n = std::min(kBakeBatch, jobs.size() - base);
    rasterizeJobs(jobs.data() + base, n);
    for (size_t i = 0; i < n; ++i) {
      if (gpuBake_ ? commitGpuJob(jobs[base + i]) : commitJob(jobs[base + i]))
        added++;
      jobs[base + i].glyph = vfe::RasterGlyph{};   // release the coverage bytes
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
    const uint32_t  phase = f.phase;

    const Face* use = faceFor(style, cp);
    if (!use || sz <= 0) { unservable_.insert(k); continue; }
    if (bakeCell(*use, style, sz, cp, phase)) {
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
// POSITION a quad is emitted at is quantized. That is what keeps textWidth()
// and the emitters in agreement — they add the same numbers in the same
// order, and the quantization never feeds back into the pen.
//
// HORIZONTALLY that quantization is now to a third of a pixel rather than a
// whole one: snapPen() splits the pen into the texel the cell is blitted at and
// the phase it was baked at, and the phase carries the remaining fraction as
// ink shifted inside the cell. See cellkey::kPhaseCount for why three.
//
// VERTICALLY it is still a whole pixel, and that asymmetry is deliberate: one
// baseline serves every glyph on the line, so rounding it moves them all
// together and there is no relative error to remove. Horizontal snapping was
// different precisely because each glyph rounded independently.

float RasterFont::layoutByKey(uint32_t key, float penX, float baselineY,
                              float sizePx, GlyphQuad& q) const {
  q.draw = false;
  if (key == 0) return penX;

  const FontStyle style = keyStyle(key);
  const uint32_t  cp    = keyCp(key);
  const int       px    = quantize(sizePx);

  float    snappedX = 0.0f;
  uint32_t phase    = 0;
  snapPen(penX, px, snappedX, phase);

  const Cell* c = find(style, px, cp, phase);
  if (!c) return penX;

  if (c->hasGlyph) {
    const float x = snappedX              + (float)c->bearingX;
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
