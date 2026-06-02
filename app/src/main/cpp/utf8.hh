#pragma once
#include <cstdint>
#include <cstddef>
#include <string_view>

// Minimal UTF-8 decoder. Text throughout the app is UTF-8 (whisper output,
// keyboard input), but glyph lookup keys on Unicode codepoints. Iterating a
// std::string byte-by-byte splits multibyte characters (e.g. "á" = 0xC3 0xA1)
// into invalid lookups, so accented Latin / non-ASCII text never renders.
//
// nextCodepoint advances `i` past one UTF-8 sequence and returns its codepoint.
// On a malformed/truncated sequence it consumes a single byte and returns the
// Unicode replacement character so callers always make forward progress.
namespace utf8 {

constexpr uint32_t kReplacement = 0xFFFD;

inline uint32_t nextCodepoint(std::string_view s, size_t& i) {
  if (i >= s.size()) return 0;
  unsigned char b0 = (unsigned char)s[i];

  // ASCII fast path.
  if (b0 < 0x80) { i += 1; return b0; }

  int extra;       // continuation bytes following the lead byte
  uint32_t cp;
  if ((b0 & 0xE0) == 0xC0) { extra = 1; cp = b0 & 0x1F; }
  else if ((b0 & 0xF0) == 0xE0) { extra = 2; cp = b0 & 0x0F; }
  else if ((b0 & 0xF8) == 0xF0) { extra = 3; cp = b0 & 0x07; }
  else { i += 1; return kReplacement; }  // stray continuation / invalid lead

  // Need `extra` more continuation bytes (10xxxxxx).
  if (i + (size_t)extra >= s.size()) { i += 1; return kReplacement; }
  for (int k = 1; k <= extra; k++) {
    unsigned char bk = (unsigned char)s[i + k];
    if ((bk & 0xC0) != 0x80) { i += 1; return kReplacement; }
    cp = (cp << 6) | (bk & 0x3F);
  }
  i += (size_t)extra + 1;
  return cp;
}

}  // namespace utf8
