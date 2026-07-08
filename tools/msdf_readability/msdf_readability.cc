// msdf_readability — measures the smallest on-screen size an MSDF atlas bake
// (a font + EM + distance range) still renders acceptably at.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
//
// Why this exists: MSDF text is scale-independent for MAGNIFICATION (that's
// its whole point), but a single bilinear GPU tap per pixel can't represent
// heavy MINIFICATION without shimmering — if you bake at a high EM (texels
// per em) but only ever display small text, every glyph gets minified far
// below its baked resolution and looks "low quality" no matter how good the
// atlas is. Rather than guess an EM and eyeball a screenshot, this tool:
//
//   1. Bakes each test glyph once via the SAME msdfgen call sequence used by
//      MsdfFont::generate() (core/msdf.cc) and the offline atlas_gen baker.
//   2. For a sweep of on-screen sizes, simulates exactly what the real
//      fragment shader (shaders_src/msdf_frag.slang) would produce — same
//      median(r,g,b) + screenPxRange smoothstep formula — by bilinearly
//      sampling the baked bitmap in plain C++ (no GPU needed: the screen-to-
//      atlas-texel ratio is known analytically here, more precisely than the
//      shader's fwidth() derivative approximation).
//   3. Renders the same glyph at the same exact pixel size via FreeType's own
//      native antialiased rasterizer (FT_RENDER_MODE_NORMAL) as ground truth.
//   4. Scores the simulated-MSDF output against that ground truth (mean
//      absolute coverage difference) and reports the smallest size where
//      every test glyph stays under a threshold, plus BMP image pairs (no
//      PNG encoder is vendored here — atlas_gen itself builds with
//      MSDFGEN_DISABLE_PNG=ON to avoid a libpng dependency, so this tool
//      writes plain uncompressed BMP instead, which Windows opens natively)
//      so a human can eyeball the threshold choice instead of trusting a
//      magic number blindly.
//
// CAVEAT: this is a CPU reimplementation of msdf_frag.slang's math. If that
// shader ever changes (blend mode, gamma, premultiplied alpha in the
// composite pass, etc), this tool can silently drift from what the GPU
// actually produces — spot-check one real screenshot against this tool's
// output after any shader change rather than trusting this forever.
//
// Usage:
//   msdf_readability <font.otf> [em=40] [threshold=0.08] [minSize=6] [maxSize=32] [outDir=out]

#include <ft2build.h>
#include FT_FREETYPE_H

#include <msdfgen.h>
#include <msdfgen-ext.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// Representative glyphs: sharp corners (A, G), thin stems (l, &), curves
// (a, g, 8), and diagonals (@) — the shapes most sensitive to minification.
const uint32_t kTestGlyphs[] = {'A', 'a', 'G', 'g', '8', '&', '@', 'l'};

std::vector<uint8_t> readFile(const char* path) {
  FILE* f = std::fopen(path, "rb");
  if (!f) return {};
  std::fseek(f, 0, SEEK_END);
  long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> buf(static_cast<size_t>(sz));
  std::fread(buf.data(), 1, buf.size(), f);
  std::fclose(f);
  return buf;
}

// Minimal uncompressed 24-bit BMP writer — no external deps (no libpng
// vendored in this tree; atlas_gen itself disables PNG for the same reason).
void writeBMP(const char* path, const std::vector<uint8_t>& gray, int w, int h) {
  int rowSize = ((w * 3 + 3) / 4) * 4;
  std::vector<uint8_t> pixels(static_cast<size_t>(rowSize) * h, 0);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      uint8_t v = gray[static_cast<size_t>(h - 1 - y) * w + x];  // BMP is bottom-up
      uint8_t* p = &pixels[static_cast<size_t>(y) * rowSize + x * 3];
      p[0] = p[1] = p[2] = v;
    }
  }
  uint8_t header[54] = {0};
  uint32_t fileSize = 54u + static_cast<uint32_t>(pixels.size());
  uint32_t dataOffset = 54, dibSize = 40, planesAndBits = 0x00180001;  // 1 plane, 24bpp
  uint32_t dataSize = static_cast<uint32_t>(pixels.size());
  int32_t ww = w, hh = h;
  header[0] = 'B'; header[1] = 'M';
  std::memcpy(&header[2], &fileSize, 4);
  std::memcpy(&header[10], &dataOffset, 4);
  std::memcpy(&header[14], &dibSize, 4);
  std::memcpy(&header[18], &ww, 4);
  std::memcpy(&header[22], &hh, 4);
  std::memcpy(&header[26], &planesAndBits, 4);
  std::memcpy(&header[34], &dataSize, 4);
  FILE* f = std::fopen(path, "wb");
  if (!f) return;
  std::fwrite(header, 1, 54, f);
  std::fwrite(pixels.data(), 1, pixels.size(), f);
  std::fclose(f);
}

