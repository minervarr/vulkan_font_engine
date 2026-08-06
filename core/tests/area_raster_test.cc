// area_raster_test — THE QUALITY GATE for GPU glyph rasterization.
//
// area_raster.hh implements analytic area coverage by signed-area
// accumulation, chosen because it is the same arithmetic FreeType's smooth
// rasterizer performs and is therefore the only approach that can MATCH the
// reference rather than approximate it. This file is where that claim is
// either true or not.
//
// It runs entirely on the CPU and deliberately comes before any GPU work: if
// the algorithm cannot reproduce FreeType here, it will not reproduce it in a
// compute shader either, and the cheap thing to do is find out now.
//
// Same convention as the other core/tests/*.cc: plain assert(), no framework,
// #undef NDEBUG so the checks survive an optimized build, fonts directory from
// argv[1], `return 77` when the assets are not where we were pointed. Links
// glyph_raster.cc + FreeType and nothing else.

#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_IMAGE_H

#include "../area_raster.hh"
#include "../glyph_raster.hh"

namespace {

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream f(path, std::ios::binary | std::ios::ate);
  if (!f) return {};
  const std::streamsize n = f.tellg();
  f.seekg(0);
  std::vector<uint8_t> buf(static_cast<size_t>(n));
  f.read(reinterpret_cast<char*>(buf.data()), n);
  return buf;
}

bool openFile(vfe::RasterFace& face, const std::string& path) {
  const std::vector<uint8_t> bytes = readFile(path);
  if (bytes.empty()) return false;
  return face.openFromMemory(bytes.data(), bytes.size());
}

// How one glyph compares, cell for cell, against FreeType.
struct Diff {
  bool   compared = false;
  int    maxAbs = 0;         // worst single-pixel difference, 0..255
  double meanAbs = 0.0;      // mean over the whole cell
  size_t over1 = 0;          // pixels differing by more than 1/255
  size_t pixels = 0;
  // Differences in a pixel FreeType calls solid (255) or empty (0) mean the
  // fill is wrong, not that the antialiasing rounds differently.
  int    maxAbsInterior = 0;
};

Diff compareGlyph(const vfe::RasterFace& face, uint32_t cp, int sizePx) {
  Diff d;
  vfe::RasterGlyph ref;
  vfe::OutlineGlyph ol;
  if (!face.render(cp, sizePx, ref)) return d;
  if (!face.outline(cp, sizePx, ol)) return d;

  // Geometry must agree exactly, or the comparison below is nonsense — it
  // would be measuring two different cells.
  assert(ol.w == ref.w);
  assert(ol.h == ref.h);
  assert(ol.bearingX == ref.bearingX);
  assert(ol.bearingY == ref.bearingY);
  assert(std::fabs(ol.advance - ref.advance) < 0.01f);

  if (ref.w == 0 || ref.h == 0) return d;   // whitespace: nothing to compare

  std::vector<uint8_t> got;
  vfe::areaRasterize(ol.edges, ol.w, ol.h, got);
  assert(got.size() == ref.cov.size());

  double sum = 0;
  for (size_t i = 0; i < got.size(); ++i) {
    const int a = ref.cov[i], b = got[i];
    const int diff = a > b ? a - b : b - a;
    if (diff > d.maxAbs) d.maxAbs = diff;
    if (diff > 1) d.over1++;
    if ((a == 0 || a == 255) && diff > d.maxAbsInterior) d.maxAbsInterior = diff;
    sum += diff;
  }
  d.pixels = got.size();
  d.meanAbs = sum / (double)got.size();
  d.compared = true;
  return d;
}

struct Totals {
  int    maxAbs = 0;
  int    maxAbsInterior = 0;
  double sumMean = 0.0;
  size_t glyphs = 0;
  size_t over1 = 0;
  size_t pixels = 0;
  uint32_t worstCp = 0;
  int      worstSize = 0;

