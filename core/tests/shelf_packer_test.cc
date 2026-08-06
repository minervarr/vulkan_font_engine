// shelf_packer_test — the atlas allocator. Same convention as the other
// core/tests/*.cc: plain assert(), no framework, #undef NDEBUG so the checks
// survive an optimized build, and links NOTHING (shelf_packer.hh is
// header-only and includes only <cstdint>).
//
// Check [4] is the one that matters. It pins the property whose absence made
// the whole UI lose its text: a failed placement must not change the packer.

#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <vector>

#include "../shelf_packer.hh"

using vfe::ShelfPacker;

namespace {

struct Rect { uint32_t page, x, y, w, h; };

bool overlaps(const Rect& a, const Rect& b) {
  if (a.page != b.page) return false;
  return a.x < b.x + b.w && b.x < a.x + a.w &&
         a.y < b.y + b.h && b.y < a.y + a.h;
}

}  // namespace

int main() {
  // ── [1] Cells march along a shelf, then wrap to the next ────────────────
  {
    ShelfPacker p(64, 64, /*pad=*/1);
    ShelfPacker::Slot s{};

    assert(p.place(30, 30, s));
    assert(s.page == 0 && s.x == 0 && s.y == 0);
    assert(p.place(30, 30, s));
    assert(s.page == 0 && s.x == 31 && s.y == 0);   // 30 + 1 pad

    assert(p.place(30, 30, s));                     // no width left: new shelf
    assert(s.page == 0 && s.x == 0 && s.y == 31);
    assert(p.pageCount() == 1);
    std::printf("[1] shelf advance and wrap\n");
  }

  // ── [2] A taller cell grows its shelf; the next shelf starts below it ───
  {
    ShelfPacker p(64, 256, /*pad=*/1);
    ShelfPacker::Slot s{};
    assert(p.place(10, 5, s));    // shelf is 6 tall
    assert(s.y == 0);
    assert(p.place(10, 20, s));   // ... now 21
    assert(s.y == 0);
    assert(p.place(50, 5, s));    // wraps; must clear the TALLER cell
    assert(s.y == 21);
    std::printf("[2] shelf height grows to its tallest cell\n");
  }

  // ── [3] Nothing is ever placed outside its page, and nothing overlaps ───
  //
  // The property the whole allocator exists for. A deterministic spread of
  // sizes, every pair checked.
  {
    ShelfPacker p(256, 128, /*pad=*/1);
    std::vector<Rect> placed;
    uint32_t seed = 12345;
    for (int i = 0; i < 400; ++i) {
      seed = seed * 1103515245u + 12345u;
      const uint32_t w = 1 + (seed >> 16) % 40;
      seed = seed * 1103515245u + 12345u;
      const uint32_t h = 1 + (seed >> 16) % 30;
      ShelfPacker::Slot s{};
      const bool ok = p.place(w, h, s);
      assert(ok);   // unlimited pages: every reasonable cell must be placeable
      assert(s.x + w <= p.pageW());
      assert(s.y + h <= p.pageH());
      placed.push_back(Rect{s.page, s.x, s.y, w, h});
    }
    for (size_t i = 0; i < placed.size(); ++i)
      for (size_t j = i + 1; j < placed.size(); ++j)
        assert(!overlaps(placed[i], placed[j]));
    assert(p.pageCount() > 1);   // 400 cells cannot have fitted in one page
    std::printf("[3] %zu cells, no overlap, all in bounds, %u pages\n",
                placed.size(), p.pageCount());
  }

  // ── [4] REGRESSION: a failed place() must change nothing ────────────────
  //
  // The old packer raised the current shelf's height before testing whether
  // the shelf still fit, and never rolled it back. One failure poisoned every
  // later call, whatever its size — the log read `atlas full ... glyph 9x9
  // does not fit` in a 4096-square sheet and the UI lost all of its text.
  {
    ShelfPacker p(64, 64, /*pad=*/1, /*maxPages=*/1);
    ShelfPacker::Slot s{};
    for (int i = 0; i < 4; ++i) assert(p.place(30, 30, s));   // page is now full

    const uint32_t pagesBefore = p.pageCount();
    const uint32_t usedBefore = p.usedHeight();

    assert(!p.place(30, 30, s));       // no room, and only one page allowed
    assert(p.pageCount() == pagesBefore);
    assert(p.usedHeight() == usedBefore);

    // The packer must still serve anything that genuinely fits.
    assert(p.place(1, 1, s));
    assert(s.page == 0 && s.x + 1 <= 64 && s.y + 1 <= 64);
    std::printf("[4] a failed placement leaves the packer usable\n");
  }

  // ── [5] Too big for any page: refused, not retried forever ─────────────
  //
  // Without this, "no fit -> open a new page -> still no fit -> open a new
  // page" allocates until the process dies.
  {
    ShelfPacker p(64, 64, /*pad=*/1);   // unlimited pages
    ShelfPacker::Slot s{};
    assert(!p.couldFit(64, 10));        // 64 + 1 pad > 64
    assert(!p.couldFit(10, 64));
    assert(!p.place(10, 1000, s));
    assert(p.pageCount() == 0);         // and it opened no pages doing so
    assert(p.place(10, 10, s));         // still healthy
    std::printf("[5] oversized cells refused without opening pages\n");
  }

  // ── [6] Page cap is honoured; unlimited means unlimited ────────────────
  {
    ShelfPacker capped(64, 64, 1, /*maxPages=*/2);
    ShelfPacker::Slot s{};
    int n = 0;
    while (capped.place(30, 30, s)) ++n;
    assert(capped.pageCount() == 2);
    assert(n == 8);   // 4 cells per 64x64 page at 31x31 each

    ShelfPacker open(64, 64, 1, /*maxPages=*/0);
    for (int i = 0; i < 100; ++i) assert(open.place(30, 30, s));
    assert(open.pageCount() == 25);
    std::printf("[6] page cap honoured; 0 means unlimited\n");
  }

  // ── [7] usedHeight tracks the last page's high-water mark ──────────────
  {
    ShelfPacker p(64, 256, 1);
    ShelfPacker::Slot s{};
    assert(p.usedHeight() == 0);
    assert(p.place(10, 20, s));
    assert(p.usedHeight() == 21);
    assert(p.place(10, 5, s));          // same shelf, no growth
    assert(p.usedHeight() == 21);
    std::printf("[7] usedHeight high-water mark\n");
  }

  // ── [8] reset() returns it to a fresh state ────────────────────────────
  {
    ShelfPacker p(64, 64, 1);
    ShelfPacker::Slot a{}, b{};
    assert(p.place(30, 30, a));
    p.reset();
    assert(p.pageCount() == 0);
    assert(p.usedHeight() == 0);
    assert(p.place(30, 30, b));
    assert(a.page == b.page && a.x == b.x && a.y == b.y);
    std::printf("[8] reset\n");
  }

  std::printf("shelf_packer_test: all checks passed\n");
  return 0;
}
