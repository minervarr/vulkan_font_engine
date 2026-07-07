// math_table.cc — see math_table.hh. Hand-rolled OpenType MATH table reader.
//
// Copyright (C) 2026 nava. Licensed under the GNU AGPLv3 or later; see LICENSE.
//
// Layout references: Microsoft OpenType spec, "MATH — The Mathematical Typesetting
// Table". Offsets are 16-bit and relative to the start of the table that contains
// them; we resolve them to absolute positions in the byte buffer as we descend.
#include "math_table.hh"

namespace mathtbl {

namespace {
// A MathValueRecord is { int16 value; Offset16 deviceTable } == 4 bytes; we only
// want the value. Records are read sequentially, so this also advances the cursor.
constexpr uint32_t kValueRecord = 4;
}  // namespace

uint16_t MathTable::u16(uint32_t off) const {
  if (off + 2 > b_.size()) return 0;
  return static_cast<uint16_t>((b_[off] << 8) | b_[off + 1]);
}

MathTable::MathTable(std::vector<uint8_t> tableBytes) : b_(std::move(tableBytes)) {
  // MATH header: majorVersion, minorVersion, then three Offset16 (from MATH start).
  if (b_.size() < 10) return;
  uint16_t major = u16(0);
  if (major != 1) return;  // only version 1 exists
  uint32_t constantsOff = u16(4);
  uint32_t glyphInfoOff = u16(6);
  mathVariantsOff_ = u16(8);

  if (constantsOff) parseConstants(constantsOff);

  if (mathVariantsOff_) {
    minConnectorOverlap_ = u16(mathVariantsOff_ + 0);
  }

  // MathGlyphInfo → MathItalicsCorrectionInfo (first Offset16 in MathGlyphInfo).
  if (glyphInfoOff) {
    uint32_t icInfo = u16(glyphInfoOff + 0);
    if (icInfo) italicsInfoOff_ = glyphInfoOff + icInfo;
  }

  ok_ = true;
}

void MathTable::parseConstants(uint32_t off) {
  Constants& c = constants_;
  // Two int16 percentages, then two UFWORDs, then 51 MathValueRecords, then a
  // trailing int16 percentage — read strictly in spec order.
  uint32_t p = off;
  auto pct = [&]() -> int16_t { int16_t v = i16(p); p += 2; return v; };
  auto ufword = [&]() -> uint16_t { uint16_t v = u16(p); p += 2; return v; };
  auto val = [&]() -> int16_t { int16_t v = i16(p); p += kValueRecord; return v; };

  c.scriptPercentScaleDown = pct();
  c.scriptScriptPercentScaleDown = pct();
  c.delimitedSubFormulaMinHeight = ufword();
  c.displayOperatorMinHeight = ufword();
  c.mathLeading = val();
  c.axisHeight = val();
  c.accentBaseHeight = val();
  c.flattenedAccentBaseHeight = val();
  c.subscriptShiftDown = val();
  c.subscriptTopMax = val();
  c.subscriptBaselineDropMin = val();
  c.superscriptShiftUp = val();
  c.superscriptShiftUpCramped = val();
  c.superscriptBottomMin = val();
  c.superscriptBaselineDropMax = val();
  c.subSuperscriptGapMin = val();
  c.superscriptBottomMaxWithSubscript = val();
  c.spaceAfterScript = val();
  c.upperLimitGapMin = val();
  c.upperLimitBaselineRiseMin = val();
  c.lowerLimitGapMin = val();
  c.lowerLimitBaselineDropMin = val();
  c.stackTopShiftUp = val();
  c.stackTopDisplayStyleShiftUp = val();
  c.stackBottomShiftDown = val();
  c.stackBottomDisplayStyleShiftDown = val();
  c.stackGapMin = val();
  c.stackDisplayStyleGapMin = val();
  c.stretchStackTopShiftUp = val();
  c.stretchStackBottomShiftDown = val();
  c.stretchStackGapAboveMin = val();
  c.stretchStackGapBelowMin = val();
  c.fractionNumeratorShiftUp = val();
  c.fractionNumeratorDisplayStyleShiftUp = val();
  c.fractionDenominatorShiftDown = val();
  c.fractionDenominatorDisplayStyleShiftDown = val();
  c.fractionNumeratorGapMin = val();
  c.fractionNumDisplayStyleGapMin = val();
  c.fractionRuleThickness = val();
  c.fractionDenominatorGapMin = val();
  c.fractionDenomDisplayStyleGapMin = val();
  c.skewedFractionHorizontalGap = val();
  c.skewedFractionVerticalGap = val();
  c.overbarVerticalGap = val();
  c.overbarRuleThickness = val();
  c.overbarExtraAscender = val();
  c.underbarVerticalGap = val();
  c.underbarRuleThickness = val();
  c.underbarExtraDescender = val();
  c.radicalVerticalGap = val();
  c.radicalDisplayStyleVerticalGap = val();
  c.radicalRuleThickness = val();
  c.radicalExtraAscender = val();
  c.radicalKernBeforeDegree = val();
  c.radicalKernAfterDegree = val();
  c.radicalDegreeBottomRaisePercent = pct();
  c.present = true;
}

int MathTable::coverageIndex(uint32_t covOff, uint16_t gid) const {
  if (!covOff || covOff + 4 > b_.size()) return -1;
  uint16_t format = u16(covOff);
  if (format == 1) {
    uint16_t count = u16(covOff + 2);
    uint32_t arr = covOff + 4;
    for (uint16_t i = 0; i < count; i++) {
      if (u16(arr + i * 2) == gid) return i;
    }
    return -1;
  }
  if (format == 2) {
    uint16_t rangeCount = u16(covOff + 2);
    uint32_t r = covOff + 4;
    for (uint16_t i = 0; i < rangeCount; i++, r += 6) {
      uint16_t start = u16(r);
      uint16_t end = u16(r + 2);
      uint16_t startCoverageIndex = u16(r + 4);
      if (gid >= start && gid <= end) return startCoverageIndex + (gid - start);
    }
    return -1;
  }
  return -1;
}

std::optional<VerticalConstruction> MathTable::verticalConstruction(uint16_t baseGid) const {
  if (!mathVariantsOff_) return std::nullopt;
  const uint32_t mv = mathVariantsOff_;
  // MathVariants: minConnectorOverlap, vertCoverage, horizCoverage, vertCount,
  // horizCount, then Offset16 vertConstruction[vertCount] (MathVariants-relative).
  uint32_t vertCovOff = u16(mv + 2);
  uint16_t vertCount = u16(mv + 6);
  if (!vertCovOff) return std::nullopt;

  int idx = coverageIndex(mv + vertCovOff, baseGid);
  if (idx < 0 || idx >= static_cast<int>(vertCount)) return std::nullopt;

  uint32_t constrTableOff = u16(mv + 10 + idx * 2);
  if (!constrTableOff) return std::nullopt;
  uint32_t gc = mv + constrTableOff;  // MathGlyphConstruction (MathVariants-relative)

  VerticalConstruction out;
  // MathGlyphConstruction: Offset16 glyphAssembly, uint16 variantCount, then
  // variantCount MathGlyphVariantRecord { uint16 variantGlyph; UFWORD advance }.
  uint16_t assemblyOff = u16(gc + 0);
  uint16_t variantCount = u16(gc + 2);
  uint32_t vp = gc + 4;
  out.variants.reserve(variantCount);
  for (uint16_t i = 0; i < variantCount; i++, vp += 4) {
    Variant v;
    v.glyph = u16(vp);
    v.advanceMeasurement = u16(vp + 2);
    out.variants.push_back(v);
  }

  if (assemblyOff) {
    uint32_t ga = gc + assemblyOff;  // GlyphAssembly (MathGlyphConstruction-relative)
    out.hasAssembly = true;
    out.italicsCorrection = i16(ga + 0);  // MathValueRecord.value
    uint16_t partCount = u16(ga + kValueRecord);
    uint32_t pp = ga + kValueRecord + 2;
    out.parts.reserve(partCount);
    for (uint16_t i = 0; i < partCount; i++, pp += 10) {
      Part part;
      part.glyph = u16(pp + 0);
      part.startConnectorLength = u16(pp + 2);
      part.endConnectorLength = u16(pp + 4);
      part.fullAdvance = u16(pp + 6);
      part.extender = (u16(pp + 8) & 0x0001) != 0;
      out.parts.push_back(part);
    }
  }
  return out;
}

std::optional<int16_t> MathTable::italicCorrection(uint16_t gid) const {
  if (!italicsInfoOff_) return std::nullopt;
  // MathItalicsCorrectionInfo: Offset16 coverage (self-relative), uint16 count,
  // then count MathValueRecords.
  uint32_t info = italicsInfoOff_;
  uint32_t covOff = u16(info + 0);
  uint16_t count = u16(info + 2);
  if (!covOff) return std::nullopt;
  int idx = coverageIndex(info + covOff, gid);
  if (idx < 0 || idx >= static_cast<int>(count)) return std::nullopt;
  return i16(info + 4 + idx * kValueRecord);
}

}  // namespace mathtbl
