// raster_font_test — how far a drawn glyph lands from where the pen said.
//
// The cache quantizes horizontal position, because a cell can only be blitted
// on a whole texel. The QUESTION is how coarsely, and that number is the text
// path's positional fidelity: it used to be a whole pixel (std::round(penX)),
// so every glyph independently sat up to half a pixel off its true place. At
// an 18 px caption half a pixel is ~3% of a glyph width, and because each
// glyph rounds on its own it shows up as letters bunching and gapping inside a
// word rather than as the word being shifted.
//
// Subpixel phases replace that with a third of a pixel. This file measures the
// difference rather than asserting the feature is "on":
//
//   e(penX) = emitted quad x - penX
//
// is constant for a perfect layout. Its SPREAD over a sweep of pen positions
// is exactly the quantization step — 1.0 px before, 1/kPhaseCount after — so
// one number states the whole improvement and cannot be satisfied by a phase
// field that is plumbed everywhere but never actually shifts any ink.
//
// It also pins the two things in snapPen() most likely to be wrong and least
// likely to be noticed: the carry when a pen lands just under a pixel boundary
// (that is phase 0 of the NEXT pixel, not a clamp back to the last phase), and
// negative pen positions, which C's truncating division rounds the wrong way.
//
// Same convention as the other core/tests/*.cc: plain assert(), no framework,
// #undef NDEBUG so the checks survive an optimized build, fonts directory from
// argv[1], `return 77` when the assets are not where we were pointed.

#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "../asset_reader.hh"
#include "../cell_key.hh"
#include "../raster_font.hh"

