// math_table.hh — a tiny, dependency-free reader for the OpenType MATH table.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
//
// FreeType (which msdfgen-ext uses for outlines) does NOT surface the MATH table
// through a typed API, and we deliberately avoid pulling in HarfBuzz — so this
// parses the raw `MATH` table bytes by hand with std:: only. It replaces the Rust
// `ttf_parser::math` path: MathConstants, the vertical stretch constructions
// (variants + assembly recipes), and per-glyph italic correction.
//
// All measurements are returned in FONT DESIGN UNITS (the caller divides by the
// font's units-per-em); percentages are returned as raw int16 (caller / 100).
// Big-endian throughout, per the OpenType spec.
#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

namespace mathtbl {

// The full 56-field MathConstants table, in spec order, kept in design units
// (the two *PercentScaleDown and radicalDegreeBottomRaisePercent are raw int16
// percentages). Only the subset the writer exports is consumed downstream, but
// we read the whole table so the sequential layout stays correct.
struct Constants {
  bool present = false;
  int16_t scriptPercentScaleDown = 0;
  int16_t scriptScriptPercentScaleDown = 0;
  uint16_t delimitedSubFormulaMinHeight = 0;
  uint16_t displayOperatorMinHeight = 0;
  int16_t mathLeading = 0;
  int16_t axisHeight = 0;
  int16_t accentBaseHeight = 0;
  int16_t flattenedAccentBaseHeight = 0;
  int16_t subscriptShiftDown = 0;
  int16_t subscriptTopMax = 0;
  int16_t subscriptBaselineDropMin = 0;
  int16_t superscriptShiftUp = 0;
  int16_t superscriptShiftUpCramped = 0;
  int16_t superscriptBottomMin = 0;
  int16_t superscriptBaselineDropMax = 0;
  int16_t subSuperscriptGapMin = 0;
  int16_t superscriptBottomMaxWithSubscript = 0;
  int16_t spaceAfterScript = 0;
  int16_t upperLimitGapMin = 0;
  int16_t upperLimitBaselineRiseMin = 0;
  int16_t lowerLimitGapMin = 0;
  int16_t lowerLimitBaselineDropMin = 0;
  int16_t stackTopShiftUp = 0;
  int16_t stackTopDisplayStyleShiftUp = 0;
  int16_t stackBottomShiftDown = 0;
  int16_t stackBottomDisplayStyleShiftDown = 0;
  int16_t stackGapMin = 0;
  int16_t stackDisplayStyleGapMin = 0;
  int16_t stretchStackTopShiftUp = 0;
  int16_t stretchStackBottomShiftDown = 0;
  int16_t stretchStackGapAboveMin = 0;
  int16_t stretchStackGapBelowMin = 0;
  int16_t fractionNumeratorShiftUp = 0;
  int16_t fractionNumeratorDisplayStyleShiftUp = 0;
  int16_t fractionDenominatorShiftDown = 0;
  int16_t fractionDenominatorDisplayStyleShiftDown = 0;
  int16_t fractionNumeratorGapMin = 0;
  int16_t fractionNumDisplayStyleGapMin = 0;
  int16_t fractionRuleThickness = 0;
  int16_t fractionDenominatorGapMin = 0;
  int16_t fractionDenomDisplayStyleGapMin = 0;
  int16_t skewedFractionHorizontalGap = 0;
  int16_t skewedFractionVerticalGap = 0;
  int16_t overbarVerticalGap = 0;
  int16_t overbarRuleThickness = 0;
  int16_t overbarExtraAscender = 0;
  int16_t underbarVerticalGap = 0;
  int16_t underbarRuleThickness = 0;
  int16_t underbarExtraDescender = 0;
  int16_t radicalVerticalGap = 0;
  int16_t radicalDisplayStyleVerticalGap = 0;
  int16_t radicalRuleThickness = 0;
  int16_t radicalExtraAscender = 0;
  int16_t radicalKernBeforeDegree = 0;
  int16_t radicalKernAfterDegree = 0;
  int16_t radicalDegreeBottomRaisePercent = 0;
};

// One pre-built bigger variant of a growable glyph.
struct Variant {
  uint16_t glyph = 0;            // glyph id
  uint16_t advanceMeasurement = 0;  // design units (the variant's stretch axis size)
};

// One part of a GlyphAssembly recipe (top hook / extender / bottom, etc.).
struct Part {
  uint16_t glyph = 0;
  uint16_t startConnectorLength = 0;  // design units
  uint16_t endConnectorLength = 0;    // design units
  uint16_t fullAdvance = 0;           // design units
  bool extender = false;              // partFlags bit 0x0001
};

// A glyph's vertical stretch construction (variants come first, then an optional
// assembly recipe). `hasAssembly` distinguishes "no assembly" from "empty parts".
struct VerticalConstruction {
  std::vector<Variant> variants;
  bool hasAssembly = false;
  int16_t italicsCorrection = 0;  // design units (assembly italic correction)
  std::vector<Part> parts;
};

// Parsed MATH table. Construct from the raw `MATH` table bytes (e.g. read via
// FreeType's FT_Load_Sfnt_Table). `ok()` is false if the bytes aren't a valid
// MATH table; callers should treat that as "no MATH" (matches Rust's Option).
class MathTable {
 public:
  explicit MathTable(std::vector<uint8_t> tableBytes);

  bool ok() const { return ok_; }
  const Constants& constants() const { return constants_; }
  uint16_t minConnectorOverlap() const { return minConnectorOverlap_; }

  // Vertical construction for a base glyph id, if it is growable in this font.
  std::optional<VerticalConstruction> verticalConstruction(uint16_t baseGid) const;

  // Per-glyph italic correction (design units), if present for this glyph id.
  std::optional<int16_t> italicCorrection(uint16_t gid) const;

 private:
  std::vector<uint8_t> b_;
  bool ok_ = false;

  Constants constants_;
  uint16_t minConnectorOverlap_ = 0;

  // MATH-relative offsets captured during the header walk (0 == absent).
  uint32_t mathVariantsOff_ = 0;
  uint32_t italicsInfoOff_ = 0;  // MathItalicsCorrectionInfo, absolute in b_

  // Big-endian readers (bounds-checked; out-of-range reads return 0 / false).
  uint16_t u16(uint32_t off) const;
  int16_t i16(uint32_t off) const { return static_cast<int16_t>(u16(off)); }

  // Coverage-table lookup: index of `gid` within the coverage at absolute offset
  // `covOff`, or -1 if not covered.
  int coverageIndex(uint32_t covOff, uint16_t gid) const;

  void parseConstants(uint32_t off);
};

}  // namespace mathtbl