// Side-by-side comparison image: FreeType reference | simulated MSDF.
void writeComparisonBMP(const char* path, const std::vector<uint8_t>& a, int aw, int ah,
                         const std::vector<uint8_t>& b, int bw, int bh) {
  int gap = 4;
  int h = std::max(ah, bh);
  int w = aw + gap + bw;
  std::vector<uint8_t> canvas(static_cast<size_t>(w) * h, 40);  // dark gray gap/background
  for (int y = 0; y < ah; y++)
    for (int x = 0; x < aw; x++) canvas[static_cast<size_t>(y) * w + x] = a[static_cast<size_t>(y) * aw + x];
  for (int y = 0; y < bh; y++)
    for (int x = 0; x < bw; x++) canvas[static_cast<size_t>(y) * w + aw + gap + x] = b[static_cast<size_t>(y) * bw + x];
  writeBMP(path, canvas, w, h);
}

float bilinearSample(const std::vector<uint8_t>& rgb, int w, int h, int channel, float u, float v) {
  float fx = std::clamp(u, 0.0f, 1.0f) * (w - 1);
  float fy = std::clamp(v, 0.0f, 1.0f) * (h - 1);
  int x0 = std::clamp((int)fx, 0, w - 1), x1 = std::clamp(x0 + 1, 0, w - 1);
  int y0 = std::clamp((int)fy, 0, h - 1), y1 = std::clamp(y0 + 1, 0, h - 1);
  float tx = fx - x0, ty = fy - y0;
  auto at = [&](int x, int y) { return rgb[(static_cast<size_t>(y) * w + x) * 3 + channel] / 255.0f; };
  float top = at(x0, y0) * (1 - tx) + at(x1, y0) * tx;
  float bot = at(x0, y1) * (1 - tx) + at(x1, y1) * tx;
  return top * (1 - ty) + bot * ty;
}

float median3(float r, float g, float b) { return std::max(std::min(r, g), std::min(std::max(r, g), b)); }

struct BakedGlyph {
  std::vector<uint8_t> rgb;  // 3-channel MSDF, baked once at construction EM
  int w = 0, h = 0;
  bool valid = false;
};

BakedGlyph bakeGlyphMsdf(msdfgen::FontHandle* fontHandle, double emUnits, double sizePxEm,
                          double distanceRange, uint32_t cp) {
  BakedGlyph out;
  msdfgen::Shape shape;
  double advance = 0;
  if (!msdfgen::loadGlyph(shape, fontHandle, cp, &advance)) return out;
  shape.normalize();
  // See core/msdf.cc's generate(): CFF/PostScript OTFs wind opposite to what
  // msdfgen's inside-positive convention assumes without this.
  shape.orientContours();
  msdfgen::edgeColoringSimple(shape, 3.0);
  msdfgen::Shape::Bounds bounds = shape.getBounds();
  if (!(bounds.l < bounds.r && bounds.b < bounds.t)) return out;  // whitespace/no ink

  double scale = sizePxEm / emUnits;
  int w = (int)std::ceil((bounds.r - bounds.l) * scale + 2.0 * distanceRange);
  int h = (int)std::ceil((bounds.t - bounds.b) * scale + 2.0 * distanceRange);
  if (w <= 0 || h <= 0) return out;

  msdfgen::Bitmap<float, 3> msdf(w, h);
  msdfgen::Vector2 translate(-bounds.l + distanceRange / scale, -bounds.b + distanceRange / scale);
  msdfgen::generateMSDF(msdf, shape, distanceRange / scale, msdfgen::Vector2(scale), translate);

  out.rgb.resize(static_cast<size_t>(w) * h * 3);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      auto px = msdf(x, h - 1 - y);  // msdfgen is Y-up; we keep Y-down like the GPU atlas
      size_t idx = (static_cast<size_t>(y) * w + x) * 3;
      out.rgb[idx]     = msdfgen::pixelFloatToByte(px[0]);
      out.rgb[idx + 1] = msdfgen::pixelFloatToByte(px[1]);
      out.rgb[idx + 2] = msdfgen::pixelFloatToByte(px[2]);
    }
  }
  out.w = w; out.h = h; out.valid = true;
  return out;
}