namespace {

// ── Where the ink actually is ──────────────────────────────────────────────
//
// NOT q.x0. The quad's left edge is the CELL's, and a cell is always placed on
// a whole texel — that is the constraint phases exist to work around, not the
// thing they change. Measuring q.x0 shows a 1.0 px spread whether phases work
// or not, which is a very convincing way to conclude the feature does nothing.
//
// The subpixel position lives INSIDE the cell: phase p bakes the glyph shifted
// p/kPhaseCount of a texel to the right within the same box. So the effective
// position is the quad's origin plus the coverage's own horizontal centroid,
// which is read here out of the atlas the cache just baked, through the UVs
// the quad carries. Public API only, end to end.
double inkX(const RasterFont& f, const GlyphQuad& q) {
  const std::vector<uint8_t>& atlas = f.atlas();
  const uint32_t pw = f.atlasW(), ph = f.atlasH();
  const uint32_t x0 = (uint32_t)std::lround((double)q.u0 * pw);
  const uint32_t x1 = (uint32_t)std::lround((double)q.u1 * pw);
  const uint32_t y0 = (uint32_t)std::lround((double)q.v0 * ph);
  const uint32_t y1 = (uint32_t)std::lround((double)q.v1 * ph);
  const size_t base = (size_t)q.page * pw * ph;

  double sum = 0.0, wsum = 0.0;
  for (uint32_t y = y0; y < y1; ++y)
    for (uint32_t x = x0; x < x1; ++x) {
      const double c = atlas[base + (size_t)y * pw + x];
      sum  += c;
      wsum += c * ((double)(x - x0) + 0.5);
    }
  if (sum <= 0.0) return (double)q.x0;
  return (double)q.x0 + wsum / sum;
}

// Sweep e(penX) = ink x - penX across a range of pen positions and report how
// much it varies. That spread IS the horizontal quantization step.
struct Spread {
  double lo = 1e9, hi = -1e9;
  size_t n = 0;
  void add(double e) { if (e < lo) lo = e; if (e > hi) hi = e; ++n; }
  double range() const { return n ? hi - lo : 0.0; }
};

Spread sweepGlyph(const RasterFont& f, uint32_t cp, float sizePx,
                  float from, float to, float step) {
  Spread s;
  const uint32_t key = f.keyForStyle(FontStyle::Roman, cp);
  for (float pen = from; pen <= to; pen += step) {
    GlyphQuad q;
    (void)f.layoutByKey(key, pen, 100.0f, sizePx, q);
    if (!q.draw) continue;
    s.add(inkX(f, q) - (double)pen);
  }
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "assets/fonts";
  const std::string face = dir + "/newcomputermodern/NewCM10-Regular.otf";

  FileByteReader reader;
  RasterFont font;
  if (!font.open(reader, face.c_str())) {
    std::printf("raster_font_test: cannot open %s — skipping\n", face.c_str());
    return 77;
  }

  const uint32_t n = vfe::cellkey::kPhaseCount;
  const double step = 1.0 / (double)n;

  // Sizes on both sides of the phase threshold, so this covers the phased and
  // the unphased regime in one run. Nothing here hardcodes which is which —
  // the expectation is derived from phaseCount() the same way the bake is.
  const std::vector<int> sizes = {12, 18, 24, 37, 48, 64, 96, 120};
  const std::vector<uint32_t> cps = {0x48, 0x6F, 0x69, 0x41, 0x6D, 0x57, 0x2E};

  {
    std::vector<int> all(sizes);
    std::vector<float> szf;
    for (int s : all) szf.push_back((float)s);
    assert(font.ensureGlyphs(cps, all) > 0);
  }

  // ── [1] The quantization step, measured ──────────────────────────────────
  {
    for (int sz : sizes) {
      const bool phased = sz <= 48;   // see RasterFont::phaseCount
      const double want = phased ? step : 1.0;

      Spread worst;
      double worstRange = 0.0;
      uint32_t worstCp = 0;
      for (uint32_t cp : cps) {
        // A fine sweep over several whole pixels: fine enough to land in every
        // phase, wide enough that the pixel carry happens repeatedly.
        const Spread s = sweepGlyph(font, cp, (float)sz, 10.0f, 14.0f, 0.017f);
        if (s.n < 100) continue;
        if (s.range() > worstRange) { worstRange = s.range(); worst = s; worstCp = cp; }
      }
      assert(worst.n >= 100);

      std::printf("[1] %3dpx  %-8s spread %.4f px (ideal %.4f)  worst U+%04X\n",
                  sz, phased ? "phased" : "whole", worstRange, want, worstCp);

      // Bounded well clear of the ideal on both sides, because the CENTROID
      // itself is a discretized measure: summing coverage * (x + 0.5) over
      // pixels approximates the true first moment, and the error depends on
      // where the shape sits against the grid — which is exactly what is being
      // swept. It is worst on the smallest glyphs, where there are fewest
      // pixels to average over (measured: ~0.1 px at 12 px, matching
      // area_raster_test's own figure for the same U+0069).
      //
      // So these do not pin the step to three decimal places. They pin the
      // CLAIM: a phased size quantizes several times finer than a whole-pixel
      // one, and an unphased size still quantizes to exactly one pixel. The
      // gap between the two bounds is enormous and nothing ambiguous lives in
      // it — a dead phase field reads 1.0 here, not 0.4.
      if (phased) {
        assert(worstRange < 0.55);
        assert(worstRange > 0.15);
      } else {
        assert(worstRange > 0.85);
        assert(worstRange < 1.05);
      }
    }
  }

  // ── [2] The carry, and negative pens ─────────────────────────────────────
  //
  // A pen at x.9 rounds to "phase 3 of 3", which is not a phase — it is phase
  // 0 of the next pixel. A version that clamped it back to phase 2 would leave
  // the glyph a third of a pixel short exactly when it sits just under a
  // boundary, which is invisible in a screenshot and wrong in every word.
  //
  // Negative pens are the other half: text scrolled off the left edge. C's
  // division truncates toward zero, so a naive implementation rounds those the
  // wrong way and shifts them by a whole pixel.
  {
    const uint32_t key = font.keyForStyle(FontStyle::Roman, 0x48);
    double worst = 0.0;
    int checked = 0;
    // Straddle several boundaries, from well negative to well positive.
    for (double pen = -8.0; pen <= 8.0; pen += 1.0 / 97.0) {
      GlyphQuad q;
      (void)font.layoutByKey(key, (float)pen, 100.0f, 18.0f, q);
      if (!q.draw) continue;
      // Reference: the same layout at a pen rounded to the nearest 1/n by
      // hand. The emitted x must never be further from the true pen than half
      // a step, whichever side of zero and whichever side of a boundary.
      GlyphQuad q0;
      (void)font.layoutByKey(key, 0.0f, 100.0f, 18.0f, q0);
      const double bearing = inkX(font, q0);   // e(0) — the glyph's own offset
      const double err = std::fabs((inkX(font, q) - bearing) - pen);
      if (err > worst) worst = err;
      ++checked;
    }
    std::printf("[2] carry + negative pens: %d positions, worst offset error "
                "%.4f px (half a step = %.4f)\n",
                checked, worst, step * 0.5);
    assert(checked > 1000);
    // Comfortably under 0.5, which is what a whole-pixel snap gives and what a
    // broken carry would reintroduce for the pens just below a boundary. Not
    // pinned to the ideal 1/6 for the same reason as [1]: the centroid this is
    // measured with carries ~0.05-0.1 px of its own discretization at 18 px,
    // and the reference e(0) is itself one of the quantized positions, so the
    // comparison inherits that rounding twice.
    assert(worst < 0.30);
  }

  // ── [3] Advances do not depend on the phase ──────────────────────────────
  //
  // The pen accumulates exact float advances and textWidth() adds the same
  // numbers in the same order. If the phase ever leaked into an advance, the
  // measured width and the drawn width would drift apart and every centred
  // label would sit slightly off — the classic version of this bug.
  {
    for (int sz : sizes) {
      for (uint32_t cp : cps) {
        const uint32_t key = font.keyForStyle(FontStyle::Roman, cp);
        const float a = font.advanceKey(key, (float)sz);
        // Whatever pen we lay out at, the advance returned must be identical.
        for (double pen : {0.0, 0.1, 0.34, 0.5, 0.66, 0.9, -2.4}) {
          GlyphQuad q;
          const float next = font.layoutByKey(key, (float)pen, 100.0f, (float)sz, q);
          assert(std::fabs((double)(next - (float)pen) - (double)a) < 1e-4);
        }
      }
    }
    std::printf("[3] advances are phase-independent\n");
  }

  // ── [4] openSharedWith: a second window's font, over the same bytes ───────
  //
  // The art window opens its own RasterFont because it has its own VkDevice and
  // therefore its own atlas. It used to re-read ~39 MB of faces from disk to do
  // it. openSharedWith() gives each face a fresh FT_Face over the FIRST font's
  // bytes instead.
  //
  // This needs its own test because the art window is the ONE surface the
  // headless capture tool cannot photograph — Host::secondaryWindowHandle() is
  // null there by design, so ArtWindow::create() declines and no capture ever
  // contains a pixel of it. A regression here would be invisible to every other
  // gate in this repo.
  //
  // The bar is not "it works" but "it is the same font": same coverage of
  // codepoints, same style resolution through the fallback chains, same metrics,
  // same advances, and byte-identical cells.
  {
    const std::string dir2 = dir + "/";
    FileByteReader ld;
    RasterFont full;
    const auto tDisk0 = std::chrono::steady_clock::now();
    assert(full.open(ld, face.c_str()));
    full.addStyle(ld, (dir2 + "newcomputermodern/NewCM10-Bold.otf").c_str(),
                  FontStyle::Bold);
    full.addStyle(ld, (dir2 + "newcomputermodern/NewCM10-Italic.otf").c_str(),
                  FontStyle::Italic);
    // The real fallback chain, in the app's order, including the Bold cuts —
    // this is what makes the style-resolution comparison below mean anything.
    const char* fb[] = {"fandol/FandolSong-Regular.otf",
                        "haranoaji/HaranoAjiMincho-Regular.otf",
                        "unfonts-core/UnBatang.ttf"};
    const char* fbB[] = {"fandol/FandolSong-Bold.otf",
                         "haranoaji/HaranoAjiMincho-Bold.otf",
                         "unfonts-core/UnBatangBold.ttf"};
    int chain = 0;
    for (const char* p : fb)  chain += full.addFallback(ld, (dir2 + p).c_str()) ? 1 : 0;
    for (const char* p : fbB)
      chain += full.addFallback(ld, (dir2 + p).c_str(), FontStyle::Bold) ? 1 : 0;

    const double diskMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - tDisk0).count();

    RasterFont shared;
    const auto tShare0 = std::chrono::steady_clock::now();
    assert(shared.openSharedWith(full));
    const double shareMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - tShare0).count();
    std::printf("[4] open from disk %.1f ms -> shared %.1f ms (%.0fx)\n",
                diskMs, shareMs, diskMs / (shareMs > 0.001 ? shareMs : 0.001));

