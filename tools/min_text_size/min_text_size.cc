// min_text_size — computes the minimum device-pixel size a font can be drawn
// at before its thinnest stroke can no longer be reliably rasterized.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
//
// This is NOT a rendering-quality comparison (see the sibling msdf_readability
// tool for that, and its README-in-comments on why that metric turned out too
// noisy to trust). This tool asks a narrower, fully objective question
// instead: what is the thinnest stroke this font ever draws (as a fraction of
// its em square), and therefore at what pixel size does that stroke first
// drop below one device pixel wide — the point below which it cannot be
// reliably rasterized *at all*, independent of any rendering technique,
// hinting, or human judgment.
//
// Method: for every glyph in the app's baked charset, build its outline
// (msdfgen::Shape, same loadGlyph/normalize/orientContours sequence used by
// core/msdf.cc and atlas_gen.cc), rasterize it at a high fixed resolution via
// msdfgen::rasterize() (an exact analytic-scanline fill — not FreeType, not
// hinted, not resolution-dependent in the way a screenshot would be), and
// scan every row and column of the resulting coverage bitmap for the minimum
// contiguous run of "filled" pixels. The global minimum across the whole
// charset, converted back to an em-fraction, is thinnestStrokeEm.
//
//   minReadableSizePx = pixelCriterion / thinnestStrokeEm
//
// pixelCriterion defaults to 0.5 device pixel. (A literal 1.0px full-pixel
// requirement is the right bound for non-antialiased rasterization, but this
// engine renders MSDF text antialiased — a sub-pixel-wide stroke shows up as
// a fainter line rather than vanishing outright, so 0.5px calibrates the
// same objective method to how this renderer actually degrades.) thinnestStrokeEm
// itself is also taken as the 10th percentile of the font's stroke widths,
// not the absolute thinnest occurring detail — see computeForFont()'s
// comment for why (a lone decorative flourish otherwise dominates the
// number). Both are still fully geometric/deterministic, just calibrated.
//
// Usage:
//   min_text_size <font.otf> [pixelCriterion=0.5] [rasterEmPx=512]
//   min_text_size --emit-header <out.h> <pixelCriterion> <rasterEmPx> <font1.otf> [font2.otf ...]
//     Computes minReadableSizePx for every listed font (e.g. all four baked
//     styles) and writes a header defining kMinReadableTextSizePx as the max
//     (strictest) across them — the single floor the app should enforce,
//     since whichever style ends up smallest on screen is the binding one.
//   min_text_size --dump-image <out.bmp> <rasterEmPx> <font.otf> [text]
//     Rasterizes `text` (default a representative sample incl. serif-heavy
//     glyphs) at rasterEmPx-per-em resolution via the exact same
//     msdfgen::rasterize() pipeline the analysis uses, laid out left-to-right
//     in one BMP — the true vector outline shape, full detail, no MSDF/AA
//     ambiguity, so you can see exactly what geometry the numbers come from.

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

