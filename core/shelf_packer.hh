#pragma once
#include <cstdint>

// ── Shelf packing over fixed-size pages ─────────────────────────────────────
//
// Cells are placed left to right along a shelf; when the shelf runs out of
// width a new one opens below it, as tall as its first cell needs and growing
// if a later cell on the same shelf is taller. When a page runs out of height
// a new page opens. That is the whole algorithm — it is cheap, it never moves
// a cell once placed, and for a batch sorted by descending height it wastes
// almost nothing.
//
// ONE PROPERTY MATTERS MORE THAN THE ALGORITHM: place() either succeeds and
// commits, or fails and changes NOTHING.
//
// The version this replaces did not have it. It raised the current shelf's
// height before testing whether the shelf still fit in the page:
//
//     if (needH > shelfH_) shelfH_ = needH;          // committed early
//     while (shelfY_ + shelfH_ > atlasH_) { ... return false; }
//
// so a single failure left shelfH_ permanently set to the height of the cell
// that did not fit. Every later call then failed the same test, whatever its
// size. In the logs this reads as `atlas full ... glyph 9x9 does not fit`
// inside a 4096-square sheet, and on screen as the entire UI losing its text
// after one oversized glyph. A packer that mutates before it validates cannot
// be recovered from; one that does not, cannot fail that way at all.
//
// Pure: <cstdint> and nothing else, so shelf_packer_test links no library.

namespace vfe {

class ShelfPacker {
 public:
  struct Slot {
    uint32_t page = 0;
    uint32_t x = 0;
    uint32_t y = 0;
  };

  // `pad` is reserved to the right of and below every cell, so a linear
  // sampler landing a hair off the texel grid reads transparency rather than
  // the neighbouring glyph. `maxPages == 0` means unlimited.
  constexpr ShelfPacker(uint32_t pageW, uint32_t pageH, uint32_t pad,
                        uint32_t maxPages = 0)
      : pageW_(pageW), pageH_(pageH), pad_(pad), maxPages_(maxPages) {}

  // True if a cell this size could EVER be placed, in an empty page. Separate
  // from place() because "too big for any page" is a different problem from
  // "no room left", and only the first is worth reporting to a human.
  constexpr bool couldFit(uint32_t w, uint32_t h) const {
    return w + pad_ <= pageW_ && h + pad_ <= pageH_;
  }

  // Reserve a w x h cell. On false, no member has changed.
  [[nodiscard]] constexpr bool place(uint32_t w, uint32_t h, Slot& out) {
    if (!couldFit(w, h)) return false;

    const uint32_t needW = w + pad_;
    const uint32_t needH = h + pad_;

    // Everything below works on locals. Nothing touches a member until the
    // very end, once the placement is known to be valid.
    uint32_t page = page_;
    uint32_t x = shelfX_;
    uint32_t y = shelfY_;
    uint32_t shelfH = shelfH_;

    if (x + needW > pageW_) {   // close this shelf, open the next
      y += shelfH;
      x = 0;
      shelfH = 0;
    }
    if (needH > shelfH) shelfH = needH;   // a taller cell grows its shelf

    if (y + shelfH > pageH_) {            // the page is out of height
      if (maxPages_ != 0 && page + 1 >= maxPages_) return false;
      page += 1;
      x = 0;
      y = 0;
      shelfH = needH;
      // couldFit() already proved needH <= pageH_ and needW <= pageW_, so a
      // fresh page always accepts this cell. That is what stops "open a page,
      // still does not fit, open a page" from running until it exhausts
      // memory.
    }

    out.page = page;
    out.x = x;
    out.y = y;

    page_ = page;
    shelfX_ = x + needW;
    shelfY_ = y;
    shelfH_ = shelfH;
    used_ = true;
    return true;
  }

  // Pages in use. Zero until the first successful place().
  constexpr uint32_t pageCount() const { return used_ ? page_ + 1 : 0; }

  // Rows used so far in the LAST page — the high-water mark a single-page
  // consumer uploads, rather than the whole page.
  constexpr uint32_t usedHeight() const { return used_ ? shelfY_ + shelfH_ : 0; }

  constexpr uint32_t pageW() const { return pageW_; }
  constexpr uint32_t pageH() const { return pageH_; }
  constexpr uint32_t pad() const { return pad_; }

  constexpr void reset() {
    page_ = 0;
    shelfX_ = 0;
    shelfY_ = 0;
    shelfH_ = 0;
    used_ = false;
  }

 private:
  uint32_t pageW_ = 0, pageH_ = 0, pad_ = 0, maxPages_ = 0;
  uint32_t page_ = 0, shelfX_ = 0, shelfY_ = 0, shelfH_ = 0;
  bool used_ = false;
};

}  // namespace vfe
