#pragma once
#include <cstdint>

#include "text_font.hh"

// ── The two keys the raster glyph cache is built on ─────────────────────────
//
// This header exists because the hand-rolled versions of these were WRONG, in
// a way nothing could see. RasterFont packed a cell key as
//
//     style << 56 | sizePx << 32 | codepoint
//
// and decoded the size back with `(k >> 32) & 0xFFFFFFFF`. The size field is
// 32 bits wide starting at bit 32, so it ENDS at bit 63 — the style byte sat
// inside it. Every non-Roman key decoded to `sizePx | style << 24`, and a
// missed 37px Italic cell was handed to FreeType as a request for a
// 50,331,685-pixel glyph. It rasterized something enormous, failed to pack,
// and was blacklisted forever. Bold and italic text simply stopped appearing
// at any size the eager ladder did not already cover.
//
// The fix is not "use a better mask". It is to stop writing shifts by hand:
//
//   * field WIDTHS are declared, and every shift is DERIVED from them, so two
//     fields cannot be given overlapping positions;
//   * a static_assert proves the fields fit in the word;
//   * a static_assert proves encode/decode round-trips at each field's
//     extremes, evaluated by the compiler, so a bad edit cannot build.
//
// Keep this header free of everything but <cstdint> and text_font.hh: it is
// checked by cell_key_test, which links nothing at all.

