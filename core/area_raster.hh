#pragma once
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

// ── Analytic area coverage, by signed-area accumulation ─────────────────────
//
// The same scanline algorithm FreeType's smooth rasterizer uses, and the same
// one font-rs implements: instead of sampling, each edge deposits the EXACT
// signed area it covers into the pixels it crosses, plus a "cover" delta that
// every pixel to its right inherits. A running sum along each row then turns
// those deltas into true coverage.
//
// This exists to be ported to a compute shader, and every decision here is
// made with that in mind:
//
// 1. **Fixed point, not float.** Accumulation on the GPU is one atomicAdd per
//    touched pixel, and atomics arrive in an order nobody controls. Integer
//    addition is associative and commutative, so the sum is bit-identical
//    however the hardware interleaves it; float addition is neither, and the
//    same glyph would rasterize differently from run to run. 32-bit integer
//    atomics on storage buffers are also core Vulkan with no feature bit,
//    while float atomics need an extension this engine does not request.
//
// 2. **One accumulator per pixel, one pass, no ordering between edges.** No
//    edge needs to see another, so the parallel version is one thread per
//    edge with no synchronisation beyond the atomic itself.
//
// 3. **The resolve is a per-row prefix sum**, which is one thread per row.
//
// Why this algorithm and not supersampling: it is the arithmetic the
// reference implementation performs, so it can actually MATCH the reference
// rather than approximate it. That is the whole bet — see area_raster_test,
// which measures the difference against FreeType directly.
//
// Pure: <cstdint>, <cstdlib> and <vector>. No FreeType, no Vulkan. Keep it
// that way; being checkable as arithmetic is the point.

