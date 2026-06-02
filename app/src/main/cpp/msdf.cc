#include "msdf.hh"

#include "utf8.hh"

#include <android/log.h>
#include <cstring>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "Msdf", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Msdf", __VA_ARGS__)

namespace {
std::vector<uint8_t> readAsset(AAssetManager* mgr, const char* path) {
  AAsset* a = AAssetManager_open(mgr, path, AASSET_MODE_BUFFER);
  if (!a) { LOGE("cannot open asset %s", path); return {}; }
  size_t n = (size_t)AAsset_getLength(a);
  std::vector<uint8_t> buf(n);
  AAsset_read(a, buf.data(), n);
  AAsset_close(a);
  return buf;
}

// Little-endian readers over a byte cursor.
uint32_t rdU32(const uint8_t*& p) { uint32_t v; std::memcpy(&v, p, 4); p += 4; return v; }
float    rdF32(const uint8_t*& p) { float v;    std::memcpy(&v, p, 4); p += 4; return v; }
}  // namespace

bool MsdfFont::load(AAssetManager* mgr, const char* metricsPath,
                    const char* atlasPath) {
  std::vector<uint8_t> meta = readAsset(mgr, metricsPath);
  if (meta.size() < 36) { LOGE("metrics too small"); return false; }

  const uint8_t* p = meta.data();
  uint32_t magic = rdU32(p);
  if (magic != 0x4644534D) { LOGE("bad magic %08x", magic); return false; }
  atlasW_        = rdU32(p);
  atlasH_        = rdU32(p);
  distanceRange_ = rdF32(p);
  sizePxEm_      = rdF32(p);
  lineHeight_    = rdF32(p);
  ascender_      = rdF32(p);
  descender_     = rdF32(p);
  uint32_t count = rdU32(p);

  for (uint32_t i = 0; i < count; i++) {
    uint32_t cp = rdU32(p);
    MsdfGlyph g;
    g.advance  = rdF32(p);
    g.hasGlyph = rdU32(p) != 0;
    g.planeL = rdF32(p); g.planeT = rdF32(p); g.planeR = rdF32(p); g.planeB = rdF32(p);
    g.atlasL = rdF32(p); g.atlasT = rdF32(p); g.atlasR = rdF32(p); g.atlasB = rdF32(p);
    glyphs_[cp] = g;
    if (cp < kFastCount) { fast_[cp] = g; fastHas_[cp] = true; }
  }

  atlas_ = readAsset(mgr, atlasPath);
  if (atlas_.size() != (size_t)atlasW_ * atlasH_ * 4) {
    LOGE("atlas size %zu != %ux%u*4", atlas_.size(), atlasW_, atlasH_);
    atlas_.clear();
    return false;
  }
  LOGI("MSDF loaded: %u glyphs, atlas %ux%u, range %.1f", count, atlasW_, atlasH_,
       distanceRange_);
  return true;
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
