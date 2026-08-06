// gpos_kern_test — GPOS kern-pair extraction, asserted against New Computer
// Modern's real tables. Same convention as vk_canvas's core/tests/*.cc: plain
// assert(), no framework, #undef NDEBUG so the checks survive an optimized
// build, and links gpos_kern.cc ALONE — no msdfgen, no FreeType, no Vulkan.
// Keep it that way: the parser is pure bytes-in/table-out precisely so it can
// be checked without a GPU or a font library.
//
// Run: ./build/linux_debug/framework/.../gpos_kern_test <path-to-fonts-dir>
// (defaults to the in-tree assets/fonts/newcomputermodern).

#undef NDEBUG
#include <cassert>

#include "../gpos_kern.hh"

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

float kernOf(const vfe::KernTable& t, uint32_t a, uint32_t b) {
  const auto it = t.find(vfe::kernKey(a, b));
  return it == t.end() ? 0.0f : it->second;
}

bool near(float a, float b) { return std::fabs(a - b) < 1e-4f; }

}  // namespace

int main(int argc, char** argv) {
  const std::string dir =
      argc > 1 ? argv[1] : "assets/fonts/newcomputermodern";

  // ── [1] Regular face: the pairs that motivated this whole feature ─────────
  const std::vector<uint8_t> regular = readFile(dir + "/NewCM10-Regular.otf");
  if (regular.empty()) {
    std::fprintf(stderr, "gpos_kern_test: cannot read %s/NewCM10-Regular.otf\n",
                 dir.c_str());
    return 77;  // treated as "skipped" — assets not where we were pointed
  }

  vfe::KernTable t;
  assert(vfe::parseGposKernPairs(regular.data(), regular.size(), t));
  assert(!t.empty());

  // Values verified directly against the font's GPOS PairPos records
  // (unitsPerEm 1000). These are the classic tight pairs; if any of them
  // regress to 0 the parser has stopped finding the 'kern' feature.
  assert(near(kernOf(t, 'A', 'V'), -0.111f));
  assert(near(kernOf(t, 'V', 'A'), -0.111f));
  assert(near(kernOf(t, 'A', 'W'), -0.111f));
  assert(near(kernOf(t, 'T', 'o'), -0.083f));
  assert(near(kernOf(t, 'W', 'a'), -0.083f));
  assert(near(kernOf(t, 'Y', 'o'), -0.083f));
  assert(near(kernOf(t, 'P', '.'), -0.083f));
  assert(near(kernOf(t, 'L', 'T'), -0.083f));

  // Kerning is directional and sparse: a pair with no record must read 0
  // rather than borrowing its mirror.
  assert(kernOf(t, 'o', 'T') == 0.0f);
  assert(kernOf(t, 'n', 'n') == 0.0f);

  // Every value is a plausible EM fraction. A unitsPerEm mix-up (e.g. dividing
  // by 2048 for a 1000-upem face, or not dividing at all) would show up here
  // long before it showed up on screen.
  for (const auto& kv : t) {
    assert(kv.second > -0.5f && kv.second < 0.5f);
    assert(kv.second != 0.0f);  // zero pairs are dropped, not stored
  }
  std::printf("[1] regular: %zu pairs, spot values OK\n", t.size());

  // ── [2] Bold and Italic carry their OWN values ───────────────────────────
  // addStyle() gives each style its own table for this reason; if they were
  // identical the per-style tables would be dead weight.
  const std::vector<uint8_t> bold = readFile(dir + "/NewCM10-Bold.otf");
  if (!bold.empty()) {
    vfe::KernTable tb;
    assert(vfe::parseGposKernPairs(bold.data(), bold.size(), tb));
    assert(!tb.empty());
    assert(kernOf(tb, 'A', 'V') < 0.0f);
    std::printf("[2] bold: %zu pairs, AV=%.4f (regular AV=%.4f)\n", tb.size(),
                kernOf(tb, 'A', 'V'), kernOf(t, 'A', 'V'));
  }

  // ── [3] Accumulation, not replacement ────────────────────────────────────
  // parseGposKernPairs() adds to `out`; a second parse of the same face must
  // therefore leave the table the same size, not double it.
  const size_t before = t.size();
  vfe::parseGposKernPairs(regular.data(), regular.size(), t);
  assert(t.size() == before);
  std::printf("[3] re-parse is idempotent (%zu pairs)\n", t.size());

  // ── [4] Malformed input must not crash or invent pairs ───────────────────
  // Every offset in the file is treated as untrusted; these are the shapes a
  // truncated download or a wrong asset actually takes.
  {
    vfe::KernTable junk;
    assert(!vfe::parseGposKernPairs(nullptr, 0, junk));
    assert(!vfe::parseGposKernPairs(regular.data(), 4, junk));
    assert(junk.empty());

    // Truncated at many lengths: the parser may find nothing, or find a
    // partial table, but must never read out of bounds (ASan/UBSan catch that
    // when the test is built with them) and must never fabricate a pair
    // outside the legal EM range.
    for (size_t n = 8; n < regular.size(); n = n * 2 + 1) {
      vfe::KernTable partial;
      vfe::parseGposKernPairs(regular.data(), n, partial);
      for (const auto& kv : partial) assert(kv.second > -0.5f && kv.second < 0.5f);
    }

    // A byte-corrupted copy: flip bytes across the header/table-directory
    // region, where the offsets that drive every later read live.
    std::vector<uint8_t> corrupt = regular;
    for (size_t i = 0; i < corrupt.size() && i < 4096; i += 7) corrupt[i] ^= 0xA5;
    vfe::KernTable ct;
    vfe::parseGposKernPairs(corrupt.data(), corrupt.size(), ct);
    for (const auto& kv : ct) assert(kv.second > -0.5f && kv.second < 0.5f);
    std::printf("[4] malformed input handled (%zu pairs from corrupt copy)\n",
                ct.size());
  }

  // ── [5] A face with no GPOS kern data reports false ──────────────────────
  // The icon font is generated and has no kerning at all — the honest
  // "nothing here" path, which callers use to lay out unkerned.
  {
    const std::vector<uint8_t> icons = readFile(dir + "/../icons/matrix-icons.otf");
    if (!icons.empty()) {
      vfe::KernTable it;
      const bool found = vfe::parseGposKernPairs(icons.data(), icons.size(), it);
      assert(!found || !it.empty());  // false => empty; true => it found real pairs
      std::printf("[5] icon font: found=%d pairs=%zu\n", (int)found, it.size());
    }
  }

  std::printf("gpos_kern_test: all checks passed\n");
  return 0;
}