namespace vfe {

// One flattened edge, in CELL-LOCAL pixel coordinates: x right, y DOWN, the
// origin at the cell's top-left texel corner. Curves are flattened to these
// before they arrive.
struct AreaEdge {
  float x0 = 0, y0 = 0;
  float x1 = 0, y1 = 0;
};

// Fixed-point scale for the accumulator. A pixel's area contribution is in
// [-1, 1], so 16 bits of fraction leaves ~15 bits of headroom for however many
// edges overlap it — far more than any glyph needs, and it keeps the whole
// accumulator in int32, which is what the shader can atomically add to.
inline constexpr int kAreaShift = 16;
inline constexpr float kAreaScale = float(1 << kAreaShift);

// The accumulator is (w + 1) wide per row.
//
// An edge whose crossing ends inside the last column still deposits the
// remainder of its cover one column further right, and that column has to
// exist to be written to. Nothing reads it — the resolve stops at w — but
// without it every glyph would corrupt the first pixel of the next row.
inline constexpr int areaAccStride(int w) { return w + 1; }

// ── Accumulate one edge ─────────────────────────────────────────────────────
//
// `acc` is areaAccStride(w) * h int32 cells. Callers zero it once, then call
// this for every edge in any order, from any number of threads (with an
// atomic add in place of `+=`). Written as a function over a flat buffer so
// the shader port is a transliteration rather than a re-derivation.
//
// The decomposition below is the standard one (font-rs / libgd's `gdImageAA`
// lineage, and the same partitioning FreeType's `gray_render_line` performs).
// It is written out rather than derived because an ad-hoc version of it —
// splitting dy in proportion to the x span and giving each pixel "its own
// trapezoid" — is very nearly right, which is the worst thing it could be: it
// produced a mean error of 1.1/255 against FreeType, invisible on inspection,
// with a worst pixel of 53/255 and disagreements in pixels FreeType called
// fully inked. Those bounds are what area_raster_test now pins.
inline void areaAccumulateEdge(int32_t* acc, int w, int h, const AreaEdge& e) {
  // A horizontal edge covers no area and crosses no scanline.
  if (e.y0 == e.y1) return;

  // Orient the edge downward and remember which way it originally ran: the
  // sign is what makes a counter-clockwise contour subtract.
  float p0x = e.x0, p0y = e.y0, p1x = e.x1, p1y = e.y1;
  float dir = 1.0f;
  if (p0y > p1y) {
    float t;
    t = p0x; p0x = p1x; p1x = t;
    t = p0y; p0y = p1y; p1y = t;
    dir = -1.0f;
  }

  const int stride = areaAccStride(w);
  const float dxdy = (p1x - p0x) / (p1y - p0y);

  int yi = (int)std::floor(p0y);
  const int yEnd = (int)std::ceil(p1y);
  float x = p0x;

  if (yi < 0) {                       // enter the cell from above
    x += dxdy * (0.0f - p0y);
    yi = 0;
  }

  for (; yi < h && yi < yEnd; ++yi) {
    const float rowTop    = (float)yi;
    const float rowBottom = rowTop + 1.0f;
    const float segTop    = p0y > rowTop    ? p0y : rowTop;
    const float segBottom = p1y < rowBottom ? p1y : rowBottom;
    const float dy = segBottom - segTop;
    if (dy <= 0.0f) { continue; }

    const float xnext = x + dxdy * dy;
    const float d = dy * dir;                  // total cover this row

    float x0 = x < xnext ? x : xnext;
    float x1 = x < xnext ? xnext : x;

    // The cell is the outline's own grid-fitted bounding box, so this only
    // trims float rounding at the edges — but an index off the end would
    // corrupt a neighbouring row, so it is clamped rather than trusted.
    if (x0 < 0.0f) x0 = 0.0f;
    if (x1 < 0.0f) x1 = 0.0f;
    if (x0 > (float)w) x0 = (float)w;
    if (x1 > (float)w) x1 = (float)w;

    const float x0floor = std::floor(x0);
    const float x1ceil  = std::ceil(x1);
    int x0i = (int)x0floor;
    int x1i = (int)x1ceil;
    if (x0i < 0) x0i = 0;
    if (x1i > w) x1i = w;

    int32_t* row = acc + (size_t)yi * stride;
    auto add = [&](int xi, float v) {
      if (xi < 0 || xi > w) return;
      // ROUND, never truncate. A C cast truncates toward zero, which shrinks
      // the magnitude of every single contribution — and since the resolve is
      // a running sum, those shrinkages accumulate along the row instead of
      // cancelling. Measured against FreeType it showed up as coverage that
      // was ALWAYS low, by up to 54/255 on a densely-flattened glyph, and it
      // got worse the finer the curves were subdivided.
      const float f = v * kAreaScale;
      row[xi] += (int32_t)(f < 0.0f ? f - 0.5f : f + 0.5f);
    };

    if (x1i <= x0i + 1) {
      // The crossing stays inside one pixel: split its cover by where the
      // midpoint sits in that pixel.
      const float xmf = 0.5f * (x + xnext) - x0floor;
      add(x0i,     d - d * xmf);
      add(x0i + 1, d * xmf);
    } else {
      // It spans several. a0 is the first pixel's share, am the last's, and
      // the columns between take an equal slice each.
      const float s   = 1.0f / (x1 - x0);
      const float x0f = x0 - x0floor;
      const float a0  = 0.5f * s * (1.0f - x0f) * (1.0f - x0f);
      const float x1f = x1 - x1ceil + 1.0f;
      const float am  = 0.5f * s * x1f * x1f;

      add(x0i, d * a0);
      if (x1i == x0i + 2) {
        add(x0i + 1, d * (1.0f - a0 - am));
      } else {
        const float a1 = s * (1.5f - x0f);
        add(x0i + 1, d * (a1 - a0));
        for (int xi = x0i + 2; xi < x1i - 1; ++xi) add(xi, d * s);
        const float a2 = a1 + (float)(x1i - x0i - 3) * s;
        add(x1i - 1, d * (1.0f - a2 - am));
      }
      add(x1i, d * am);
    }

    x = xnext;
  }
}

// ── Resolve one row ─────────────────────────────────────────────────────────
//
// The running sum along the row is the winding-weighted coverage; abs() gives
// the non-zero fill rule and the clamp handles self-overlapping contours.
// One thread per row on the GPU.
inline void areaResolveRow(const int32_t* accRow, uint8_t* out, int w) {
  int32_t sum = 0;
  for (int x = 0; x < w; ++x) {
    sum += accRow[x];
    int32_t v = sum < 0 ? -sum : sum;
    // Fixed-point 1.0 is (1 << kAreaShift); scale to 0..255 with rounding.
    int32_t c = (v * 255 + (1 << (kAreaShift - 1))) >> kAreaShift;
    if (c > 255) c = 255;
    out[x] = uint8_t(c);
  }
}

// ── The whole thing, for a caller that just wants a cell ────────────────────
inline void areaRasterize(const std::vector<AreaEdge>& edges, int w, int h,
                          std::vector<uint8_t>& cov) {
  cov.assign((size_t)w * h, 0);
  if (w <= 0 || h <= 0) return;
  const int stride = areaAccStride(w);
  std::vector<int32_t> acc((size_t)stride * h, 0);
  for (const AreaEdge& e : edges) areaAccumulateEdge(acc.data(), w, h, e);
  for (int y = 0; y < h; ++y)
    areaResolveRow(acc.data() + (size_t)y * stride, cov.data() + (size_t)y * w, w);
}

}  // namespace vfe