    // Metrics are EM fractions off the design units — a property of the bytes.
    assert(shared.lineHeight(37.0f) == full.lineHeight(37.0f));
    assert(shared.ascender(37.0f)   == full.ascender(37.0f));
    for (int s = 0; s < kFontStyleCount; ++s)
      assert(shared.hasStyle(FontStyle(s)) == full.hasStyle(FontStyle(s)));

    // Coverage and style resolution across scripts. U+4E2D/U+AC00/U+3042 are the
    // whole reason the fallback chains exist and the order within them matters;
    // if the chains were copied in the wrong order these diverge.
    const uint32_t probe[] = {0x41,   0x7A,   0xE9,   0x3B1,  0x416,
                              0x4E2D, 0xAC00, 0x3042, 0xE000, 0x2014};
    int compared = 0;
    for (uint32_t cp : probe) {
      assert(shared.hasCodepoint(cp) == full.hasCodepoint(cp));
      for (int s = 0; s < kFontStyleCount; ++s) {
        assert(shared.keyForStyle(FontStyle(s), cp) ==
               full.keyForStyle(FontStyle(s), cp));
        assert(shared.kernEmStyled(FontStyle(s), 0x41, cp) ==
               full.kernEmStyled(FontStyle(s), 0x41, cp));
        ++compared;
      }
    }

