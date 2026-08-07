#include "glyph_raster.hh"

// For kPhaseCount alone. The phase denominator MUST be the one the cell key
// encodes — a face that shifted by thirds while the key indexed quarters would
// bake perfectly good glyphs at positions nothing ever asks for. cell_key.hh
// pulls in text_font.hh, which is <cstdint>/<string_view>/<vector> and nothing
// else, so this keeps glyph_raster_test's standalone link intact.
#include "cell_key.hh"
#include "log.hh"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include <cmath>

#include <cstring>
#include <utility>

namespace vfe {
namespace {

FT_Library lib(void* p)  { return reinterpret_cast<FT_Library>(p); }
FT_Face    face(void* p) { return reinterpret_cast<FT_Face>(p); }

// 26.6 fixed point -> pixels.
constexpr float k26_6 = 1.0f / 64.0f;

}  // namespace

RasterFace::~RasterFace() { close(); }

RasterFace::RasterFace(RasterFace&& o) noexcept
    : library_(o.library_), face_(o.face_), bytes_(std::move(o.bytes_)) {
  o.library_ = nullptr;
  o.face_    = nullptr;
}

RasterFace& RasterFace::operator=(RasterFace&& o) noexcept {
  if (this != &o) {
    close();
    library_   = o.library_;
    face_      = o.face_;
    bytes_     = std::move(o.bytes_);
    o.library_ = nullptr;
    o.face_    = nullptr;
  }
  return *this;
}

void RasterFace::close() {
  if (face_)    FT_Done_Face(face(face_));
  if (library_) FT_Done_FreeType(lib(library_));
  face_    = nullptr;
  library_ = nullptr;
}

bool RasterFace::openFromMemory(const uint8_t* data, size_t size) {
  close();
  if (!data || size == 0) return false;

  // Our own copy: FT_New_Memory_Face does NOT take ownership and parses
  // lazily, so a caller's temporary buffer would be read after it died.
  bytes_ = std::make_shared<const std::vector<uint8_t>>(data, data + size);
  return openBytes();
}

bool RasterFace::openSharedWith(const RasterFace& other) {
  if (!other.bytes_ || other.bytes_->empty()) return false;
  close();
  bytes_ = other.bytes_;   // same buffer, its own FT_Library and FT_Face
  return openBytes();
}

// Build an FT_Library + FT_Face over whatever bytes_ already points at.
bool RasterFace::openBytes() {
  FT_Library l = nullptr;
  if (FT_Init_FreeType(&l) != 0) { bytes_.reset(); return false; }

  FT_Face f = nullptr;
  if (FT_New_Memory_Face(l, bytes_->data(), (FT_Long)bytes_->size(), 0, &f) != 0) {
    FT_Done_FreeType(l);
    bytes_.reset();
    return false;
  }
  if (FT_Select_Charmap(f, FT_ENCODING_UNICODE) != 0) {
    // Not fatal on every face, but a face with no Unicode cmap cannot serve a
    // codepoint lookup, which is the only way this class is used.
    FT_Done_Face(f);
    FT_Done_FreeType(l);
    bytes_.reset();
    return false;
  }

  library_ = l;
  face_    = f;
  return true;
}

uint32_t RasterFace::unitsPerEm() const {
  return face_ ? (uint32_t)face(face_)->units_per_EM : 0u;
}

bool RasterFace::hasCodepoint(uint32_t cp) const {
  if (!face_) return false;
  return FT_Get_Char_Index(face(face_), (FT_ULong)cp) != 0;
}

bool RasterFace::render(uint32_t cp, int sizePx, RasterGlyph& out) const {
  out = RasterGlyph{};
  if (!face_ || sizePx <= 0) return false;

  // Defence in depth against a caller with a corrupt size. FT_Set_Pixel_Sizes
  // will happily take tens of millions and FT_Render_Glyph will then try to
  // allocate a bitmap measured in gigabytes — which is exactly what a bug in
  // the cache's key packing once made it do, once per missed styled glyph.
  // The cache clamps too (RasterFont::quantize); this is the layer that does
  // not depend on the cache being right.
  if ((uint32_t)sizePx > kMaxRenderPx) {
    VFE_LOGE("Raster", "render: refusing a %d px glyph (cap %u) for U+%04X",
             sizePx, kMaxRenderPx, cp);
    return false;
  }

  FT_Face f = face(face_);
  const FT_UInt gid = FT_Get_Char_Index(f, (FT_ULong)cp);
  if (gid == 0) return false;

  if (FT_Set_Pixel_Sizes(f, 0, (FT_UInt)sizePx) != 0) return false;

  // NO_HINTING: see the header. TARGET_NORMAL selects the 8-bit AA rasterizer
  // for the FT_Render_Glyph below.
  if (FT_Load_Glyph(f, gid, FT_LOAD_NO_HINTING | FT_LOAD_TARGET_NORMAL) != 0)
    return false;

  out.advance = (float)f->glyph->advance.x * k26_6;

  if (FT_Render_Glyph(f->glyph, FT_RENDER_MODE_NORMAL) != 0) return false;

  const FT_Bitmap& bm = f->glyph->bitmap;
  out.bearingX = f->glyph->bitmap_left;
  out.bearingY = f->glyph->bitmap_top;

  // Whitespace: a real glyph with an advance and no ink. Reported as success.
  if (bm.width == 0 || bm.rows == 0) return true;

  out.w = (int)bm.width;
  out.h = (int)bm.rows;
  out.cov.resize((size_t)out.w * (size_t)out.h);

  // FT_RENDER_MODE_NORMAL always yields 8bpp gray, but `pitch` is a signed
  // stride that can be negative for a bottom-up bitmap, so copy row by row
  // rather than assuming a packed buffer.
  for (int y = 0; y < out.h; ++y) {
    const uint8_t* src = bm.buffer + (ptrdiff_t)y * bm.pitch;
    std::memcpy(out.cov.data() + (size_t)y * (size_t)out.w, src, (size_t)out.w);
  }
  return true;
}

// ── Outline extraction ──────────────────────────────────────────────────────
//
// Everything render() does except FT_Render_Glyph. The cell it reports must be
// the SAME cell FreeType would have produced, so the geometry is taken from
// FT_Outline_Get_CBox grid-fitted exactly the way FreeType's own rasterizer
// does it: floor the minimum, ceil the maximum, in 26.6.

namespace {

// Flatten in cell-local pixels, y down. FreeType hands us 26.6 y-up
// coordinates with the origin at the pen.
struct OutlineCtx {
  std::vector<vfe::AreaEdge>* edges;
  float originX = 0.0f;   // 26.6 x of the cell's left edge
  float originY = 0.0f;   // 26.6 y of the cell's TOP edge
  float curX = 0.0f, curY = 0.0f;
  float startX = 0.0f, startY = 0.0f;
  bool  open = false;

