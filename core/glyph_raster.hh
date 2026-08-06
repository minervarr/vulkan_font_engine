#pragma once
#include <cstdint>
#include <vector>

// ── Per-size glyph rasterization ────────────────────────────────────────────
//
// FreeType's own rasterizer, at the exact pixel size the glyph will be drawn
// at, producing 8-bit coverage. This is the "exact" half of the raster text
// path; RasterFont packs what comes out of here into an atlas.
//
// UNHINTED, deliberately. Hinting snaps stems to the pixel grid by MOVING the
// outline, which trades the designer's shapes for crisper verticals. That is a
// distortion, not fidelity, and this path exists to reproduce the face.
//
// FreeType only — no msdfgen, no Vulkan — which is what lets glyph_raster_test
// link it directly. Keep it that way, the same way gpos_kern.cc is kept clean.

namespace vfe {

// One rasterized glyph. `cov` is `w * h` bytes, row-major, 255 = fully inked.
// Empty for whitespace: `w == h == 0` with a real `advance`.
//
// bearingX/bearingY are FreeType's, in whole pixels: the offset from the pen
// origin to the bitmap's top-left corner, with Y measured UP from the
// baseline. Callers draw at (penX + bearingX, baselineY - bearingY).
struct RasterGlyph {
  int   w = 0, h = 0;
  int   bearingX = 0, bearingY = 0;
  float advance  = 0.0f;   // in pixels, at this size
  std::vector<uint8_t> cov;
};

// A face opened once and rasterized from many times. Wraps FT_Face without
// leaking FreeType's headers to callers.
class RasterFace {
 public:
  // The largest size render() will accept. Not a design limit — a backstop.
  // FreeType takes a pixel size as an unsigned int and does not sanity-check
  // it, so a corrupt size reaches FT_Render_Glyph and becomes a multi-gigabyte
  // allocation. Deliberately far above anything a UI asks for (~120 px at 8K)
  // and far below anything that could hurt.
  static constexpr uint32_t kMaxRenderPx = 4096;

  RasterFace() = default;
  ~RasterFace();
  RasterFace(const RasterFace&)            = delete;
  RasterFace& operator=(const RasterFace&) = delete;
  RasterFace(RasterFace&&) noexcept;
  RasterFace& operator=(RasterFace&&) noexcept;

  // The bytes must outlive the face — FreeType parses them lazily. RasterFace
  // therefore takes its own copy.
  bool openFromMemory(const uint8_t* data, size_t size);
  bool isOpen() const { return face_ != nullptr; }

  // True if this face has a real (non-.notdef) glyph for `cp`.
  bool hasCodepoint(uint32_t cp) const;

  // Rasterize `cp` at `sizePx` pixels per em. Returns false if the face has no
  // glyph for it. A whitespace glyph returns TRUE with an empty bitmap — the
  // caller still needs its advance.
  bool render(uint32_t cp, int sizePx, RasterGlyph& out) const;

  // Vertical metrics as EM fractions, straight from the face's design units.
  //
  // Use THIS, not verticalMetrics(), for anything that must scale exactly.
  // FreeType grid-fits `size->metrics` to whole pixels even with hinting
  // disabled — that rounding is per-size, so deriving an em ratio from one
  // size and multiplying gives a different answer than asking at the other
  // size. Design units have no such rounding. (`descender` is negative.)
  bool designMetrics(float& ascenderEm, float& descenderEm,
                     float& lineHeightEm) const;

  // Grid-fitted vertical metrics at `sizePx`, in whole-ish pixels — what
  // FreeType itself would use to lay out a line. Kept for callers that want
  // to match that exactly; see designMetrics() for why it is not the default.
  bool verticalMetrics(int sizePx, float& ascender, float& descender,
                       float& lineHeight) const;

  // Units per em — the divisor that turns GPOS kern values (font units) into
  // em fractions. 0 if not open.
  uint32_t unitsPerEm() const;

  // The raw font bytes, for handing to the GPOS kern parser.
  const std::vector<uint8_t>& bytes() const { return bytes_; }

 private:
  void close();

  void* library_ = nullptr;   // FT_Library
  void* face_    = nullptr;   // FT_Face
  std::vector<uint8_t> bytes_;
};

}  // namespace vfe