namespace vfe {
namespace cellkey {

// ── Shared limits ──────────────────────────────────────────────────────────

// Unicode's last codepoint. Not a policy — the standard's ceiling.
inline constexpr uint32_t kMaxCodepoint = 0x10FFFF;

// The largest cell the cache will ever bake, in pixels.
//
// This is a real cap, not a formality: it is the last line of defence between
// a corrupt size and FreeType being asked to allocate a bitmap measured in
// gigabytes. Generous against the sizes that actually occur — the app's
// largest type role is ~120 px at 8K, and an icon box a little more — while
// staying well inside a page (see shelf_packer.hh).
inline constexpr uint32_t kMaxCellPx = 512;

// ── Cell key: (style, sizePx, codepoint) -> uint64 ─────────────────────────
//
// Identifies one baked cell. Widths first; positions are derived below and are
// deliberately not written out anywhere.

inline constexpr int kCpBits    = 21;   // 0x10FFFF needs 21
inline constexpr int kSizeBits  = 13;   // kMaxCellPx needs 10; 13 leaves room
inline constexpr int kStyleBits = 3;    // kFontStyleCount == 4 needs 2

inline constexpr int kCpShift    = 0;
inline constexpr int kSizeShift  = kCpShift   + kCpBits;
inline constexpr int kStyleShift = kSizeShift + kSizeBits;

inline constexpr uint64_t kCpMask    = (uint64_t{1} << kCpBits)    - 1;
inline constexpr uint64_t kSizeMask  = (uint64_t{1} << kSizeBits)  - 1;
inline constexpr uint64_t kStyleMask = (uint64_t{1} << kStyleBits) - 1;

static_assert(kStyleShift + kStyleBits <= 64,
              "cell key fields do not fit in a uint64_t");
static_assert(kMaxCodepoint <= kCpMask,
              "codepoint field is too narrow for Unicode");
static_assert(kMaxCellPx <= kSizeMask,
              "size field is too narrow for kMaxCellPx");
static_assert(uint64_t(kFontStyleCount) - 1 <= kStyleMask,
              "style field is too narrow for kFontStyleCount");

struct CellFields {
  FontStyle style = FontStyle::Roman;
  uint32_t  sizePx = 0;
  uint32_t  cp = 0;
};

// Values wider than their field are masked rather than aliased into the next
// one. Callers must not rely on that: quantize() clamps the size and faceFor()
// only ever sees real codepoints. It exists so a future caller's mistake stays
// inside its own field instead of silently becoming a different cell.
inline constexpr uint64_t encodeCell(FontStyle style, uint32_t sizePx,
                                     uint32_t cp) {
  return ((uint64_t(uint8_t(style)) & kStyleMask) << kStyleShift) |
         ((uint64_t(sizePx)         & kSizeMask)  << kSizeShift)  |
         ((uint64_t(cp)             & kCpMask)    << kCpShift);
}

inline constexpr CellFields decodeCell(uint64_t key) {
  return CellFields{
      FontStyle(uint8_t((key >> kStyleShift) & kStyleMask)),
      uint32_t((key >> kSizeShift) & kSizeMask),
      uint32_t((key >> kCpShift) & kCpMask),
  };
}

// ── Glyph key: (style, codepoint) -> uint32 ────────────────────────────────
//
// The handle Canvas receives from keyForStyle() and hands back to
// layoutByKey()/advanceKey(). It carries no size — the size travels alongside
// it as a float, because the same key is drawn at different sizes.
//
// ZERO MEANS "NO KEY", and that contract is load-bearing: keyForStyle()
// returns 0 to say "this style has nothing of its own for this codepoint, use
// the default face", and canvas.cc tests `key == 0` on every text call. So the
// encoding carries an explicit validity bit rather than the old
// `(style + 1) << 24` trick, whose decoder `(uint8_t)((key >> 24) - 1)`
// underflowed to 255 for any key with a zero style nibble.

inline constexpr uint32_t kGlyphCpBits    = kCpBits;
inline constexpr uint32_t kGlyphStyleBits = kStyleBits;
inline constexpr uint32_t kGlyphCpShift    = 0;
inline constexpr uint32_t kGlyphStyleShift = kGlyphCpShift + kGlyphCpBits;
inline constexpr uint32_t kGlyphValidShift = kGlyphStyleShift + kGlyphStyleBits;

inline constexpr uint32_t kGlyphCpMask    = (uint32_t{1} << kGlyphCpBits) - 1;
inline constexpr uint32_t kGlyphStyleMask = (uint32_t{1} << kGlyphStyleBits) - 1;
inline constexpr uint32_t kGlyphValidBit  = uint32_t{1} << kGlyphValidShift;

static_assert(kGlyphValidShift < 32, "glyph key fields do not fit in a uint32_t");
static_assert(kMaxCodepoint <= kGlyphCpMask,
              "glyph key codepoint field is too narrow for Unicode");
static_assert(uint32_t(kFontStyleCount) - 1 <= kGlyphStyleMask,
              "glyph key style field is too narrow for kFontStyleCount");

inline constexpr uint32_t kNoGlyphKey = 0;

inline constexpr uint32_t encodeGlyph(FontStyle style, uint32_t cp) {
  return kGlyphValidBit |
         ((uint32_t(uint8_t(style)) & kGlyphStyleMask) << kGlyphStyleShift) |
         ((cp & kGlyphCpMask) << kGlyphCpShift);
}

inline constexpr bool glyphKeyValid(uint32_t key) {
  return (key & kGlyphValidBit) != 0;
}

inline constexpr FontStyle glyphStyle(uint32_t key) {
  return FontStyle(uint8_t((key >> kGlyphStyleShift) & kGlyphStyleMask));
}

inline constexpr uint32_t glyphCp(uint32_t key) {
  return (key >> kGlyphCpShift) & kGlyphCpMask;
}

// ── Round-trips, proved at compile time ────────────────────────────────────
//
// The extremes of every field, in both codecs. This is the check the old code
// never had; it is what makes the 50-million-pixel bug a build failure.

static_assert(decodeCell(encodeCell(FontStyle::Italic, 37, 0x41)).sizePx == 37, "");
static_assert(decodeCell(encodeCell(FontStyle::Italic, 37, 0x41)).cp == 0x41, "");
static_assert(decodeCell(encodeCell(FontStyle::Italic, 37, 0x41)).style ==
                  FontStyle::Italic, "");

static_assert(decodeCell(encodeCell(FontStyle(kFontStyleCount - 1), uint32_t(kSizeMask),
                                    kMaxCodepoint)).sizePx == uint32_t(kSizeMask), "");
static_assert(decodeCell(encodeCell(FontStyle(kFontStyleCount - 1), uint32_t(kSizeMask),
                                    kMaxCodepoint)).cp == kMaxCodepoint, "");
static_assert(decodeCell(encodeCell(FontStyle(kFontStyleCount - 1), uint32_t(kSizeMask),
                                    kMaxCodepoint)).style ==
                  FontStyle(kFontStyleCount - 1), "");

static_assert(decodeCell(encodeCell(FontStyle::Roman, 1, 0)).sizePx == 1, "");
static_assert(encodeCell(FontStyle::Roman, 0, 0) == 0, "");

// A style change must not disturb the size, and a size change must not disturb
// the style. Stated as its own assertion because that is precisely what broke.
static_assert(decodeCell(encodeCell(FontStyle::Bold, 22, 0x4E2D)).sizePx == 22, "");
static_assert(decodeCell(encodeCell(FontStyle::Math, 22, 0x4E2D)).sizePx == 22, "");
static_assert(encodeCell(FontStyle::Bold, 22, 0x4E2D) !=
                  encodeCell(FontStyle::Math, 22, 0x4E2D), "");

static_assert(glyphKeyValid(encodeGlyph(FontStyle::Roman, 0)), "");
static_assert(encodeGlyph(FontStyle::Roman, 0) != kNoGlyphKey,
              "a valid glyph key must never collide with kNoGlyphKey");
static_assert(!glyphKeyValid(kNoGlyphKey), "");
static_assert(glyphStyle(encodeGlyph(FontStyle::Italic, kMaxCodepoint)) ==
                  FontStyle::Italic, "");
static_assert(glyphCp(encodeGlyph(FontStyle::Italic, kMaxCodepoint)) ==
                  kMaxCodepoint, "");

}  // namespace cellkey
}  // namespace vfe
