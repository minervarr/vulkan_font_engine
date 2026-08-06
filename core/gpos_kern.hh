#pragma once
// GPOS kern-pair extraction.
//
// New Computer Modern (and every other modern OTF this engine ships) has NO
// legacy `kern` table — kerning lives in GPOS. That means FreeType's
// FT_Get_Kerning() returns 0 for every pair, which is why text was drawn with
// raw advance widths and no kerning at all until this existed.
//
// Verified on assets/fonts/newcomputermodern/NewCM10-Regular.otf: no `kern`
// table, GPOS present, 16390 non-zero pairs, of which 1323 fall inside the
// charset MsdfFont::generate() bakes. The values are not cosmetic — "AV" is
// -0.111 em and "To"/"Wa"/"Yo"/"P."/"LT" are -0.083 em, i.e. 1.2-1.6 px of
// error at the 11-18 px sizes this UI draws.
//
// This parser is deliberately self-contained (raw font bytes in, table out):
// no FreeType, no msdfgen, no Vulkan — so gpos_kern_test can link it alone and
// the bounds checking can be asserted directly. Every read is range-checked;
// a malformed or truncated font must yield an empty/partial table, never a
// crash or an out-of-bounds read.

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace vfe {

// Key for a kern pair: the two CODEPOINTS, not glyph ids.
//
// The text path (MsdfFont::layout / textWidth) is codepoint-addressed, and the
// glyph tables it reads are keyed by codepoint too, so resolving to gids at
// draw time would mean carrying a second mapping for no benefit. Pairs whose
// glyphs have no codepoint in the font's cmap are dropped — they are only
// reachable through GSUB substitutions this engine does not perform.
inline constexpr uint64_t kernKey(uint32_t cpA, uint32_t cpB) {
  return (static_cast<uint64_t>(cpA) << 32) | cpB;
}

// cpA,cpB -> horizontal adjustment in EM (negative = tighten). Multiply by the
// pixel size at layout time, exactly like MsdfGlyph::advance.
using KernTable = std::unordered_map<uint64_t, float>;

// Parses `data` (a complete OTF/TTF file image) and fills `out` with every
// non-zero horizontal kern pair reachable from the GPOS `kern` feature.
//
// Returns false if the font has no usable GPOS kern data (not an error — some
// faces genuinely have none; the caller should simply lay out unkerned).
// `out` is added to, not cleared, so several faces can accumulate if wanted.
bool parseGposKernPairs(const uint8_t* data, size_t size, KernTable& out);

}  // namespace vfe