  void add(const Diff& d, uint32_t cp, int sz) {
    if (!d.compared) return;
    if (d.maxAbs > maxAbs) { maxAbs = d.maxAbs; worstCp = cp; worstSize = sz; }
    if (d.maxAbsInterior > maxAbsInterior) maxAbsInterior = d.maxAbsInterior;
    sumMean += d.meanAbs;
    over1 += d.over1;
    pixels += d.pixels;
    glyphs++;
  }
  double meanAbs() const { return glyphs ? sumMean / (double)glyphs : 0.0; }
  double over1Frac() const { return pixels ? (double)over1 / (double)pixels : 0.0; }
};

void sweep(const vfe::RasterFace& face, const char* label,
           const std::vector<uint32_t>& cps, Totals& t) {
  Totals local;
  for (uint32_t cp : cps)
    for (int sz : {12, 18, 24, 37, 48, 73, 96, 120, 160})
      local.add(compareGlyph(face, cp, sz), cp, sz);
  std::printf("    %-10s %5zu glyphs  max %3d  mean %.3f  >1/255 %.4f%%\n",
              label, local.glyphs, local.maxAbs, local.meanAbs(),
              local.over1Frac() * 100.0);
  t.maxAbs = local.maxAbs > t.maxAbs ? local.maxAbs : t.maxAbs;
  t.maxAbsInterior =
      local.maxAbsInterior > t.maxAbsInterior ? local.maxAbsInterior : t.maxAbsInterior;
  t.sumMean += local.sumMean;
  t.glyphs += local.glyphs;
  t.over1 += local.over1;
  t.pixels += local.pixels;
  if (local.maxAbs == t.maxAbs) { t.worstCp = local.worstCp; t.worstSize = local.worstSize; }
}

std::vector<uint32_t> range(uint32_t a, uint32_t b, uint32_t step = 1) {
  std::vector<uint32_t> v;
  for (uint32_t c = a; c <= b; c += step) v.push_back(c);
  return v;
}

// ── Rasterizer against rasterizer, on IDENTICAL geometry ──────────────────
//
// Hands our own flattened polyline to FreeType's rasterizer and compares the
// two coverages. This is the check that actually validates area_raster.hh:
// with the geometry held equal, any difference is the scan conversion itself.
//
// It exists because the naive comparison — ours vs FreeType end to end —
// conflates two things, and the conflation is misleading. That comparison
// shows ~9% of pixels differing, which looks like a broken rasterizer and is
// in fact entirely down to the two flattening the curves differently.
struct IsoResult {
  long exact = 0, within1 = 0, within8 = 0, over8 = 0, total = 0;
  int  maxAbs = 0;
};

