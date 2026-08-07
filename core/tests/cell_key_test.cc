// cell_key_test — the raster cache's two key codecs. Same convention as the
// other core/tests/*.cc: plain assert(), no framework, #undef NDEBUG so the
// checks survive an optimized build, and links NOTHING — cell_key.hh is
// header-only and depends on text_font.hh alone.
//
// cell_key.hh already proves its round-trips with static_assert, so a broken
// codec cannot even compile. This file covers what a static_assert cannot say
// well: the concrete historical bug, and exhaustive sweeps.

#undef NDEBUG
#include <cassert>
#include <cstdio>

#include "../cell_key.hh"

using namespace vfe::cellkey;

int main() {
  // ── [1] Round-trip, exhaustively over every style and a full size sweep ──
  {
    int n = 0;
    for (int s = 0; s < kFontStyleCount; ++s) {
      for (uint32_t sz = 1; sz <= kMaxCellPx; ++sz) {
        for (uint32_t cp : {0x20u, 0x41u, 0xFFu, 0x4E2Du, 0xAC00u, kMaxCodepoint}) {
          const FontStyle style = FontStyle(s);
          const CellFields f = decodeCell(encodeCell(style, sz, cp));
          assert(f.style == style);
          assert(f.sizePx == sz);
          assert(f.cp == cp);
          ++n;
        }
      }
    }
    std::printf("[1] cell key round-trip: %d combinations\n", n);
  }

  // ── [2] Field independence: the bug this header was written for ─────────
  //
  // The old codec put the style byte inside the size field, so decoding a
  // non-Roman key returned `sizePx | style << 24`. A 37px Italic cell came
  // back as 50,331,685 px, which FreeType dutifully tried to rasterize. The
  // assertion is not "the mask is right"; it is that the style cannot reach
  // the size at all.
  {
    for (int s = 0; s < kFontStyleCount; ++s) {
      const uint64_t k = encodeCell(FontStyle(s), 37, 0x41);
      assert(decodeCell(k).sizePx == 37);
      assert(decodeCell(k).cp == 0x41);
      assert(decodeCell(k).sizePx != 50331685u);   // the observed wrong answer
    }
    // ... and symmetrically, the size cannot reach the style.
    for (uint32_t sz = 1; sz <= kMaxCellPx; ++sz)
      assert(decodeCell(encodeCell(FontStyle::Italic, sz, 0x41)).style ==
             FontStyle::Italic);
    std::printf("[2] style and size do not overlap\n");
  }

  // ── [3] Distinct inputs give distinct keys ──────────────────────────────
  {
    assert(encodeCell(FontStyle::Roman, 20, 0x41) !=
           encodeCell(FontStyle::Bold, 20, 0x41));
    assert(encodeCell(FontStyle::Roman, 20, 0x41) !=
           encodeCell(FontStyle::Roman, 21, 0x41));
    assert(encodeCell(FontStyle::Roman, 20, 0x41) !=
           encodeCell(FontStyle::Roman, 20, 0x42));

    // A size difference must never be expressible as a codepoint difference,
    // which is the collision an under-wide field would produce.
    for (uint32_t sz = 1; sz < 64; ++sz)
      assert(encodeCell(FontStyle::Roman, sz, kMaxCodepoint) !=
             encodeCell(FontStyle::Roman, sz + 1, 0));
    std::printf("[3] distinct inputs, distinct keys\n");
  }

  // ── [3b] The phase field, held to the same standard ─────────────────────
  //
  // Exhaustive over every (style, size, phase): the phase is the newest field
  // and therefore the one a later edit is most likely to give an overlapping
  // position. The failure mode is not subtle in its consequences and is
  // completely invisible in its symptoms — a phase leaking into the size field
  // asks FreeType for a nonsense bake (that is the 50-million-pixel bug again,
  // in a new field), and a phase that collapses into another cell makes three
  // bakes silently share one slot, which just looks like the old whole-pixel
  // snapping nobody could see was wrong in the first place.
  {
    int n = 0;
    for (int s = 0; s < kFontStyleCount; ++s) {
      for (uint32_t sz = 1; sz <= kMaxCellPx; ++sz) {
        for (uint32_t ph = 0; ph < kPhaseCount; ++ph) {
          const FontStyle style = FontStyle(s);
          const CellFields f = decodeCell(encodeCell(style, sz, 0x4E2D, ph));
          assert(f.style == style);
          assert(f.sizePx == sz);
          assert(f.cp == 0x4E2D);
          assert(f.phase == ph);
          ++n;
        }
        // Every phase of one cell is a DIFFERENT key. Without this the whole
        // feature is a no-op that still pays for itself in atlas memory.
        for (uint32_t a = 0; a < kPhaseCount; ++a)
          for (uint32_t b = a + 1; b < kPhaseCount; ++b)
            assert(encodeCell(FontStyle(s), sz, 0x41, a) !=
                   encodeCell(FontStyle(s), sz, 0x41, b));
      }
    }
    // An omitted phase means phase 0 — what every advance query relies on.
    assert(encodeCell(FontStyle::Bold, 22, 0x41) ==
           encodeCell(FontStyle::Bold, 22, 0x41, 0));
    std::printf("[3b] phase round-trip and independence: %d combinations\n", n);
  }

  // ── [4] Glyph key: zero means "no key", and nothing valid collides ──────
  //
  // canvas.cc tests `key == 0` on every text call, and keyForStyle() returns 0
  // for "this style has nothing of its own here". A valid key that encoded to
  // zero would silently route styled text down the default-face path.
  {
    assert(!glyphKeyValid(kNoGlyphKey));
    for (int s = 0; s < kFontStyleCount; ++s) {
      for (uint32_t cp : {0u, 0x20u, 0x41u, 0x4E2Du, kMaxCodepoint}) {
        const uint32_t k = encodeGlyph(FontStyle(s), cp);
        assert(k != kNoGlyphKey);
        assert(glyphKeyValid(k));
        assert(glyphStyle(k) == FontStyle(s));
        assert(glyphCp(k) == cp);
      }
    }
    // Roman + U+0000 is the encoding most likely to fall out as zero.
    assert(encodeGlyph(FontStyle::Roman, 0) != kNoGlyphKey);
    std::printf("[4] glyph key validity and zero contract\n");
  }

  // ── [5] Glyph keys are distinct across the whole codepoint range ────────
  {
    for (uint32_t cp = 0; cp <= kMaxCodepoint; cp += 997) {
      const uint32_t roman = encodeGlyph(FontStyle::Roman, cp);
      const uint32_t bold = encodeGlyph(FontStyle::Bold, cp);
      assert(roman != bold);
      assert(glyphCp(roman) == cp);
      assert(glyphCp(bold) == cp);
    }
    std::printf("[5] glyph keys distinct across Unicode\n");
  }

  std::printf("cell_key_test: all checks passed\n");
  return 0;
}
