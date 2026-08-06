#include "gpos_kern.hh"

#include <map>
#include <vector>

namespace vfe {
namespace {

// ── Bounds-checked big-endian reader ────────────────────────────────────────
//
// Every offset in an OpenType file is untrusted as far as this parser is
// concerned: a truncated download, a mismatched submodule asset or a genuinely
// malformed face must not walk off the buffer. Reads past the end return 0,
// which makes malformed structures decode as empty rather than as garbage.
struct Reader {
  const uint8_t* p = nullptr;
  size_t size = 0;

  bool has(size_t off, size_t n) const { return off + n >= off && off + n <= size; }
  uint8_t  u8 (size_t o) const { return has(o, 1) ? p[o] : 0; }
  uint16_t u16(size_t o) const {
    return has(o, 2) ? static_cast<uint16_t>((p[o] << 8) | p[o + 1]) : 0;
  }
  int16_t  i16(size_t o) const { return static_cast<int16_t>(u16(o)); }
  uint32_t u32(size_t o) const {
    return has(o, 4) ? (static_cast<uint32_t>(p[o]) << 24 | static_cast<uint32_t>(p[o + 1]) << 16 |
                        static_cast<uint32_t>(p[o + 2]) << 8 | p[o + 3])
                     : 0;
  }
  uint32_t tag(size_t o) const { return u32(o); }
};

constexpr uint32_t kTagCmap = 0x636D6170;  // 'cmap'
constexpr uint32_t kTagGpos = 0x47504F53;  // 'GPOS'
constexpr uint32_t kTagHead = 0x68656164;  // 'head'
constexpr uint32_t kTagKern = 0x6B65726E;  // 'kern' (feature tag)

// Size in bytes of a ValueRecord with this ValueFormat: one int16 per set bit.
int valueRecordSize(uint16_t fmt) {
  int n = 0;
  for (int b = 0; b < 16; ++b)
    if (fmt & (1u << b)) ++n;
  return n * 2;
}

// Byte offset of XAdvance inside a ValueRecord, or -1 if this format has none.
// XAdvance is bit 2 (0x0004); the fields below it are XPlacement (0x0001) and
// YPlacement (0x0002), each int16, and they come first in that order.
int xAdvanceOffset(uint16_t fmt) {
  if (!(fmt & 0x0004)) return -1;
  int off = 0;
  if (fmt & 0x0001) off += 2;
  if (fmt & 0x0002) off += 2;
  return off;
}

// ── Coverage table -> ordered list of glyph ids ─────────────────────────────
// The order matters: PairPos format 1 indexes its PairSet array by coverage
// INDEX, so the Nth glyph here owns the Nth PairSet.
void readCoverage(const Reader& r, size_t off, std::vector<uint16_t>& out) {
  const uint16_t fmt = r.u16(off);
  if (fmt == 1) {
    const uint16_t n = r.u16(off + 2);
    out.reserve(out.size() + n);
    for (uint16_t i = 0; i < n; ++i) out.push_back(r.u16(off + 4 + 2 * i));
  } else if (fmt == 2) {
    const uint16_t n = r.u16(off + 2);
    for (uint16_t i = 0; i < n; ++i) {
      const size_t rec = off + 4 + 6 * i;
      const uint16_t start = r.u16(rec), end = r.u16(rec + 2);
      // startCoverageIndex (rec+4) is implied by append order; a font whose
      // ranges disagree with it is malformed and we prefer append order.
      if (end < start) continue;
      for (uint32_t g = start; g <= end; ++g) out.push_back(static_cast<uint16_t>(g));
    }
  }
}

// ── ClassDef table -> gid -> class ──────────────────────────────────────────
void readClassDef(const Reader& r, size_t off, std::map<uint16_t, uint16_t>& out) {
  const uint16_t fmt = r.u16(off);
  if (fmt == 1) {
    const uint16_t start = r.u16(off + 2), n = r.u16(off + 4);
    for (uint16_t i = 0; i < n; ++i) {
      const uint16_t cls = r.u16(off + 6 + 2 * i);
      if (cls) out[static_cast<uint16_t>(start + i)] = cls;
    }
  } else if (fmt == 2) {
    const uint16_t n = r.u16(off + 2);
    for (uint16_t i = 0; i < n; ++i) {
      const size_t rec = off + 4 + 6 * i;
      const uint16_t start = r.u16(rec), end = r.u16(rec + 2), cls = r.u16(rec + 4);
      if (!cls || end < start) continue;
      for (uint32_t g = start; g <= end; ++g) out[static_cast<uint16_t>(g)] = cls;
    }
  }
  // Glyphs absent from a ClassDef are class 0 by definition — left out of the
  // map and handled by the caller's find() miss.
}

// ── cmap -> codepoint -> gid ────────────────────────────────────────────────
void readCmapFormat4(const Reader& r, size_t off, std::map<uint32_t, uint16_t>& out) {
  const uint16_t segX2 = r.u16(off + 6);
  const uint16_t segs = segX2 / 2;
  const size_t endO = off + 14;
  const size_t startO = endO + segX2 + 2;
  const size_t deltaO = startO + segX2;
  const size_t rangeO = deltaO + segX2;
  for (uint16_t s = 0; s < segs; ++s) {
    const uint16_t end = r.u16(endO + 2 * s);
    const uint16_t start = r.u16(startO + 2 * s);
    const int16_t delta = r.i16(deltaO + 2 * s);
    const uint16_t rangeOff = r.u16(rangeO + 2 * s);
    if (start > end) continue;
    for (uint32_t c = start; c <= end; ++c) {
      if (c == 0xFFFF) continue;
      uint16_t gid = 0;
      if (rangeOff == 0) {
        gid = static_cast<uint16_t>((c + delta) & 0xFFFF);
      } else {
        const size_t gp = rangeO + 2 * s + rangeOff + 2 * (c - start);
        gid = r.u16(gp);
        if (gid) gid = static_cast<uint16_t>((gid + delta) & 0xFFFF);
      }
      if (gid) out.emplace(c, gid);
    }
  }
}

void readCmapFormat12(const Reader& r, size_t off, std::map<uint32_t, uint16_t>& out) {
  const uint32_t n = r.u32(off + 12);
  // Guard against a bogus group count claiming more data than the file holds.
  const uint32_t maxGroups = static_cast<uint32_t>((r.size - off) / 12);
  const uint32_t groups = n < maxGroups ? n : maxGroups;
  for (uint32_t i = 0; i < groups; ++i) {
    const size_t rec = off + 16 + 12 * i;
    const uint32_t start = r.u32(rec), end = r.u32(rec + 4), startGid = r.u32(rec + 8);
    if (end < start || end - start > 0x10FFFF) continue;
    for (uint32_t c = start; c <= end; ++c) {
      const uint32_t gid = startGid + (c - start);
      if (gid && gid <= 0xFFFF) out.emplace(c, static_cast<uint16_t>(gid));
    }
  }
}

void readCmap(const Reader& r, size_t cmapOff, std::map<uint32_t, uint16_t>& out) {
  const uint16_t n = r.u16(cmapOff + 2);
  size_t best = 0;
  int bestScore = -1;
  for (uint16_t i = 0; i < n; ++i) {
    const size_t rec = cmapOff + 4 + 8 * i;
    const uint16_t plat = r.u16(rec), enc = r.u16(rec + 2);
    const uint32_t sub = cmapOff + r.u32(rec + 4);
    const uint16_t fmt = r.u16(sub);
    if (fmt != 4 && fmt != 12) continue;
    // Prefer full-Unicode (format 12) over BMP-only, and Windows over the
    // Unicode platform, matching what every shaping engine picks.
    int score = 0;
    if (fmt == 12) score += 4;
    if (plat == 3 && (enc == 1 || enc == 10)) score += 2;
    else if (plat == 0) score += 1;
    if (score > bestScore) { bestScore = score; best = sub; }
  }
  if (bestScore < 0) return;
  const uint16_t fmt = r.u16(best);
  if (fmt == 4) readCmapFormat4(r, best, out);
  else if (fmt == 12) readCmapFormat12(r, best, out);
}

// ── One PairPos subtable ────────────────────────────────────────────────────
void parsePairPos(const Reader& r, size_t st, const std::map<uint16_t, uint32_t>& gidToCp,
                  float upem, KernTable& out) {
  const uint16_t posFormat = r.u16(st);
  const uint16_t vf1 = r.u16(st + 4);
  const uint16_t vf2 = r.u16(st + 6);
  const int xAdvOff = xAdvanceOffset(vf1);
  if (xAdvOff < 0) return;  // this subtable adjusts something other than advance
  const int sz1 = valueRecordSize(vf1), sz2 = valueRecordSize(vf2);

  auto emit = [&](uint16_t gidA, uint16_t gidB, int16_t raw) {
    if (!raw) return;
    const auto a = gidToCp.find(gidA);
    if (a == gidToCp.end()) return;
    const auto b = gidToCp.find(gidB);
    if (b == gidToCp.end()) return;
    out[kernKey(a->second, b->second)] = static_cast<float>(raw) / upem;
  };

  if (posFormat == 1) {
    std::vector<uint16_t> cov;
    readCoverage(r, st + r.u16(st + 2), cov);
    const uint16_t pairSetCount = r.u16(st + 8);
    const size_t n = pairSetCount < cov.size() ? pairSetCount : cov.size();
    for (size_t i = 0; i < n; ++i) {
      const size_t ps = st + r.u16(st + 10 + 2 * i);
      const uint16_t pvCount = r.u16(ps);
      for (uint16_t k = 0; k < pvCount; ++k) {
        const size_t rec = ps + 2 + static_cast<size_t>(k) * (2 + sz1 + sz2);
        if (!r.has(rec, static_cast<size_t>(2 + sz1 + sz2))) break;
        emit(cov[i], r.u16(rec), r.i16(rec + 2 + xAdvOff));
      }
    }
  } else if (posFormat == 2) {
    std::vector<uint16_t> cov;
    readCoverage(r, st + r.u16(st + 2), cov);
    std::map<uint16_t, uint16_t> cd1, cd2;
    readClassDef(r, st + r.u16(st + 8), cd1);
    readClassDef(r, st + r.u16(st + 10), cd2);
    const uint16_t c1n = r.u16(st + 12), c2n = r.u16(st + 14);
    if (!c1n || !c2n) return;
    const size_t recSize = static_cast<size_t>(sz1 + sz2);
    const size_t base = st + 16;

    // Class 2 membership has to be walked per first-glyph, so invert once:
    // class -> the glyphs in it that actually have a codepoint.
    std::map<uint16_t, std::vector<uint16_t>> byClass2;
    for (const auto& kv : gidToCp) {
      const auto it = cd2.find(kv.first);
      byClass2[it == cd2.end() ? 0 : it->second].push_back(kv.first);
    }

    for (uint16_t gidA : cov) {
      if (gidToCp.find(gidA) == gidToCp.end()) continue;
      const auto it1 = cd1.find(gidA);
      const uint16_t c1 = it1 == cd1.end() ? 0 : it1->second;
      if (c1 >= c1n) continue;
      for (uint16_t c2 = 0; c2 < c2n; ++c2) {
        const size_t rec = base + (static_cast<size_t>(c1) * c2n + c2) * recSize;
        if (!r.has(rec, recSize)) continue;
        const int16_t raw = r.i16(rec + xAdvOff);
        if (!raw) continue;
        const auto members = byClass2.find(c2);
        if (members == byClass2.end()) continue;
        for (uint16_t gidB : members->second) emit(gidA, gidB, raw);
      }
    }
  }
}

}  // namespace

bool parseGposKernPairs(const uint8_t* data, size_t size, KernTable& out) {
  if (!data || size < 12) return false;
  Reader r{data, size};

  const uint16_t numTables = r.u16(4);
  size_t cmapOff = 0, gposOff = 0, headOff = 0;
  for (uint16_t i = 0; i < numTables; ++i) {
    const size_t rec = 12 + 16 * static_cast<size_t>(i);
    if (!r.has(rec, 16)) break;
    const uint32_t tag = r.tag(rec);
    const uint32_t off = r.u32(rec + 8);
    if (off >= size) continue;
    if (tag == kTagCmap) cmapOff = off;
    else if (tag == kTagGpos) gposOff = off;
    else if (tag == kTagHead) headOff = off;
  }
  if (!cmapOff || !gposOff || !headOff) return false;

  const float upem = static_cast<float>(r.u16(headOff + 18));
  if (upem <= 0.0f) return false;

  std::map<uint32_t, uint16_t> cpToGid;
  readCmap(r, cmapOff, cpToGid);
  if (cpToGid.empty()) return false;

  // gid -> codepoint. std::map iterates ascending, and emplace keeps the first
  // insert, so a gid reachable from several codepoints deterministically takes
  // the lowest — the table must not depend on hash order.
  std::map<uint16_t, uint32_t> gidToCp;
  for (const auto& kv : cpToGid) gidToCp.emplace(kv.second, kv.first);

  // FeatureList: collect every lookup index referenced by a 'kern' feature.
  // Script/language filtering is deliberately skipped — this engine lays out
  // one shaping-free run, and every 'kern' feature record in a text face
  // carries the same pair lookups.
  const size_t featureList = gposOff + r.u16(gposOff + 6);
  const size_t lookupList = gposOff + r.u16(gposOff + 8);
  const uint16_t featureCount = r.u16(featureList);
  std::vector<uint16_t> kernLookups;
  for (uint16_t i = 0; i < featureCount; ++i) {
    const size_t rec = featureList + 2 + 6 * static_cast<size_t>(i);
    if (r.tag(rec) != kTagKern) continue;
    const size_t feat = featureList + r.u16(rec + 4);
    const uint16_t n = r.u16(feat + 2);
    for (uint16_t j = 0; j < n; ++j) kernLookups.push_back(r.u16(feat + 4 + 2 * j));
  }
  if (kernLookups.empty()) return false;

  const uint16_t lookupCount = r.u16(lookupList);
  const size_t before = out.size();
  for (uint16_t li : kernLookups) {
    if (li >= lookupCount) continue;
    const size_t lo = lookupList + r.u16(lookupList + 2 + 2 * static_cast<size_t>(li));
    const uint16_t type = r.u16(lo);
    const uint16_t subCount = r.u16(lo + 4);
    for (uint16_t s = 0; s < subCount; ++s) {
      size_t st = lo + r.u16(lo + 6 + 2 * static_cast<size_t>(s));
      uint16_t effType = type;
      // LookupType 9 = Extension Positioning: a 32-bit indirection to a real
      // subtable, used when the lookup data sits past the 16-bit offset reach.
      if (type == 9) {
        effType = r.u16(st + 2);
        st = st + r.u32(st + 4);
      }
      if (effType == 2) parsePairPos(r, st, gidToCp, upem, out);
    }
  }
  return out.size() > before;
}

}  // namespace vfe
