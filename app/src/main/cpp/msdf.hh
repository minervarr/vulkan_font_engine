#pragma once
#include <android/asset_manager.h>
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
  bool  hasGlyph = false;
  float planeL = 0, planeT = 0, planeR = 0, planeB = 0;  // em units (yOrigin top)
  float atlasL = 0, atlasT = 0, atlasR = 0, atlasB = 0;  // atlas pixels
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

  bool load(AAssetManager* mgr, const char* metricsPath, const char* atlasPath);
  bool valid() const { return !atlas_.empty() && !glyphs_.empty(); }

  uint32_t atlasW() const { return atlasW_; }
  uint32_t atlasH() const { return atlasH_; }
  const std::vector<uint8_t>& atlas() const { return atlas_; }
  float distanceRange() const { return distanceRange_; }

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

 private:
  std::unordered_map<uint32_t, MsdfGlyph> glyphs_;
  std::vector<uint8_t> atlas_;
  uint32_t atlasW_ = 0, atlasH_ = 0;
  float distanceRange_ = 4.0f, sizePxEm_ = 40.0f;
  float lineHeight_ = 1.2f, ascender_ = 0.0f, descender_ = 0.0f;
};