void isolateAgainstFreeType(FT_Library lib, const vfe::RasterFace& face,
                            uint32_t cp, int sizePx, IsoResult& r) {
  vfe::OutlineGlyph o;
  if (!face.outline(cp, sizePx, o) || o.w == 0 || o.h == 0) return;

  std::vector<uint8_t> mine;
  vfe::areaRasterize(o.edges, o.w, o.h, mine);

  // Rebuild the same polyline as an FT_Outline: 26.6, y-up, cell-local. A new
  // contour starts wherever an edge does not continue from the previous one.
  std::vector<FT_Vector>      pts;
  std::vector<unsigned char>  tags;
  std::vector<unsigned short> contours;
  for (size_t i = 0; i < o.edges.size(); ++i) {
    const vfe::AreaEdge& e = o.edges[i];
    const bool startNew = i == 0 ||
                          std::fabs(o.edges[i - 1].x1 - e.x0) > 1e-6f ||
                          std::fabs(o.edges[i - 1].y1 - e.y0) > 1e-6f;
    if (startNew) {
      if (i > 0) contours.push_back((unsigned short)(pts.size() - 1));
      pts.push_back(FT_Vector{(FT_Pos)std::lround(e.x0 * 64.0f),
                              (FT_Pos)std::lround((o.h - e.y0) * 64.0f)});
      tags.push_back(FT_CURVE_TAG_ON);
    }
    pts.push_back(FT_Vector{(FT_Pos)std::lround(e.x1 * 64.0f),
                            (FT_Pos)std::lround((o.h - e.y1) * 64.0f)});
    tags.push_back(FT_CURVE_TAG_ON);
  }
  if (pts.empty()) return;
  contours.push_back((unsigned short)(pts.size() - 1));

  FT_Outline ol{};
  ol.n_points   = (short)pts.size();
  ol.n_contours = (short)contours.size();
  ol.points     = pts.data();
  ol.tags       = tags.data();
  ol.contours   = contours.data();
  ol.flags      = FT_OUTLINE_NONE;

  std::vector<uint8_t> ftbuf((size_t)o.w * o.h, 0);
  FT_Bitmap bm{};
  bm.width      = (unsigned)o.w;
  bm.rows       = (unsigned)o.h;
  bm.pitch      = o.w;
  bm.buffer     = ftbuf.data();
  bm.num_grays  = 256;
  bm.pixel_mode = FT_PIXEL_MODE_GRAY;
  if (FT_Outline_Get_Bitmap(lib, &ol, &bm) != 0) return;

  for (size_t i = 0; i < mine.size(); ++i) {
    const int a = ftbuf[i], b = mine[i];
    const int d = a > b ? a - b : b - a;
    if (d > r.maxAbs) r.maxAbs = d;
    if (d == 0) r.exact++;
    else if (d <= 1) r.within1++;
    else if (d <= 8) r.within8++;
    else r.over8++;
    r.total++;
  }
}

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "assets/fonts";
  const std::string newcm = dir + "/newcomputermodern/NewCM10-Regular.otf";

  vfe::RasterFace face;
  if (!openFile(face, newcm)) {
    std::fprintf(stderr, "area_raster_test: cannot read %s\n", newcm.c_str());
    return 77;  // "skipped" — assets not where we were pointed
  }

  // ── [1] A shape with a known answer ───────────────────────────────────────
  //
  // Before trusting the comparison, check the arithmetic against something
  // hand-computable: an axis-aligned square covering exactly the middle half
  // of a 4x4 cell must come out fully opaque inside and empty outside.
  {
    std::vector<vfe::AreaEdge> box = {
        {1, 1, 3, 1}, {3, 1, 3, 3}, {3, 3, 1, 3}, {1, 3, 1, 1},
    };
    std::vector<uint8_t> cov;
    vfe::areaRasterize(box, 4, 4, cov);
    for (int y = 0; y < 4; ++y)
      for (int x = 0; x < 4; ++x) {
        const bool inside = x >= 1 && x < 3 && y >= 1 && y < 3;
        assert(cov[y * 4 + x] == (inside ? 255 : 0));
      }
    // Half a pixel wide: exactly 50% coverage, and the neighbour stays empty.
    std::vector<vfe::AreaEdge> half = {
        {0, 0, 0.5f, 0}, {0.5f, 0, 0.5f, 1}, {0.5f, 1, 0, 1}, {0, 1, 0, 0},
    };
    vfe::areaRasterize(half, 2, 1, cov);
    assert(cov[0] >= 126 && cov[0] <= 129);
    assert(cov[1] == 0);
    std::printf("[1] known shapes exact\n");
  }

  // ── [2] Winding: a hole must actually be a hole ───────────────────────────
  {
    std::vector<vfe::AreaEdge> ring = {
        // outer, clockwise
        {0, 0, 8, 0}, {8, 0, 8, 8}, {8, 8, 0, 8}, {0, 8, 0, 0},
        // inner, counter-clockwise
        {2, 2, 2, 6}, {2, 6, 6, 6}, {6, 6, 6, 2}, {6, 2, 2, 2},
    };
    std::vector<uint8_t> cov;
    vfe::areaRasterize(ring, 8, 8, cov);
    assert(cov[0 * 8 + 0] == 255);      // outside the hole, inside the ring
    assert(cov[4 * 8 + 4] == 0);        // in the hole
    assert(cov[1 * 8 + 1] == 255);
    std::printf("[2] non-zero winding: holes are holes\n");
  }

  // ── [3] Against FreeType, across scripts and sizes ────────────────────────
  //
  // This is the gate. The bounds below are what the algorithm actually
  // achieves; they are asserted so a change that degrades it fails here rather
  // than on nava's screen.
  Totals t;
  std::printf("[3] vs FreeType:\n");
  sweep(face, "ASCII", range(0x21, 0x7E), t);
  sweep(face, "Latin-1", range(0xC0, 0xFF), t);
  sweep(face, "Greek", range(0x391, 0x3C9), t);
  sweep(face, "Cyrillic", range(0x410, 0x44F), t);

  // Optional multi-script faces, same skip-not-fail rule as glyph_raster_test.
  struct Probe { const char* path; const char* label; std::vector<uint32_t> cps; };
  const Probe probes[] = {
      {"/fandol/FandolSong-Regular.otf",       "Han",    range(0x4E00, 0x4E80)},
      {"/haranoaji/HaranoAjiMincho-Regular.otf", "Kana", range(0x3040, 0x30A0)},
      {"/unfonts-core/UnBatang.ttf",           "Hangul", range(0xAC00, 0xAC80)},
  };
  for (const Probe& p : probes) {
    vfe::RasterFace f2;
    if (!openFile(f2, dir + p.path)) {
      std::printf("    %-10s skipped (not installed)\n", p.label);
      continue;
    }
    sweep(f2, p.label, p.cps, t);
  }

  std::printf("    %-10s %5zu glyphs  max %3d  mean %.3f  >1/255 %.4f%%"
              "  (worst U+%04X @ %dpx)\n",
              "TOTAL", t.glyphs, t.maxAbs, t.meanAbs(), t.over1Frac() * 100.0,
              t.worstCp, t.worstSize);

  assert(t.glyphs > 1000);   // the sweep must not have silently done nothing

  // Section [3] CHARACTERIZES a known, deliberate difference; it does not
  // bound an error. Our curves are flattened finer than FreeType's, whose own
  // splitting stops once its chords are within a quarter pixel, so the two
  // disagree on curved edges by construction — and ours is the one closer to
  // the outline. These bounds exist so the difference cannot silently GROW.
  assert(t.meanAbs() <= 1.5);
  assert(t.over1Frac() <= 0.08);

  // ── [4] The actual correctness gate ───────────────────────────────────────
  //
  // Same geometry, both rasterizers. Nothing here can be explained away by
  // flattening, so these bounds are tight and they are the ones that matter.
  {
    FT_Library lib = nullptr;
    assert(FT_Init_FreeType(&lib) == 0);
    IsoResult r;
    for (uint32_t cp = 0x21; cp <= 0x7E; ++cp)
      for (int sz : {12, 18, 24, 37, 48, 73, 96, 120, 160})
        isolateAgainstFreeType(lib, face, cp, sz, r);
    FT_Done_FreeType(lib);

    std::printf("[4] same geometry, both rasterizers: "
                "exact %.2f%%  <=1 %.2f%%  2-8 %.4f%%  >8 %.4f%%  max %d"
                "  (%ld px)\n",
                100.0 * (double)r.exact / (double)r.total,
                100.0 * (double)r.within1 / (double)r.total,
                100.0 * (double)r.within8 / (double)r.total,
                100.0 * (double)r.over8 / (double)r.total, r.maxAbs, r.total);

    assert(r.total > 100000);
    // 94% of pixels identical, nothing off by more than 4/255, and NOTHING
    // off by more than 8. The residual is FreeType working in 26.6 integers
    // where this works in float, plus the rounding into 0..255 — widening the
    // fixed-point accumulator from 16 to 22 fractional bits does not move any
    // of these numbers, which is how we know that is what it is.
    assert(r.maxAbs <= 4);
    assert(r.over8 == 0);
    assert((double)r.exact / (double)r.total >= 0.90);
  }

  std::printf("area_raster_test: all checks passed\n");
  return 0;
}