// Same charset core/msdf.cc's generate() bakes — the set this tool must
// cover, since anything not in it is never actually drawn by the app.
std::vector<uint32_t> charset() {
  std::vector<uint32_t> chars;
  for (uint32_t c = 0x20; c <= 0x7E; c++) chars.push_back(c);
  for (uint32_t c = 0xA1; c <= 0xFF; c++) chars.push_back(c);
  uint32_t extras[] = {0x152, 0x153, 0x178, 0x20AC, 0x2212, 0x221A, 0x03C0, 0x03B8};
  for (uint32_t c : extras) chars.push_back(c);
  return chars;
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
  uint32_t dataOffset = 54, dibSize = 40, planesAndBits = 0x00180001;
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

// Appends the length of every contiguous run of `filled` pixels (coverage >
// 0.5) in `bmp` to `out` — scanning rows (measuring vertical stems) and
// columns (measuring horizontal stems/crossbars). One sample per run.
//
// We pool these across the WHOLE charset and later take a low PERCENTILE
// rather than the absolute minimum: a plain global minimum is dominated by
// rare single-pixel corner/tip artifacts (a serif's tapered tip, a curve
// nearly tangent to horizontal/vertical) that occur on only one or two
// scanlines and aren't a real "stroke" — whereas a genuine thin stem/hairline
// produces the SAME narrow width across many consecutive scanlines, so it
// shows up as many samples at that width and survives a percentile cut that
// a one-off artifact doesn't.
void collectRunLengths(const msdfgen::Bitmap<float, 1>& bmp, std::vector<int>& out) {
  int w = bmp.width(), h = bmp.height();
  auto record = [&](int runLen) { if (runLen > 0) out.push_back(runLen); };
  for (int y = 0; y < h; y++) {
    int run = 0;
    for (int x = 0; x < w; x++) {
      bool filled = bmp(x, y)[0] > 0.5f;
      if (filled) { run++; }
      else { record(run); run = 0; }
    }
    record(run);
  }
  for (int x = 0; x < w; x++) {
    int run = 0;
    for (int y = 0; y < h; y++) {
      bool filled = bmp(x, y)[0] > 0.5f;
      if (filled) { run++; }
      else { record(run); run = 0; }
    }
    record(run);
  }
}

// Returns false on load failure. On success, fills thinnestStrokeEm and
// minReadableSizePx for this one font.
bool computeForFont(const std::string& fontPath, double pixelCriterion, int rasterEmPx,
                    double* thinnestStrokeEm, double* minReadableSizePx, int* glyphsWithInk) {
  std::vector<uint8_t> fontData = readFile(fontPath.c_str());
  if (fontData.empty()) {
    std::fprintf(stderr, "Could not read font: %s\n", fontPath.c_str());
    return false;
  }

  msdfgen::FreetypeHandle* ft = msdfgen::initializeFreetype();
  if (!ft) { std::fprintf(stderr, "initializeFreetype failed\n"); return false; }
  msdfgen::FontHandle* fontHandle = msdfgen::loadFontData(ft, fontData.data(), fontData.size());
  if (!fontHandle) {
    std::fprintf(stderr, "loadFontData failed: %s\n", fontPath.c_str());
    msdfgen::deinitializeFreetype(ft);
    return false;
  }
  msdfgen::FontMetrics metrics;
  msdfgen::getFontMetrics(metrics, fontHandle);

  double scale = rasterEmPx / metrics.emSize;
  std::vector<int> runLengths;
  *glyphsWithInk = 0;

  for (uint32_t cp : charset()) {
    msdfgen::Shape shape;
    double advance = 0;
    if (!msdfgen::loadGlyph(shape, fontHandle, cp, &advance)) continue;
    shape.normalize();
    shape.orientContours();
    msdfgen::Shape::Bounds bounds = shape.getBounds();
    if (!(bounds.l < bounds.r && bounds.b < bounds.t)) continue;  // whitespace glyph

    int margin = 4;  // a few raster px of border so edge AA doesn't clip
    int w = (int)std::ceil((bounds.r - bounds.l) * scale) + 2 * margin;
    int h = (int)std::ceil((bounds.t - bounds.b) * scale) + 2 * margin;
    if (w <= 0 || h <= 0) continue;

    msdfgen::Bitmap<float, 1> bmp(w, h);
    msdfgen::Vector2 translate(-bounds.l + margin / scale, -bounds.b + margin / scale);
    msdfgen::rasterize(bmp, shape, msdfgen::Projection(msdfgen::Vector2(scale), translate), msdfgen::FILL_NONZERO);

    size_t before = runLengths.size();
    collectRunLengths(bmp, runLengths);
    if (runLengths.size() > before) (*glyphsWithInk)++;
  }

  msdfgen::destroyFont(fontHandle);
  msdfgen::deinitializeFreetype(ft);

  if (runLengths.empty()) {
    std::fprintf(stderr, "No ink found in any charset glyph: %s\n", fontPath.c_str());
    return false;
  }

  // 10th percentile across every scanline run in the whole charset. Started
  // as the 1st percentile, but visual inspection (min_text_size --dump-image)
  // showed that was dominated by a single decorative flourish (the thin
  // trailing tail on '@') rather than any stroke ordinary alphabetic text
  // actually recurs with — 10th percentile reflects the strokes that show up
  // repeatedly across normal letters instead of one rare outlier detail.
  std::sort(runLengths.begin(), runLengths.end());
  size_t idx = std::min(runLengths.size() - 1, runLengths.size() * 10 / 100);
  int globalMinRunPx = runLengths[idx];

  *thinnestStrokeEm = (double)globalMinRunPx / rasterEmPx;
  *minReadableSizePx = pixelCriterion / *thinnestStrokeEm;
  return true;
}

int runSingleFont(const std::string& fontPath, double pixelCriterion, int rasterEmPx) {
  double thinnestStrokeEm = 0, minReadableSizePx = 0;
  int glyphsWithInk = 0;
  if (!computeForFont(fontPath, pixelCriterion, rasterEmPx, &thinnestStrokeEm, &minReadableSizePx, &glyphsWithInk))
    return 1;

  std::printf("Font: %s\n", fontPath.c_str());
  std::printf("Glyphs with ink: %d / raster resolution: %dpx/em\n", glyphsWithInk, rasterEmPx);
  std::printf("Thinnest stroke run at %dpx/em -> thinnestStrokeEm = %.6f\n", rasterEmPx, thinnestStrokeEm);
  std::printf("Minimum readable size (%.1fpx criterion): %.2fpx\n", pixelCriterion, minReadableSizePx);
  return 0;
}

int runEmitHeader(const std::string& outHeaderPath, double pixelCriterion, int rasterEmPx,
                   const std::vector<std::string>& fontPaths) {
  double worstMinReadable = -1;
  std::string worstFont;
  std::vector<std::string> lines;
  for (const std::string& fontPath : fontPaths) {
    double thinnestStrokeEm = 0, minReadableSizePx = 0;
    int glyphsWithInk = 0;
    if (!computeForFont(fontPath, pixelCriterion, rasterEmPx, &thinnestStrokeEm, &minReadableSizePx, &glyphsWithInk))
      return 1;
    char line[512];
    std::snprintf(line, sizeof(line), "//   %-40s thinnestStrokeEm=%.6f  minReadableSizePx=%.2f",
                  fontPath.c_str(), thinnestStrokeEm, minReadableSizePx);
    lines.push_back(line);
    if (minReadableSizePx > worstMinReadable) { worstMinReadable = minReadableSizePx; worstFont = fontPath; }
  }

  FILE* out = std::fopen(outHeaderPath.c_str(), "w");
  if (!out) { std::fprintf(stderr, "Could not open %s for writing\n", outHeaderPath.c_str()); return 1; }
  std::fprintf(out,
      "// Generated by min_text_size --emit-header. Do not edit by hand.\n"
      "// Per-font results (pixelCriterion=%.1f, rasterEmPx=%d):\n", pixelCriterion, rasterEmPx);
  for (const std::string& line : lines) std::fprintf(out, "%s\n", line.c_str());
  std::fprintf(out,
      "// Binding constraint: %s\n"
      "#pragma once\n"
      "constexpr float kMinReadableTextSizePx = %.4ff;\n",
      worstFont.c_str(), worstMinReadable);
  std::fclose(out);

  std::printf("Wrote %s: kMinReadableTextSizePx = %.4f (from %s)\n",
              outHeaderPath.c_str(), worstMinReadable, worstFont.c_str());
  return 0;
}

int runDumpImage(const std::string& outPath, int rasterEmPx, const std::string& fontPath,
                  const std::string& text) {
  std::vector<uint8_t> fontData = readFile(fontPath.c_str());
  if (fontData.empty()) { std::fprintf(stderr, "Could not read font: %s\n", fontPath.c_str()); return 1; }

  msdfgen::FreetypeHandle* ft = msdfgen::initializeFreetype();
  if (!ft) { std::fprintf(stderr, "initializeFreetype failed\n"); return 1; }
  msdfgen::FontHandle* fontHandle = msdfgen::loadFontData(ft, fontData.data(), fontData.size());
  if (!fontHandle) { std::fprintf(stderr, "loadFontData failed\n"); return 1; }
  msdfgen::FontMetrics metrics;
  msdfgen::getFontMetrics(metrics, fontHandle);
  double scale = rasterEmPx / metrics.emSize;

  struct Glyph { std::vector<uint8_t> gray; int w = 0, h = 0; };
  std::vector<Glyph> glyphs;
  int totalW = 0, maxH = 0;
  int margin = rasterEmPx / 32;  // a little breathing room so serifs aren't clipped

  for (size_t i = 0; i < text.size(); ) {
    uint32_t cp = (uint8_t)text[i++];  // ASCII-only sample string is enough here
    if (cp == ' ') {
      Glyph g; g.w = rasterEmPx / 2; g.h = 1; g.gray.assign((size_t)g.w, 0);
      totalW += g.w + margin; glyphs.push_back(std::move(g));
      continue;
    }
    msdfgen::Shape shape;
    double advance = 0;
    if (!msdfgen::loadGlyph(shape, fontHandle, cp, &advance)) continue;
    shape.normalize();
    shape.orientContours();
    msdfgen::Shape::Bounds bounds = shape.getBounds();
    if (!(bounds.l < bounds.r && bounds.b < bounds.t)) continue;

    int w = (int)std::ceil((bounds.r - bounds.l) * scale) + 2 * margin;
    int h = (int)std::ceil((bounds.t - bounds.b) * scale) + 2 * margin;
    if (w <= 0 || h <= 0) continue;

    msdfgen::Bitmap<float, 1> bmp(w, h);
    msdfgen::Vector2 translate(-bounds.l + margin / scale, -bounds.b + margin / scale);
    msdfgen::rasterize(bmp, shape, msdfgen::Projection(msdfgen::Vector2(scale), translate), msdfgen::FILL_NONZERO);

    // msdfgen rasterizes Y-up; flip to Y-down (row 0 = top) like a normal image.
    Glyph g; g.w = w; g.h = h; g.gray.resize((size_t)w * h);
    for (int y = 0; y < h; y++)
      for (int x = 0; x < w; x++)
        g.gray[(size_t)y * w + x] = (uint8_t)std::lround(std::clamp(bmp(x, h - 1 - y)[0], 0.0f, 1.0f) * 255.0f);
    maxH = std::max(maxH, h);
    totalW += w + margin;
    glyphs.push_back(std::move(g));
  }

  msdfgen::destroyFont(fontHandle);
  msdfgen::deinitializeFreetype(ft);

  if (glyphs.empty()) { std::fprintf(stderr, "Nothing to render\n"); return 1; }

  std::vector<uint8_t> canvas((size_t)totalW * maxH, 0);
  int penX = 0;
  for (const Glyph& g : glyphs) {
    int yOff = maxH - g.h;  // bottom-align (baselines roughly line up)
    for (int y = 0; y < g.h; y++)
      for (int x = 0; x < g.w; x++)
        canvas[(size_t)(y + yOff) * totalW + penX + x] = g.gray[(size_t)y * g.w + x];
    penX += g.w + margin;
  }
  writeBMP(outPath.c_str(), canvas, totalW, maxH);
  std::printf("Wrote %s (%dx%d, %dpx/em, %zu glyphs)\n", outPath.c_str(), totalW, maxH, rasterEmPx, glyphs.size());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::string(argv[1]) == "--dump-image") {
    if (argc < 5) {
      std::printf("Usage: %s --dump-image <out.bmp> <rasterEmPx> <font.otf> [text]\n", argv[0]);
      return 1;
    }
    std::string outPath = argv[2];
    int rasterEmPx = std::atoi(argv[3]);
    std::string fontPath = argv[4];
    std::string text = argc > 5 ? argv[5] : "AaGg8&@lni Matrix";
    return runDumpImage(outPath, rasterEmPx, fontPath, text);
  }

  if (argc > 1 && std::string(argv[1]) == "--emit-header") {
    if (argc < 6) {
      std::printf("Usage: %s --emit-header <out.h> <pixelCriterion> <rasterEmPx> <font1.otf> [font2.otf ...]\n", argv[0]);
      return 1;
    }
    std::string outHeaderPath = argv[2];
    double pixelCriterion = std::atof(argv[3]);
    int rasterEmPx = std::atoi(argv[4]);
    std::vector<std::string> fontPaths;
    for (int i = 5; i < argc; i++) fontPaths.push_back(argv[i]);
    return runEmitHeader(outHeaderPath, pixelCriterion, rasterEmPx, fontPaths);
  }

  std::string fontPath = argc > 1 ? argv[1] : "";
  double pixelCriterion = argc > 2 ? std::atof(argv[2]) : 0.5;
  int rasterEmPx = argc > 3 ? std::atoi(argv[3]) : 512;
  if (fontPath.empty()) {
    std::printf("Usage: %s <font.otf> [pixelCriterion=1.0] [rasterEmPx=512]\n", argv[0]);
    std::printf("   or: %s --emit-header <out.h> <pixelCriterion> <rasterEmPx> <font1.otf> [font2.otf ...]\n", argv[0]);
    return 1;
  }
  return runSingleFont(fontPath, pixelCriterion, rasterEmPx);
}