// Simulates the real fragment shader (msdf_frag.slang) sampling this glyph's
// baked bitmap as if displayed at `renderSizePx` on screen. Returns an 8-bit
// coverage (alpha) image sized for that display size.
std::vector<uint8_t> simulateAtSize(const BakedGlyph& baked, double sizePxEm,
                                     double distanceRange, double renderSizePx,
                                     int* outW, int* outH) {
  double k = renderSizePx / sizePxEm;  // display scale relative to the bake size
  int outW_ = std::max(1, (int)std::round(baked.w * k));
  int outH_ = std::max(1, (int)std::round(baked.h * k));

  // dot(unitRange, screenTexSize): unitRange = distanceRange/bakedSize (per
  // axis, in UV units, dimensionless). screenTexSize is screen PIXELS spanned
  // by the full u:0->1 (or v:0->1) span — i.e. the output size in pixels
  // (outW_/outH_), NOT its reciprocal (that's atlas-texels-per-screen-pixel,
  // a different quantity — this was inverted in an earlier version of this
  // tool, which floored screenPxRange to 1.0 for every glyph regardless of
  // size and produced visibly wrong, over-bold output). This is the exact
  // quantity msdf_frag.slang's fwidth() approximates on the GPU, computed
  // here analytically instead: unitRange.x * outW_ = (distanceRange/baked.w)
  // * (baked.w*k) = distanceRange*k, and the same for the other axis, so the
  // dot product collapses to simply 2*distanceRange*k.
  double screenPxRange = std::max(distanceRange * k, 1.0);

  std::vector<uint8_t> cov(static_cast<size_t>(outW_) * outH_);
  for (int y = 0; y < outH_; y++) {
    for (int x = 0; x < outW_; x++) {
      float u = (x + 0.5f) / outW_, v = (y + 0.5f) / outH_;
      float r = bilinearSample(baked.rgb, baked.w, baked.h, 0, u, v);
      float g = bilinearSample(baked.rgb, baked.w, baked.h, 1, u, v);
      float b = bilinearSample(baked.rgb, baked.w, baked.h, 2, u, v);
      float sd = median3(r, g, b);
      float screenPxDist = (float)screenPxRange * (sd - 0.5f);
      float a = std::clamp(screenPxDist + 0.5f, 0.0f, 1.0f);
      cov[static_cast<size_t>(y) * outW_ + x] = (uint8_t)std::lround(a * 255.0f);
    }
  }
  *outW = outW_; *outH = outH_;
  return cov;
}

