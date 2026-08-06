#include "glyph_raster.hh"

#include "log.hh"

#include <ft2build.h>
#include FT_FREETYPE_H

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
  bytes_.assign(data, data + size);

  FT_Library l = nullptr;
  if (FT_Init_FreeType(&l) != 0) { bytes_.clear(); return false; }

  FT_Face f = nullptr;
  if (FT_New_Memory_Face(l, bytes_.data(), (FT_Long)bytes_.size(), 0, &f) != 0) {
    FT_Done_FreeType(l);
    bytes_.clear();
    return false;
  }
  if (FT_Select_Charmap(f, FT_ENCODING_UNICODE) != 0) {
    // Not fatal on every face, but a face with no Unicode cmap cannot serve a
    // codepoint lookup, which is the only way this class is used.
    FT_Done_Face(f);
    FT_Done_FreeType(l);
    bytes_.clear();
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
