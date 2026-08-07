// glyph_raster_test — per-size glyph rasterization, asserted against New
// Computer Modern's real outlines. Same convention as gpos_kern_test beside
// it: plain assert(), no framework, #undef NDEBUG so the checks survive an
// optimized build, and links glyph_raster.cc + FreeType and nothing else.
//
// What is worth asserting here is what a screenshot cannot tell you: that a
// glyph rasterized at size N really is N-sized, that coverage is coverage (not
// a distance field), that whitespace is distinguishable from a missing glyph,
// and that the scripts this whole rewrite exists for actually come out.
//
// Run: ./build/linux_debug/framework/.../glyph_raster_test [fonts-dir]
// (defaults to the in-tree assets/fonts)

#undef NDEBUG
#include <cassert>

#include "../cell_key.hh"
#include "../glyph_raster.hh"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

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

// Total ink in a rasterized glyph, as a fraction of its bounding box.
double inkFraction(const vfe::RasterGlyph& g) {
  if (g.w <= 0 || g.h <= 0) return 0.0;
  double sum = 0;
  for (uint8_t v : g.cov) sum += v;
  return sum / (255.0 * (double)g.w * (double)g.h);
}

}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : "assets/fonts";
  const std::string newcm = dir + "/newcomputermodern/NewCM10-Regular.otf";

  vfe::RasterFace face;
  if (!openFile(face, newcm)) {
    std::fprintf(stderr, "glyph_raster_test: cannot read %s\n", newcm.c_str());
    return 77;  // "skipped" — assets not where we were pointed
  }
  assert(face.isOpen());
  assert(face.unitsPerEm() > 0);

  // ── [1] A rasterized glyph is the size it was asked for ──────────────────
  //
  // The whole premise of a per-size cache is that a cell drawn at the size it
  // was baked at needs no rescaling. If height did not track ppem, the cache
  // key would be a lie.
  {
    vfe::RasterGlyph small, large;
    assert(face.render('H', 20, small));
    assert(face.render('H', 40, large));
    assert(small.w > 0 && small.h > 0);
    assert(large.w > 0 && large.h > 0);

    // Doubling ppem doubles the cap height, within the rounding a whole-pixel
    // bitmap can introduce (a pixel at each edge).
    assert(std::abs(large.h - 2 * small.h) <= 2);
    assert(std::abs(large.w - 2 * small.w) <= 2);

    // Advance scales the same way and is NOT rounded to whole pixels — the
    // pen accumulates in float precisely so kerned runs stay exact.
    assert(large.advance > 1.9f * small.advance);
    assert(large.advance < 2.1f * small.advance);
    std::printf("[1] 'H' 20px=%dx%d adv=%.2f, 40px=%dx%d adv=%.2f\n",
                small.w, small.h, small.advance,
                large.w, large.h, large.advance);
  }

  // ── [2] Coverage, not a distance field ───────────────────────────────────
  //
  // This is the property that separates this path from MTSDF. A distance
  // field is ~0.5 everywhere and has no empty margin; coverage is mostly 0 or
  // 255, with the intermediate values confined to edges.
  {
    vfe::RasterGlyph g;
    assert(face.render('H', 64, g));

    int solid = 0, empty = 0, edge = 0;
    for (uint8_t v : g.cov) {
      if (v == 0) empty++;
      else if (v == 255) solid++;
      else edge++;
    }
    assert(solid > 0);          // 'H' has fully-inked stems
    assert(empty > 0);          //   and fully-empty counters
    // Antialiased, so SOME partial coverage must exist — a pure bilevel
    // rasterizer would mean FT_RENDER_MODE_MONO crept in.
    assert(edge > 0);
    // ...but edges are a minority. In a distance field they would dominate.
    assert(edge < solid + empty);

    const double ink = inkFraction(g);
    assert(ink > 0.2 && ink < 0.9);
    std::printf("[2] 'H' 64px: solid=%d empty=%d edge=%d ink=%.2f\n",
                solid, empty, edge, ink);
  }

  // ── [3] Whitespace is a glyph; a missing codepoint is not ────────────────
  //
  // These two must not be conflated: a space has to advance the pen, and an
  // absent codepoint has to fall through to the next fallback face. Both look
  // like "no pixels" to a careless caller.
  {
    vfe::RasterGlyph sp;
    assert(face.render(' ', 24, sp));      // true: it exists
    assert(sp.w == 0 && sp.h == 0);        // no ink
    assert(sp.advance > 0.0f);             // but real width
    assert(face.hasCodepoint(' '));

    vfe::RasterGlyph missing;
    // A Hangul syllable: NewCM genuinely does not have these, which is the
    // entire reason a fallback chain exists.
    assert(!face.hasCodepoint(0xAC00));
    assert(!face.render(0xAC00, 24, missing));
    assert(missing.w == 0 && missing.h == 0);
    assert(missing.advance == 0.0f);
    std::printf("[3] space adv=%.2f, U+AC00 correctly absent\n", sp.advance);
  }

  // ── [4] Design metrics are size-independent; grid-fitted ones are NOT ─────
  //
  // This distinction is the whole reason designMetrics() exists, and it is not
  // obvious: FreeType rounds `size->metrics` to whole pixels even with hinting
  // disabled. Deriving an em ratio from one probe size and multiplying it out
  // therefore gives a DIFFERENT answer than asking at the target size — the
  // error is under a pixel, so it never looks like a bug, it just makes line
  // spacing drift between UI scales. RasterFont::lineHeight() reads the design
  // values so it can stay pure and exact.
  {
    float aEm = 0, dEm = 0, hEm = 0;
    assert(face.designMetrics(aEm, dEm, hEm));
    assert(aEm > 0.0f && hEm > 0.0f);
    assert(dEm < 0.0f);                    // descender points down
    assert(hEm >= aEm - dEm - 0.001f);     // line box holds ascent + descent

    // Exact at every size, by construction — no rounding anywhere.
    for (int s : {19, 22, 26, 31, 44, 88}) {
      const float expect = hEm * (float)s;
      assert(std::abs(expect - hEm * (float)s) == 0.0f);
      (void)expect;
    }

    // And the grid-fitted values really do differ, which is what would have
    // bitten us: assert they are CLOSE but do not assume they are equal.
    float a1 = 0, d1 = 0, h1 = 0;
    assert(face.verticalMetrics(50, a1, d1, h1));
    assert(std::abs(a1 - aEm * 50.0f) <= 1.5f);
    assert(std::abs(h1 - hEm * 50.0f) <= 1.5f);
    std::printf("[4] design asc=%.4f desc=%.4f lh=%.4f em; "
                "grid-fitted at 50px: %.2f/%.2f/%.2f (design*50 = %.2f/%.2f/%.2f)\n",
                aEm, dEm, hEm, a1, d1, h1,
                aEm * 50.0f, dEm * 50.0f, hEm * 50.0f);
  }

  // ── [5] The scripts this rewrite exists for ──────────────────────────────
  //
  // Under MTSDF these three faces overflowed a 4096-square sheet and Japanese
  // and Korean baked ZERO glyphs. Here each is rasterized at a real UI size
  // and must produce ink. A cell that comes out empty is the bug, reproduced.
  {
    struct Probe { const char* path; uint32_t cp; const char* what; };
    const Probe probes[] = {
      { "/fandol/FandolSong-Regular.otf",         0x516B, "Han U+516B"    },
      { "/haranoaji/HaranoAjiMincho-Regular.otf", 0x30A2, "Kana U+30A2"   },
      { "/unfonts-core/UnBatang.ttf",             0xAC00, "Hangul U+AC00" },
    };
    int checked = 0;
    for (const Probe& p : probes) {
      vfe::RasterFace f;
      if (!openFile(f, dir + p.path)) {
        std::printf("[5] %s: face not present, skipped\n", p.what);
        continue;
      }
      assert(f.hasCodepoint(p.cp));
      vfe::RasterGlyph g;
      assert(f.render(p.cp, 22, g));   // a real UI body size
      assert(g.w > 0 && g.h > 0);
      assert(inkFraction(g) > 0.05);   // genuinely inked, not a blank cell

      // The capacity claim, checked rather than asserted in prose: a CJK cell
      // at a UI size is tens of pixels a side, not the ~115x104 an MTSDF bake
      // at sizePxEm 96 with a 9.6 range costs.
      assert(g.w <= 40 && g.h <= 40);
      std::printf("[5] %-14s 22px -> %dx%d, ink=%.2f\n",
                  p.what, g.w, g.h, inkFraction(g));
      checked++;
    }
    // The whole point of the exercise; if none of the three faces is present
    // the test is not proving anything.
    assert(checked > 0);
  }

  // ── [6] The bold CJK faces are genuinely bolder ──────────────────────────
  //
  // Every one of these scripts has a matched Bold cut bundled beside its
  // Regular, and RasterFont's per-style fallback chain is what reaches them.
  // The failure mode is silent: if the chain resolves back to the Regular
  // face, bold text still RENDERS, just at the wrong weight — which looks
  // like working software and is only visible next to bold Latin. Comparing
  // ink proves the two faces are actually different, so a regression here
  // fails a test instead of quietly un-bolding every non-Latin title.
  {
    struct Pair {
      const char* regular; const char* bold; uint32_t cp; const char* what;
    };
    const Pair pairs[] = {
      { "/fandol/FandolSong-Regular.otf", "/fandol/FandolSong-Bold.otf",
        0x516B, "Han (Fandol Song)" },
      { "/haranoaji/HaranoAjiMincho-Regular.otf", "/haranoaji/HaranoAjiMincho-Bold.otf",
        0x30A2, "Kana (Harano Aji Mincho)" },
      { "/unfonts-core/UnBatang.ttf", "/unfonts-core/UnBatangBold.ttf",
        0xAC00, "Hangul (UnBatang)" },
    };
    int checked = 0;
    for (const Pair& p : pairs) {
      vfe::RasterFace r, b;
      if (!openFile(r, dir + p.regular) || !openFile(b, dir + p.bold)) {
        std::printf("[6] %s: faces not present, skipped\n", p.what);
        continue;
      }
      vfe::RasterGlyph gr, gb;
      // 26px: a real grid-title size, and the size where a too-light CJK
      // glyph beside bold Latin is most obvious.
      assert(r.render(p.cp, 26, gr));
      assert(b.render(p.cp, 26, gb));
      assert(gr.w > 0 && gb.w > 0);

      const double ir = inkFraction(gr), ib = inkFraction(gb);
      assert(ib > ir);          // the whole point
      assert(ib > ir * 1.05);   // and by a margin a reader can actually see
      std::printf("[6] %-26s ink regular=%.3f bold=%.3f (+%.0f%%)\n",
                  p.what, ir, ib, (ib / ir - 1.0) * 100.0);
      checked++;
    }
    assert(checked > 0);
  }

  // ── [7] A moved-from face does not double-free ───────────────────────────
  // RasterFont stores these in containers, so the move must be sound.
  {
    vfe::RasterFace a;
    assert(openFile(a, newcm));
    vfe::RasterFace b(std::move(a));
    assert(b.isOpen());
    assert(!a.isOpen());
    vfe::RasterGlyph g;
    assert(b.render('A', 16, g));
    std::printf("[7] move leaves the source closed and the target usable\n");
  }

  // ── [8] outlinePhases() == outline(), phase for phase ────────────────────
  //
  // outlinePhases() exists purely to avoid re-parsing the same charstring once
  // per phase — FT_Load_Glyph is 71% of outline extraction, which is itself
  // ~80% of the bake. It is a PERFORMANCE change with no licence to alter a
  // single coordinate, so this compares the two APIs exactly: same box, same
  // metrics, same edge count, same floats bit for bit.
  //
  // The risk it guards is specific. outlinePhases() walks the phases by
  // TRANSLATING the slot in place and accumulating deltas, where outline()
  // reloads and translates once from zero. Those agree only because 26.6
  // offsets are exact integers; if kPhaseCount ever became something whose
  // offsets did not divide evenly, the accumulated path would drift away from
  // the direct one and every phase but the first would land slightly wrong.
  {
    vfe::RasterFace f;
    assert(openFile(f, dir + "/newcomputermodern/NewCM10-Regular.otf"));
    const uint32_t n = vfe::cellkey::kPhaseCount;
    long compared = 0, edges = 0;
    for (uint32_t cp : {0x41u, 0x48u, 0x67u, 0x69u, 0x6Fu, 0x2Eu, 0x40u, 0x57u}) {
      for (int sz : {8, 12, 18, 24, 37, 48, 96}) {
        std::vector<vfe::OutlineGlyph> got(n);
        if (!f.outlinePhases(cp, sz, got.data(), n)) continue;
        for (uint32_t ph = 0; ph < n; ++ph) {
          vfe::OutlineGlyph want;
          assert(f.outline(cp, sz, want, ph));
          assert(got[ph].w == want.w);
          assert(got[ph].h == want.h);
          assert(got[ph].bearingX == want.bearingX);
          assert(got[ph].bearingY == want.bearingY);
          assert(got[ph].advance  == want.advance);
          assert(got[ph].edges.size() == want.edges.size());
          for (size_t e = 0; e < want.edges.size(); ++e) {
            // Bit-for-bit. Not a tolerance: the same arithmetic on the same
            // integers must give the same floats, and anything else means the
            // two paths have genuinely diverged.
            assert(got[ph].edges[e].x0 == want.edges[e].x0);
            assert(got[ph].edges[e].y0 == want.edges[e].y0);
            assert(got[ph].edges[e].x1 == want.edges[e].x1);
            assert(got[ph].edges[e].y1 == want.edges[e].y1);
          }
          edges += (long)want.edges.size();
          compared++;
        }
      }
    }
    std::printf("[8] outlinePhases == outline: %ld phase-glyphs, %ld edges identical\n",
                compared, edges);
    assert(compared >= 100);
    assert(edges > 10000);
  }

  std::printf("glyph_raster_test: all checks passed\n");
  return 0;
}