std::vector<uint8_t> renderFreetypeReference(FT_Face face, uint32_t cp, int sizePx, int* outW, int* outH) {
  FT_Set_Pixel_Sizes(face, 0, sizePx);
  if (FT_Load_Char(face, cp, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0) {
    *outW = *outH = 0;
    return {};
  }
  FT_GlyphSlot slot = face->glyph;
  int w = slot->bitmap.width, h = slot->bitmap.rows;
  std::vector<uint8_t> out(static_cast<size_t>(w) * h, 0);
  for (int y = 0; y < h; y++)
    std::memcpy(&out[static_cast<size_t>(y) * w], slot->bitmap.buffer + y * slot->bitmap.pitch, w);
  *outW = w; *outH = h;
  return out;
}

std::vector<uint8_t> resampleBilinear(const std::vector<uint8_t>& src, int sw, int sh, int dw, int dh) {
  if (sw == 0 || sh == 0) return std::vector<uint8_t>(static_cast<size_t>(dw) * dh, 0);
  std::vector<uint8_t> out(static_cast<size_t>(dw) * dh);
  for (int y = 0; y < dh; y++) {
    for (int x = 0; x < dw; x++) {
      float fx = (x + 0.5f) / dw * sw - 0.5f, fy = (y + 0.5f) / dh * sh - 0.5f;
      int x0 = std::clamp((int)std::floor(fx), 0, sw - 1), x1 = std::clamp(x0 + 1, 0, sw - 1);
      int y0 = std::clamp((int)std::floor(fy), 0, sh - 1), y1 = std::clamp(y0 + 1, 0, sh - 1);
      float tx = std::clamp(fx - x0, 0.0f, 1.0f), ty = std::clamp(fy - y0, 0.0f, 1.0f);
      auto at = [&](int x, int y) { return src[static_cast<size_t>(y) * sw + x] / 255.0f; };
      float top = at(x0, y0) * (1 - tx) + at(x1, y0) * tx;
      float bot = at(x0, y1) * (1 - tx) + at(x1, y1) * tx;
      out[static_cast<size_t>(y) * dw + x] = (uint8_t)std::lround((top * (1 - ty) + bot * ty) * 255.0f);
    }
  }
  return out;
}

// Crops to the tight bounding box of "ink" (alpha above threshold), with a
// small uniform margin kept for the AA fringe. Necessary because FreeType's
// FT_RENDER_MODE_NORMAL bitmap is tightly ink-cropped, while the simulated
// MSDF crop still carries its baked distanceRange padding — comparing the two
// frames without first equalizing that padding would compare different
// effective scales/offsets, not just AA quality.
std::vector<uint8_t> cropToContent(const std::vector<uint8_t>& src, int w, int h,
                                    int* outW, int* outH, int threshold = 12, int margin = 1) {
  int minX = w, minY = h, maxX = -1, maxY = -1;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      if (src[static_cast<size_t>(y) * w + x] > threshold) {
        minX = std::min(minX, x); maxX = std::max(maxX, x);
        minY = std::min(minY, y); maxY = std::max(maxY, y);
      }
    }
  }
  if (maxX < 0) { *outW = w; *outH = h; return src; }  // blank (e.g. space) — return as-is
  minX = std::max(0, minX - margin); minY = std::max(0, minY - margin);
  maxX = std::min(w - 1, maxX + margin); maxY = std::min(h - 1, maxY + margin);
  int cw = maxX - minX + 1, ch = maxY - minY + 1;
  std::vector<uint8_t> out(static_cast<size_t>(cw) * ch);
  for (int y = 0; y < ch; y++)
    std::memcpy(&out[static_cast<size_t>(y) * cw], &src[static_cast<size_t>(minY + y) * w + minX], cw);
  *outW = cw; *outH = ch;
  return out;
}

double meanAbsDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  if (a.size() != b.size() || a.empty()) return 1.0;
  double sum = 0;
  for (size_t i = 0; i < a.size(); i++) sum += std::abs((int)a[i] - (int)b[i]) / 255.0;
  return sum / a.size();
}

}  // namespace

