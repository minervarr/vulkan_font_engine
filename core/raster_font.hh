#pragma once
#include "asset_reader.hh"
#include "cell_key.hh"
#include "glyph_raster.hh"
#include "gpos_kern.hh"
#include "shelf_packer.hh"
#include "text_font.hh"

#include <cmath>
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
// 1. **Cells are keyed (style, sizePx, phase, codepoint), with sizePx an
//    INTEGER.** Quantizing the size at the key is what stops two sizes a
//    hundredth of a pixel apart from each baking their own copy of the
//    library. The PHASE is the opposite trade, made deliberately: the pen's
//    horizontal fraction is not thrown away but baked into a few pre-shifted
//    copies of the cell, so a glyph lands within a sixth of a pixel of where
//    the pen actually is instead of within a half. See
//    vfe::cellkey::kPhaseCount for why three, and phaseCount() for why only at
//    small sizes.
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
  // ── Pages ────────────────────────────────────────────────────────────────
  //
  // The atlas is N fixed-size pages, uploaded as the layers of one 2D array
  // image. It used to be a single 4096-square sheet, and that was a hard
  // ceiling reached in ordinary use: a raster cell's AREA grows with the
  // square of the size, so the same library that fits in 8 MB at 1080p wants
  // four times that at 4K and sixteen at 8K. Measured on the real library, a
  // 4K capture filled the sheet at 9571 cells and everything after it — most
  // of the UI's text — simply never got a cell.
  //
  // Pages remove the ceiling instead of raising it, and there is no policy cap
  // on how many there may be: an atlas that refuses to hold what is on screen
  // is not a budget, it is a bug. The renderer checks the count against the
  // device's maxImageArrayLayers (guaranteed >= 256, i.e. a gigabyte of pages
  // before it binds) and says so if a device ever objects.
  //
  // 4096 x 1024 R8 = 4 MB a page. Width is the sampler-friendly maximum every
  // implementation guarantees; the height is a granularity choice. Shelf waste
  // is about maxCellHeight/pageHeight — ~12% for a 120 px cell at 8K — and a
  // shorter page would waste proportionally more while a taller one would make
  // the art window, which needs a few dozen cells, pay for a bigger minimum.
  static constexpr uint32_t kPageW = 4096;
  static constexpr uint32_t kPageH = 1024;

  // Transparent margin reserved to the right of and below every cell. One
  // pixel is enough to stop the linear sampler reaching a neighbour when a
  // quad lands a hair off its texel grid.
  static constexpr uint32_t kPad = 1;

  static_assert(vfe::cellkey::kMaxCellPx + kPad <= kPageH,
                "a cell at the size cap must be placeable in an empty page");
  static_assert((size_t)kPageW * kPageH % 4 == 0,
                "VkBufferImageCopy::bufferOffset must be 4-byte aligned, and "
                "per-layer offsets are multiples of the page size");

  // ── Setup ────────────────────────────────────────────────────────────────

  // The default face. Must be called first: it supplies the vertical metrics
  // every size derives from.
  bool open(AssetReader& reader, const char* fontPath);

  // A named face (Bold/Italic/Math). Only the codepoints it genuinely covers
  // are registered for it; anything else falls through to the default face,
  // matching how Canvas::textStyled() already behaves on a keyForStyle() miss.
  bool addStyle(AssetReader& reader, const char* fontPath, FontStyle style);

  // Script-coverage faces (CJK/Hangul/Kana), consulted in registration order
  // for codepoints `style`'s own face lacks.
  //
  // Registering a chain PER STYLE is what makes bold text bold in every
  // script. Song/Mincho/Batang all ship a matched Bold cut, and without a
  // Bold chain a bold label resolves back to the Regular face: the text still
  // renders, at the wrong weight, which looks like working software right up
  // until it sits next to bold Latin.
  //
  // Order within a chain is Chinese -> Japanese -> Korean and it matters:
  // all three faces cover Han and Kana, and only the Korean one has Hangul.
  bool addFallback(AssetReader& reader, const char* fontPath,
                   FontStyle style = FontStyle::Roman);

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

  // Register exactly what `src` has registered, over the SAME font bytes.
  //
  // For a second window. Opening the faces from disk again costs a full re-read
  // and re-parse of every face — ~39 MB and the best part of a tenth of a
  // second for this app's ten — to produce byte-identical data that is already
  // in memory. RasterFace::openSharedWith() gives each face here its own
  // FT_Library/FT_Face (which is what FreeType's per-face thread-unsafety
  // requires) over a shared_ptr to the bytes (which are immutable, and are the
  // only large part).
  //
  // What is NOT shared, and cannot be: the atlas, the cells, the packer, the
  // size set. Two windows have two Renderers and therefore two VkDevices, and
  // an atlas image belongs to one of them. This shares the INPUT to baking, not
  // the result — each window still bakes what it draws, which for an art window
  // is a few dozen cells.
  //
  // `src` must outlive nothing in particular: the bytes are held by shared_ptr,
  // so this stays valid even if `src` is destroyed first.
  bool openSharedWith(const RasterFont& src);

  bool hasStyle(FontStyle style) const;

  // ── Baking ───────────────────────────────────────────────────────────────

  // Bake every registered face's coverage of `cps`, at every size in
  // `sizesPx` plus every size requested by a previous call. Already-present
  // cells are skipped, so calling this again after a rescan costs only what is
  // genuinely new. Returns the number of cells added.
  //
  // The caller must re-run Renderer::initMsdf() afterwards if this returns
  // nonzero — the atlas it grew is still only on the CPU.
  [[nodiscard]] int ensureGlyphs(const std::vector<uint32_t>& cps,
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
  [[nodiscard]] int bakeMisses();

  // Throw the sheet away and start over, keeping the opened faces.
  //
  // This is how the cache stays BOUNDED. Cells are keyed by size, so every
  // window resize that changes the type roles adds a whole new set and nothing
  // ever removes the old one. An LRU would work too, and would also mean a
  // cell could vanish while a quad still referenced it; starting over cannot
  // do that, and re-baking is only tens of microseconds a glyph.
  //
  // Note it clears unservable_ as well. That set is not only "no face has this
  // codepoint" — bakeMisses() also lands a key there when the sheet had no
  // ROOM for it, and those are exactly the glyphs a reset exists to recover.
  void reset();

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

  // Validity is about the FACES, not about what has been baked yet.
  //
  // It used to be `!cells_.empty() && atlasH_ > 0`, and both windows gate
  // Canvas::useMsdf() on it — so a font with no cells was never given to a
  // Canvas, therefore never recorded a miss, therefore never baked anything,
  // therefore stayed invalid. The cache could only ever start because
  // something else happened to seed it eagerly first.
  bool valid() const override;

  // A fully opaque texel, for a caller that wants a solid quad out of the
  // text pipeline (Canvas::quadMsdfRect). False if none has been reserved.
  bool solidTexel(float& u, float& v) const override;
  const std::vector<uint8_t>& atlas() const override {
    static const std::vector<uint8_t> kNone;
    return gpuBake_ ? kNone : atlas_;
  }
  // The PAGE size, not the whole sheet's: every page is exactly this, which is
  // what lets them be the layers of one array image and what lets layoutByKey()
  // normalise against a constant instead of a moving high-water mark.
  uint32_t atlasW() const override { return kPageW; }
  uint32_t atlasH() const override { return kPageH; }
  uint32_t atlasPages() const override { return packer_.pageCount(); }
  void takeDirtyPages(std::vector<uint32_t>& out) const override;

  // ── GPU baking ───────────────────────────────────────────────────────────
  //
  // In GPU mode ensureGlyphs still PLANS and PACKS exactly as before — same
  // ShelfPacker, same cell keys, same deterministic layout — but instead of
  // scan-converting each glyph it keeps the flattened outline and where the
  // cell goes. The host hands those to GlyphBaker, which rasterizes them in
  // compute and writes them straight into the atlas image.
  //
  // The CPU-side atlas_ vector is then never written, which is why
  // atlasOnGpu() reports true and atlas() comes back empty: there is nothing
  // to upload, because the pixels are produced where they are consumed.
  struct GpuCell {
    vfe::OutlineGlyph glyph;
    uint32_t page = 0, x = 0, y = 0;
  };
  void useGpuBake(bool on) { gpuBake_ = on; }
  bool atlasOnGpu() const override { return gpuBake_; }
  bool hasGpuWork() const { return gpuBaked_ < gpuCells_.size() || solidPending_; }
  // Every cell ever placed in GPU mode, kept rather than handed away.
  //
  // Growing the atlas creates a NEW image, and a new image is empty — every
  // cell baked into the old one is gone. Keeping the outlines means that case
  // is just "bake all of them again" instead of a silent hole in the text.
  // gpuBakedCount() is how far the host has got; it resets that to 0 when the
  // image handle changes underneath it.
  const std::vector<GpuCell>& gpuCells() const { return gpuCells_; }
  size_t gpuBakedCount() const { return gpuBaked_; }
  void setGpuBakedCount(size_t n) { gpuBaked_ = n; }
  // The reserved opaque block, which has no outline to rasterize. True once,
  // after it has been placed.
  bool takeSolidCell(uint32_t& page, uint32_t& x, uint32_t& y, uint32_t& n);
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
    uint32_t page = 0;        // which atlas page (array layer)
    uint32_t atlasX = 0, atlasY = 0;
  };

  // A face and how it kerns. Coverage is asked of the face itself
  // (RasterFace::hasCodepoint) rather than cached: a CJK face has tens of
  // thousands of codepoints and only a few hundred are ever wanted.
  struct Face {
    vfe::RasterFace raster;   // cmap and metric queries — the owning thread
    vfe::KernTable  kern;

    // One rasterizing face per bake worker, opened over the SAME font bytes.
    // FreeType is not thread-safe per face — every render() calls
    // FT_Set_Pixel_Sizes, which writes face->size — so a parallel bake needs a
    // face each. Sharing the bytes means this costs a pair of handles per
    // worker, not another copy of a 9 MB CJK font. Built on first use.
    std::vector<vfe::RasterFace> bakeFaces;
  };

  // One glyph waiting to be baked: what to rasterize, which face does it, and
  // the result. Planning and committing are serial; only rasterize() is not.
  struct BakeJob {
    const Face* face = nullptr;
    FontStyle   style = FontStyle::Roman;
    int         sizePx = 0;
    uint32_t    cp = 0;

    // ALL of this glyph's phases at this size, in one job.
    //
    // One job per phase would mean one FT_Load_Glyph per phase — the same
    // charstring parsed three times to produce three outlines that differ only
    // by a sub-pixel shift. That parse is the bake's single largest cost (71%
    // of outline extraction, which is itself ~80% of the bake), so the phases
    // are batched here and served by one RasterFace::outlinePhases() call.
    //
    // Round-robin scheduling in rasterizeJobs() is also why this has to be one
    // JOB rather than three adjacent ones: adjacent jobs go to different
    // workers, so there would be nothing to reuse.
    uint32_t    nPhases = 1;
    vfe::RasterGlyph  glyph[vfe::cellkey::kPhaseCount];
    vfe::OutlineGlyph outline[vfe::cellkey::kPhaseCount];   // GPU mode instead
    bool        ok = false;
  };

  // How many threads the bake may use, and how many jobs are held in flight.
  //
  // The batch bound is about MEMORY, not scheduling: a rasterized 8K cell is
  // ~12 KB of coverage, so holding all 15,000 of them at once would be ~180 MB
  // of transient buffers. A batch at a time keeps that at a few MB while still
  // giving every core a full slice of work.
  static uint32_t bakeThreadCount();
  static constexpr size_t kBakeBatch = 2048;

  // Rasterize every job in [begin, end) in parallel. Pure with respect to the
  // cache: it touches only the jobs and the per-worker faces.
  void rasterizeJobs(BakeJob* jobs, size_t count);

  // Place and store one rasterized job. Serial by necessity — the shelf packer
  // is a single allocator, and commit ORDER is what keeps the atlas layout
  // deterministic for a given input.
  // Both return the number of CELLS committed — a job now carries up to
  // kPhaseCount of them.
  int commitJob(BakeJob& job);
  int commitGpuJob(BakeJob& job);

  // Both key codecs live in cell_key.hh, where the field positions are derived
  // from declared widths and the round-trips are proved with static_assert.
  // They were written out by hand here, and the cell key's style bits sat
  // INSIDE its size field — see that header for what it cost.
  static uint64_t cellKey(FontStyle s, int sizePx, uint32_t cp,
                          uint32_t phase = 0) {
    return vfe::cellkey::encodeCell(s, (uint32_t)sizePx, cp, phase);
  }
  static uint32_t glyphKey(FontStyle s, uint32_t cp) {
    return vfe::cellkey::encodeGlyph(s, cp);
  }
  static FontStyle keyStyle(uint32_t key) { return vfe::cellkey::glyphStyle(key); }
  static uint32_t keyCp(uint32_t key) { return vfe::cellkey::glyphCp(key); }

  // The one place a float size becomes a cell size. Everything that touches
  // the cache goes through it, so measurement and drawing cannot disagree
  // about which bake they mean.
  //
  // The upper clamp is defence in depth, not a design limit: it is what stands
  // between a corrupt size and FreeType being asked for a bitmap measured in
  // gigabytes. A clamped size stays SELF-CONSISTENT — measurement and drawing
  // both land on the same cell — so text drawn past the cap is the wrong size
  // but never misaligned. Nothing in this app comes close (the largest type
  // role is ~120 px at 8K).
  static int quantize(float sizePx) {
    int px = (int)(sizePx + 0.5f);
    if (px < 1) return 1;
    if (px > (int)vfe::cellkey::kMaxCellPx) return (int)vfe::cellkey::kMaxCellPx;
    return px;
  }

  const Cell* find(FontStyle s, int sizePx, uint32_t cp,
                   uint32_t phase = 0) const {
    const uint64_t k = cellKey(s, sizePx, cp, phase);
    auto it = cells_.find(k);
    if (it != cells_.end()) return &it->second;
    if (!unservable_.count(k)) misses_.insert(k);
    return nullptr;
  }

  // ── Where subpixel phases are worth their memory ─────────────────────────
  //
  // Every extra phase is a whole extra copy of a cell, and a cell's area grows
  // with the SQUARE of its size — so the atlas cost of phases is concentrated
  // exactly where the benefit is smallest. Half a pixel is ~3% of an 18 px
  // caption's glyph and ~0.4% of a 120 px title's: the small text is where
  // uneven spacing is visible, and the large text is where tripling the atlas
  // actually hurts.
  //
  // So phases are baked below this size and not above it. The number is a
  // measurement, not a guess — see the commit that introduced it.
  //
  // This is consulted by BOTH the bake and the layout, through this one
  // function. If they ever disagreed, layout would ask for a phase nothing
  // baked and record a miss for it every single frame, forever.
  static constexpr int kMaxPhasedPx = 48;
  static uint32_t phaseCount(int sizePx) {
    return sizePx <= kMaxPhasedPx ? vfe::cellkey::kPhaseCount : 1u;
  }

  // Snap a float pen to the nearest available sub-position, splitting the
  // result into the whole pixel the cell is blitted at and the phase it was
  // baked at.
  //
  // The two come out of ONE rounding, deliberately. Rounding the pixel and the
  // phase separately has a boundary case that is easy to get wrong and hard to
  // see: a pen at x.9 rounds to phase 3 of 3, which is not a phase — it is
  // phase 0 of the NEXT pixel, and a version that clamps it back to phase 2
  // leaves the glyph a third of a pixel short exactly when it sits just under a
  // boundary. Rounding to the nearest 1/n first and then dividing makes the
  // carry fall out of the arithmetic instead of needing to be spotted.
  static void snapPen(float penX, int sizePx, float& xOut, uint32_t& phaseOut) {
    const uint32_t n = phaseCount(sizePx);
    if (n <= 1) { xOut = std::round(penX); phaseOut = 0; return; }
    const long q = std::lround((double)penX * (double)n);   // nearest 1/n
    // Floor division: penX is negative for anything scrolled off the left, and
    // C's truncating division would round those toward zero and shift them.
    const long ln = (long)n;
    const long px = q >= 0 ? q / ln : -((-q + ln - 1) / ln);
    xOut     = (float)px;
    phaseOut = (uint32_t)(q - px * ln);   // always in [0, n)
  }

  // Which face serves `cp` for `style`: the style's own face if it covers it,
  // else the default face, else the fallbacks in order. nullptr if nothing has
  // it.
  //
  // COVERAGE ONLY — this does not decide kerning. kernEmStyled() keys off the
  // requested style and never looks at what faceFor() returned, which is why
  // fallback and override faces do not have their kern tables parsed at all
  // (see addFallback() in the .cc). This comment used to claim the opposite;
  // that claim was never true, and believing it cost ~70 ms of every startup.
  const Face* faceFor(FontStyle style, uint32_t cp) const;

  bool bakeCell(const Face& f, FontStyle style, int sizePx, uint32_t cp,
                uint32_t phase);
  bool packInto(int w, int h, vfe::ShelfPacker::Slot& out);

  // Byte index of (page, x, y) in the page-major backing store.
  size_t texelOffset(uint32_t page, uint32_t x, uint32_t y) const;

  // Reserve the opaque block solidTexel() reports. Costs one 2x2 cell, taken
  // before any glyph so its position is stable for the life of the sheet.
  void reserveSolidTexel();

  std::unique_ptr<Face> styles_[kFontStyleCount];
  std::vector<std::unique_ptr<Face>> overrides_;                  // before styles_
  std::vector<std::unique_ptr<Face>> fallbacks_[kFontStyleCount]; // after

  std::unordered_map<uint64_t, Cell> cells_;
  // The sizes ensureGlyphs() bakes its codepoint set at — the app's type
  // roles, and only those. Sizes learned from the miss path are deliberately
  // NOT added here; see the comment on bakeMisses().
  std::set<int> sizes_;

  // Cell keys asked for and absent (mutable: recorded from the const draw
  // path — see hasMisses()), and the ones no face can ever supply, so a
  // genuinely uncoverable codepoint costs one lookup rather than a bake
  // attempt every frame.
  mutable std::set<uint64_t> misses_;
  std::set<uint64_t>         unservable_;

  // R8 coverage, PAGE-MAJOR: page p occupies [p*kPageW*kPageH, (p+1)*...).
  // One contiguous block so the whole atlas uploads from a single staging
  // buffer, with one copy region per layer.
  std::vector<uint8_t> atlas_;

  // Where cells go. Holding the shelf bookkeeping in a type whose place() is
  // all-or-nothing is what stops one oversized glyph from wedging the packer
  // for the rest of the session — see shelf_packer.hh.
  vfe::ShelfPacker packer_{kPageW, kPageH, kPad, /*maxPages=*/0};   // 0 = unlimited

  // The reserved opaque block (see solidTexel()). 2x2 rather than 1x1 so its
  // centre sits at a texel corner and a linear fetch there averages four
  // fully-opaque texels instead of straddling the padding.
  // Pages written since the consumer last took the record. Mutable because
  // takeDirtyPages() is const on the TextFont seam — it reports and clears,
  // and only the uploader ever calls it.
  mutable std::set<uint32_t> dirtyPages_;

  bool gpuBake_ = false;
  bool solidPending_ = false;
  uint32_t solidPage_ = 0;
  std::vector<GpuCell> gpuCells_;
  size_t gpuBaked_ = 0;

  static constexpr uint32_t kSolidPx = 2;
  uint32_t solidX_ = 0, solidY_ = 0;
  bool     solidOk_ = false;

  float ascenderEm_ = 0.0f, descenderEm_ = 0.0f, lineHeightEm_ = 1.2f;
};