  float toX(FT_Pos v) const { return ((float)v - originX) * (1.0f / 64.0f); }
  // y-up (font) -> y-down (cell)
  float toY(FT_Pos v) const { return (originY - (float)v) * (1.0f / 64.0f); }

  void lineTo(float x, float y) {
    edges->push_back(vfe::AreaEdge{curX, curY, x, y});
    curX = x; curY = y;
  }

  // FT_Outline_Decompose starts each contour with move_to and never emits the
  // closing edge back to its first point. An unclosed contour has no defined
  // interior — the row's running winding sum never returns to zero and the
  // fill bleeds to the right edge of the cell — so close them here.
  void closeContour() {
    if (!open) return;
    if (curX != startX || curY != startY) lineTo(startX, startY);
    open = false;
  }
};

// ── Flattening ──────────────────────────────────────────────────────────────
//
// Uniform subdivision, with the segment count derived from the curve's own
// SECOND DIFFERENCE rather than from its length. The maximum distance a
// Bézier strays from its chord is a fixed multiple of that second difference,
// and splitting into n uniform pieces divides the deviation by n squared — so
// the count below is the smallest n that holds the error under kFlatTol.
//
// A length-proportional heuristic (segments per pixel) was tried first and is
// strictly worse in both directions: it over-subdivides a long gentle curve
// into thousands of edges the GPU then has to process, and under-subdivides a
// short sharp one, which is where the error actually lives.
//
// kFlatTol is deliberately BELOW the 1/64 pixel the outline itself is
// quantized to, so flattening is not the limiting factor in the result —
// area_raster_test measures what is, and it is the rasterizer's own
// fixed-point rounding at +/-2/255.
//
// Note this makes the curves FINER than FreeType's, whose own splitting stops
// at a quarter pixel. That is a deliberate choice (see area_raster_test): the
// output is closer to the designer's outline than FreeType's is, at the cost
// of not being bit-identical to it.
constexpr float kFlatTol = 1.0f / 128.0f;

int stepsForDeviation(float d) {
  if (d <= 0.0f) return 1;
  int n = (int)std::ceil(std::sqrt(d / kFlatTol));
  if (n < 1) n = 1;
  if (n > 256) n = 256;
  return n;
}

float vlen(float x, float y) { return std::sqrt(x * x + y * y); }

int cbMove(const FT_Vector* to, void* user) {
  auto* c = (OutlineCtx*)user;
  c->closeContour();
  c->curX = c->startX = c->toX(to->x);
  c->curY = c->startY = c->toY(to->y);
  c->open = true;
  return 0;
}

int cbLine(const FT_Vector* to, void* user) {
  auto* c = (OutlineCtx*)user;
  c->lineTo(c->toX(to->x), c->toY(to->y));
  return 0;
}

int cbConic(const FT_Vector* ctrl, const FT_Vector* to, void* user) {
  auto* c = (OutlineCtx*)user;
  const float x0 = c->curX, y0 = c->curY;
  const float cx = c->toX(ctrl->x), cy = c->toY(ctrl->y);
  const float x1 = c->toX(to->x),   y1 = c->toY(to->y);
  // |P0 - 2*P1 + P2| / 8 is the quadratic's maximum deviation from its chord.
  const int n = stepsForDeviation(vlen(x0 - 2 * cx + x1, y0 - 2 * cy + y1) * 0.125f);
  for (int i = 1; i <= n; ++i) {
    const float t = (float)i / (float)n, u = 1.0f - t;
    c->lineTo(u * u * x0 + 2 * u * t * cx + t * t * x1,
              u * u * y0 + 2 * u * t * cy + t * t * y1);
  }
  return 0;
}

int cbCubic(const FT_Vector* c1, const FT_Vector* c2, const FT_Vector* to,
            void* user) {
  auto* c = (OutlineCtx*)user;
  const float x0 = c->curX, y0 = c->curY;
  const float ax = c->toX(c1->x), ay = c->toY(c1->y);
  const float bx = c->toX(c2->x), by = c->toY(c2->y);
  const float x1 = c->toX(to->x), y1 = c->toY(to->y);
  // (3/4) * max of the two second differences bounds the cubic's deviation.
  const float d1 = vlen(x0 - 2 * ax + bx, y0 - 2 * ay + by);
  const float d2 = vlen(ax - 2 * bx + x1, ay - 2 * by + y1);
  const int n = stepsForDeviation(0.75f * (d1 > d2 ? d1 : d2));
  for (int i = 1; i <= n; ++i) {
    const float t = (float)i / (float)n, u = 1.0f - t;
    c->lineTo(u*u*u*x0 + 3*u*u*t*ax + 3*u*t*t*bx + t*t*t*x1,
              u*u*u*y0 + 3*u*u*t*ay + 3*u*t*t*by + t*t*t*y1);
  }
  return 0;
}

}  // namespace

bool RasterFace::outline(uint32_t cp, int sizePx, OutlineGlyph& out,
                         uint32_t phase) const {
  out = OutlineGlyph{};
  if (!face_ || sizePx <= 0) return false;
  if ((uint32_t)sizePx > kMaxRenderPx) return false;
  if (phase >= vfe::cellkey::kPhaseCount) return false;

  FT_Face f = face(face_);
  const FT_UInt gid = FT_Get_Char_Index(f, (FT_ULong)cp);
  if (gid == 0) return false;
  if (FT_Set_Pixel_Sizes(f, 0, (FT_UInt)sizePx) != 0) return false;
  if (FT_Load_Glyph(f, gid, FT_LOAD_NO_HINTING | FT_LOAD_TARGET_NORMAL) != 0)
    return false;

  out.advance = (float)f->glyph->advance.x * k26_6;
  if (f->glyph->format != FT_GLYPH_FORMAT_OUTLINE) return false;

  FT_Outline& ol = f->glyph->outline;

  // ── The subpixel shift ───────────────────────────────────────────────────
  //
  // Applied to the OUTLINE, before the box is measured, rather than to the
  // edges afterwards. Doing it here is what makes everything below — the
  // grid-fitted box, bearingX, w, and the cell-local origin the edges are
  // expressed against — describe the shifted glyph consistently. Shifting the
  // edges after the box had been measured would move the ink inside a cell
  // that no longer contained it, and the right-hand column would be clipped.
  //
  // The glyph slot is FreeType's own scratch, reloaded by the next
  // FT_Load_Glyph, so translating it in place costs nothing and leaks nothing.
  //
  // 64/3 is not an integer in 26.6, so a third of a pixel rounds to 21/64 and
  // two thirds to 43/64 — under a hundredth of a pixel from the ideal, which is
  // an order of magnitude finer than the half-pixel error this exists to remove
  // and far below the rasterizer's own +/-2/255.
  if (phase != 0) {
    const FT_Pos dx =
        (FT_Pos)((phase * 64 + vfe::cellkey::kPhaseCount / 2) /
                 vfe::cellkey::kPhaseCount);
    FT_Outline_Translate(&ol, dx, 0);
  }

  // The cell, grid-fitted the way FreeType's rasterizer does: the bitmap
  // covers whole pixels from floor(min) to ceil(max). Matching this is what
  // makes bearingX/bearingY and w/h agree with render().
  FT_BBox cbox;
  FT_Outline_Get_CBox(&ol, &cbox);
  const FT_Pos xMin = (cbox.xMin) & ~63;
  const FT_Pos yMin = (cbox.yMin) & ~63;
  const FT_Pos xMax = (cbox.xMax + 63) & ~63;
  const FT_Pos yMax = (cbox.yMax + 63) & ~63;

  out.bearingX = (int)(xMin >> 6);
  out.bearingY = (int)(yMax >> 6);
  out.w = (int)((xMax - xMin) >> 6);
  out.h = (int)((yMax - yMin) >> 6);

  // Whitespace: a real advance and no ink, reported as success — same contract
  // as render().
  if (out.w <= 0 || out.h <= 0) {
    out.w = out.h = 0;
    return true;
  }

  OutlineCtx ctx;
  ctx.edges   = &out.edges;
  ctx.originX = (float)xMin;
  ctx.originY = (float)yMax;

  FT_Outline_Funcs funcs;
  funcs.move_to  = cbMove;
  funcs.line_to  = cbLine;
  funcs.conic_to = cbConic;
  funcs.cubic_to = cbCubic;
  funcs.shift    = 0;
  funcs.delta    = 0;

  if (FT_Outline_Decompose(&ol, &funcs, &ctx) != 0) return false;
  ctx.closeContour();   // the last contour gets no trailing move_to
  return true;
}

bool RasterFace::designMetrics(float& ascenderEm, float& descenderEm,
                               float& lineHeightEm) const {
  if (!face_) return false;
  FT_Face f = face(face_);
  const float upem = (float)f->units_per_EM;
  if (upem <= 0.0f) return false;

  ascenderEm   = (float)f->ascender  / upem;
  descenderEm  = (float)f->descender / upem;   // negative
  lineHeightEm = (float)f->height    / upem;
  return true;
}

bool RasterFace::verticalMetrics(int sizePx, float& ascender, float& descender,
                                 float& lineHeight) const {
  if (!face_ || sizePx <= 0) return false;
  FT_Face f = face(face_);
  if (FT_Set_Pixel_Sizes(f, 0, (FT_UInt)sizePx) != 0) return false;

  ascender   = (float)f->size->metrics.ascender  * k26_6;
  descender  = (float)f->size->metrics.descender * k26_6;   // negative
  lineHeight = (float)f->size->metrics.height    * k26_6;
  return true;
}

}  // namespace vfe