int main(int argc, char** argv) {
  std::string fontPath = argc > 1 ? argv[1] : "";
  double em = argc > 2 ? std::atof(argv[2]) : 40.0;
  double threshold = argc > 3 ? std::atof(argv[3]) : 0.08;
  int minSize = argc > 4 ? std::atoi(argv[4]) : 6;
  int maxSize = argc > 5 ? std::atoi(argv[5]) : 32;
  std::string outDir = argc > 6 ? argv[6] : "out";

  if (fontPath.empty()) {
    std::printf("Usage: %s <font.otf> [em=40] [threshold=0.08] [minSize=6] [maxSize=32] [outDir=out]\n", argv[0]);
    return 1;
  }
  double distanceRange = em * 0.1;  // matches atlas_gen.cc's / the app's own EM/10 convention

  std::string mkdirCmd = "mkdir \"" + outDir + "\" 2>nul";
  std::system(mkdirCmd.c_str());

  std::vector<uint8_t> fontData = readFile(fontPath.c_str());
  if (fontData.empty()) {
    std::fprintf(stderr, "Could not read font: %s\n", fontPath.c_str());
    return 1;
  }

  // Ground-truth path: our own plain FreeType face (FT_Render_Glyph).
  FT_Library ftLib;
  if (FT_Init_FreeType(&ftLib) != 0) { std::fprintf(stderr, "FT_Init_FreeType failed\n"); return 1; }
  FT_Face face;
  if (FT_New_Memory_Face(ftLib, fontData.data(), (FT_Long)fontData.size(), 0, &face) != 0) {
    std::fprintf(stderr, "FT_New_Memory_Face failed\n");
    return 1;
  }

  // Bake path: msdfgen's own FreeType handle (separate, matches core/msdf.cc).
  msdfgen::FreetypeHandle* msdfFt = msdfgen::initializeFreetype();
  msdfgen::FontHandle* fontHandle = msdfgen::loadFontData(msdfFt, fontData.data(), fontData.size());
  if (!fontHandle) { std::fprintf(stderr, "msdfgen::loadFontData failed\n"); return 1; }
  msdfgen::FontMetrics metrics;
  msdfgen::getFontMetrics(metrics, fontHandle);

  std::printf("Font: %s  EM=%.1f  range=%.2f  emUnits=%.1f\n", fontPath.c_str(), em, distanceRange, metrics.emSize);
  std::printf("size_px,glyph,mae_score\n");

  std::string csvPath = outDir + "/scores.csv";
  FILE* csv = std::fopen(csvPath.c_str(), "w");
  if (csv) std::fprintf(csv, "size_px,glyph,mae_score\n");

  int reportedMinSize = -1;
  for (int size = minSize; size <= maxSize; size++) {
    double worst = 0.0;
    for (uint32_t cp : kTestGlyphs) {
      BakedGlyph baked = bakeGlyphMsdf(fontHandle, metrics.emSize, em, distanceRange, cp);
      if (!baked.valid) continue;

      int simW, simH;
      std::vector<uint8_t> sim = simulateAtSize(baked, em, distanceRange, size, &simW, &simH);

      int refW, refH;
      std::vector<uint8_t> ref = renderFreetypeReference(face, cp, size, &refW, &refH);
      if (refW == 0 || refH == 0) continue;

      // Equalize framing before comparing: the simulated crop still carries
      // its baked distanceRange padding (needed for the SDF falloff), while
      // FreeType's bitmap is tightly ink-cropped — without this, a plain
      // resize compares two different effective scales, not just AA quality.
      int simCW, simCH, refCW, refCH;
      std::vector<uint8_t> simCropped = cropToContent(sim, simW, simH, &simCW, &simCH);
      std::vector<uint8_t> refCropped = cropToContent(ref, refW, refH, &refCW, &refCH);

      std::vector<uint8_t> simResampled = resampleBilinear(simCropped, simCW, simCH, refCW, refCH);
      double mae = meanAbsDiff(refCropped, simResampled);
      worst = std::max(worst, mae);

      std::printf("%d,%c,%.4f\n", size, (char)cp, mae);
      if (csv) std::fprintf(csv, "%d,%c,%.4f\n", size, (char)cp, mae);

      // Codepoint in the filename (not the raw char) — 'A' vs 'a', 'G' vs 'g'
      // collide on Windows' case-insensitive filesystem otherwise.
      char bmpPath[512];
      std::snprintf(bmpPath, sizeof(bmpPath), "%s/size%02d_cp%u.bmp", outDir.c_str(), size, cp);
      writeComparisonBMP(bmpPath, refCropped, refCW, refCH, simResampled, refCW, refCH);
    }
    if (reportedMinSize < 0 && worst > 0.0 && worst <= threshold) reportedMinSize = size;
  }
  if (csv) std::fclose(csv);

  if (reportedMinSize > 0)
    std::printf("\nMinimum readable size at threshold %.2f: %dpx\n", threshold, reportedMinSize);
  else
    std::printf("\nNo size in [%d,%d] stayed under threshold %.2f for every test glyph "
                "— try raising EM or the threshold.\n", minSize, maxSize, threshold);
  std::printf("Per-size comparison BMPs (FreeType reference | simulated MSDF) written to %s/\n", outDir.c_str());

  FT_Done_Face(face);
  FT_Done_FreeType(ftLib);
  return 0;
}
