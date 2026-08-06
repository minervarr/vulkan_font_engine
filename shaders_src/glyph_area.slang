// Pass 1 of GPU glyph rasterization: signed-area accumulation.
//
// One thread per EDGE. Each deposits the exact signed area it covers into the
// pixels it crosses, plus the cover that everything to its right inherits.
// Pass 2 (glyph_resolve.slang) turns those deltas into coverage with a running
// sum along each row.
//
// This is a transliteration of areaAccumulateEdge() in core/area_raster.hh,
// and it is meant to stay one — that header is checked against FreeType by
// area_raster_test, which is the only reason to believe either of them.
// Change one, change the other.
//
// FIXED POINT, deliberately. These adds land in whatever order the hardware
// schedules them; integer addition is associative and commutative, so the sum
// is bit-identical every run. Float atomics would not be, and would need an
// extension the engine does not request. 32-bit integer atomics on storage
// buffers are core Vulkan with no feature bit.

struct CellDesc {
  uint w;
  uint h;
  uint accOff;   // first int of this cell's (w+1)*h accumulator
  uint covOff;   // first BYTE of this cell's packed coverage, 4-byte aligned
};

struct Push {
  uint edgeCount;
  uint pad0, pad1, pad2;
};
[[vk::push_constant]] ConstantBuffer<Push> push;

[[vk::binding(0, 0)]] StructuredBuffer<float4>   edges;     // x0 y0 x1 y1, cell-local
[[vk::binding(1, 0)]] StructuredBuffer<uint>     edgeCell;  // which cell each edge belongs to
[[vk::binding(2, 0)]] StructuredBuffer<CellDesc> cells;
[[vk::binding(3, 0)]] RWStructuredBuffer<int>    acc;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint>   cov;       // written by pass 2

// Must equal vfe::kAreaScale (1 << kAreaShift) in core/area_raster.hh.
static const float kAreaScale = 65536.0;

// Round, never truncate: a cast toward zero shrinks the magnitude of every
// contribution, and the resolve's running sum accumulates those shrinkages
// instead of cancelling them. Measured against FreeType it read as coverage
// that was always low, by up to 54/255.
void addFixed(uint rowBase, int xi, int w, float v) {
  if (xi < 0 || xi > w) return;
  const float f = v * kAreaScale;
  const int q = int(f < 0.0 ? f - 0.5 : f + 0.5);
  if (q == 0) return;
  int prev;
  InterlockedAdd(acc[rowBase + uint(xi)], q, prev);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  const uint i = tid.x;
  if (i >= push.edgeCount) return;

  const float4 e = edges[i];
  const CellDesc c = cells[edgeCell[i]];
  const int w = int(c.w);
  const int h = int(c.h);
  if (w <= 0 || h <= 0) return;

  // A horizontal edge covers no area and crosses no scanline.
  if (e.y == e.w) return;

  // Orient downward; the sign is what makes a reversed contour subtract.
  float p0x = e.x, p0y = e.y, p1x = e.z, p1y = e.w;
  float dir = 1.0;
  if (p0y > p1y) {
    float t;
    t = p0x; p0x = p1x; p1x = t;
    t = p0y; p0y = p1y; p1y = t;
    dir = -1.0;
  }

  const int stride = w + 1;
  const float dxdy = (p1x - p0x) / (p1y - p0y);

  int yi = int(floor(p0y));
  const int yEnd = int(ceil(p1y));
  float x = p0x;

  if (yi < 0) {                      // entering the cell from above
    x += dxdy * (0.0 - p0y);
    yi = 0;
  }

  for (; yi < h && yi < yEnd; ++yi) {
    const float rowTop = float(yi);
    const float segTop    = max(p0y, rowTop);
    const float segBottom = min(p1y, rowTop + 1.0);
    const float dy = segBottom - segTop;
    if (dy <= 0.0) continue;

    const float xnext = x + dxdy * dy;
    const float d = dy * dir;

    float x0 = min(x, xnext);
    float x1 = max(x, xnext);
    x0 = clamp(x0, 0.0, float(w));
    x1 = clamp(x1, 0.0, float(w));

    const float x0floor = floor(x0);
    const float x1ceil  = ceil(x1);
    const int x0i = clamp(int(x0floor), 0, w);
    const int x1i = clamp(int(x1ceil),  0, w);

    const uint rowBase = c.accOff + uint(yi) * uint(stride);

    if (x1i <= x0i + 1) {
      const float xmf = 0.5 * (x + xnext) - x0floor;
      addFixed(rowBase, x0i,     w, d - d * xmf);
      addFixed(rowBase, x0i + 1, w, d * xmf);
    } else {
      const float s   = 1.0 / (x1 - x0);
      const float x0f = x0 - x0floor;
      const float a0  = 0.5 * s * (1.0 - x0f) * (1.0 - x0f);
      const float x1f = x1 - x1ceil + 1.0;
      const float am  = 0.5 * s * x1f * x1f;

      addFixed(rowBase, x0i, w, d * a0);
      if (x1i == x0i + 2) {
        addFixed(rowBase, x0i + 1, w, d * (1.0 - a0 - am));
      } else {
        const float a1 = s * (1.5 - x0f);
        addFixed(rowBase, x0i + 1, w, d * (a1 - a0));
        for (int xi = x0i + 2; xi < x1i - 1; ++xi)
          addFixed(rowBase, xi, w, d * s);
        const float a2 = a1 + float(x1i - x0i - 3) * s;
        addFixed(rowBase, x1i - 1, w, d * (1.0 - a2 - am));
      }
      addFixed(rowBase, x1i, w, d * am);
    }

    x = xnext;
  }
}