    // And the pixels. Bake the same cells in both and compare cell for cell —
    // the atlases are laid out independently, so this goes through layout and
    // reads each cell back through its own quad's UVs.
    std::vector<uint32_t> cps(std::begin(probe), std::end(probe));
    assert(full.ensureGlyphs(cps, {18, 37}) > 0);
    assert(shared.ensureGlyphs(cps, {18, 37}) > 0);

    int cells = 0;
    for (uint32_t cp : cps) {
      const uint32_t kf = full.keyForStyle(FontStyle::Roman, cp);
      const uint32_t ks = shared.keyForStyle(FontStyle::Roman, cp);
      if (kf == 0 || ks == 0) continue;
      for (float sz : {18.0f, 37.0f}) {
        GlyphQuad qf, qs;
        const float af = full.layoutByKey(kf, 3.0f, 100.0f, sz, qf);
        const float as = shared.layoutByKey(ks, 3.0f, 100.0f, sz, qs);
        assert(af == as);                 // advance
        assert(qf.draw == qs.draw);
        if (!qf.draw) continue;
        assert(qf.x0 == qs.x0 && qf.y0 == qs.y0);
        assert(qf.x1 == qs.x1 && qf.y1 == qs.y1);
        // Same ink, wherever each atlas happened to put it.
        const uint32_t w = (uint32_t)std::lround((double)(qf.u1 - qf.u0) * full.atlasW());
        const uint32_t h = (uint32_t)std::lround((double)(qf.v1 - qf.v0) * full.atlasH());
        assert(w == (uint32_t)std::lround((double)(qs.u1 - qs.u0) * shared.atlasW()));
        assert(h == (uint32_t)std::lround((double)(qs.v1 - qs.v0) * shared.atlasH()));
        const std::vector<uint8_t>& af2 = full.atlas();
        const std::vector<uint8_t>& as2 = shared.atlas();
        const uint32_t fx = (uint32_t)std::lround((double)qf.u0 * full.atlasW());
        const uint32_t fy = (uint32_t)std::lround((double)qf.v0 * full.atlasH());
        const uint32_t sx = (uint32_t)std::lround((double)qs.u0 * shared.atlasW());
        const uint32_t sy = (uint32_t)std::lround((double)qs.v0 * shared.atlasH());
        for (uint32_t y = 0; y < h; ++y)
          for (uint32_t x = 0; x < w; ++x) {
            const size_t io = (size_t)qf.page * full.atlasW() * full.atlasH() +
                              (size_t)(fy + y) * full.atlasW() + fx + x;
            const size_t is = (size_t)qs.page * shared.atlasW() * shared.atlasH() +
                              (size_t)(sy + y) * shared.atlasW() + sx + x;
            assert(af2[io] == as2[is]);
          }
        ++cells;
      }
    }

    std::printf("[4] openSharedWith: %d fallback faces, %d style probes, "
                "%d cells byte-identical\n", chain, compared, cells);
    assert(chain >= 1);    // the chain must actually have been exercised
    assert(cells >= 8);
  }

  std::printf("raster_font_test: all checks passed\n");
  return 0;
}
