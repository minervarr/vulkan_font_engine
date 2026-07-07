// Second compute pass: rasterize curves into the output image.
// - Type 4 (winding / filled glyph): read from row buffer (all curves in tile-row).
//   The row buffer holds curves for the ENTIRE tile-row width, so the rightward
//   winding ray can see curves in any tile column to the right.
// - Types 0-3 (SDF): read from tile buffer (curves in this 16x16 tile only).
[[vk::binding(0, 0)]] RWStructuredBuffer<float>   curves;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint>    tiles;
[[vk::binding(2, 0)]] RWTexture2D<float4>         outputImage;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint>    winds;

struct PushConstants {
  uint screenWidth;
  uint screenHeight;
  uint curveCount;
  uint tileCountX;
  uint tileCountY;
  uint hasWinding;     // 0 when the scene has no type-4/5/6 curves → skip Pass 2
};

[[vk::push_constant]] ConstantBuffer<PushConstants> push;

static const uint TILE_STRIDE       = 97;    // 1 count + 96 curve indices per SDF tile
static const uint WIND_STRIDE       = 65;    // 1 count + 64 winding indices per winding-tile
static const uint MAX_PER_TILE      = 96u;   // must match tiling.slang + host
static const uint MAX_PER_WIND_TILE = 64u;

// ── Robust winding-crossing helpers ─────────────────────────────────────────
//
// Each curve contributes the signed number of times the rightward ray from the
// pixel centre (fpx, fpy) crosses it.  Naively counting parametric roots of
// y(t) == fpy and signing them by dy/dt is NOT robust: when the ray passes
// exactly through a Y-extremum or through a vertex shared by two adjoining
// curve records, the crossing is double-counted or dropped, flipping the
// winding parity of every pixel to its left on that scanline.  Because font
// glyphs share horizontal features at identical Y across a text row (cap
// height, x-height, baseline, the mid-bars of E/F/T, round-glyph overshoot),
// the error lands on a whole row at once — a stray full-width line, or a solid
// block where several adjacent scanlines err.
//
// Fix: split each Bézier into Y-monotonic spans (at its dy/dt roots) and apply
// the half-open straddle rule  (yA > fpy) != (yB > fpy)  per span, exactly as
// for a line segment.  Extrema then touch without crossing (net 0) and a shared
// joint vertex is owned by exactly one span, so parity stays correct.

float quadEvalY(float p0, float p1, float p2, float t) {
  float u = 1.0 - t;
  return u*u*p0 + 2.0*u*t*p1 + t*t*p2;
}

float cubicEval(float p0, float p1, float p2, float p3, float t) {
  float u = 1.0 - t;
  return u*u*u*p0 + 3.0*u*u*t*p1 + 3.0*u*t*t*p2 + t*t*t*p3;
}

// Horizontal segment from (xa,ya) to (xb,yb), monotonic in y, crosses the ray
// at fpy iff its endpoints straddle fpy under the half-open rule.  Returns the
// signed crossing if the intersection lies to the right of fpx.
// Each winding helper also reports, via nearestX, the X of its closest crossing
// strictly to the right of fpx (or leaves nearestX untouched if none). The fill
// colour is taken from the curve owning that nearest crossing — i.e. the right
// edge of the glyph the pixel actually sits in — so colour can't bleed in from
// glyphs further along the same text row.
int segCrossLine(float xa, float ya, float xb, float yb,
                 float fpx, float fpy, inout float nearestX) {
  bool a = ya > fpy;
  bool b = yb > fpy;
  if (a == b) return 0;
  float t  = (fpy - ya) / (yb - ya);
  float xi = xa + t * (xb - xa);
  if (xi > fpx) { nearestX = min(nearestX, xi); return (yb > ya) ? 1 : -1; }
  return 0;
}

int windingContribLine(uint base, float fpx, float fpy, inout float nearestX) {
  return segCrossLine(curves[base+1], curves[base+2],
                      curves[base+3], curves[base+4], fpx, fpy, nearestX);
}

// ── Quadratic Bézier ─────────────────────────────────────────────────────────

int windingContribQuadratic(uint base, float fpx, float fpy, inout float nearestX) {
  float x0 = curves[base+1], y0 = curves[base+2];
  float x1 = curves[base+3], y1 = curves[base+4];
  float x2 = curves[base+5], y2 = curves[base+6];

  // Y-monotonic breakpoints: 0, the dy/dt root (if interior), 1.
  float bp[3];
  int nb = 0;
  bp[nb++] = 0.0;
  float denom = y0 - 2.0*y1 + y2;
  if (abs(denom) > 1e-6) {
    float te = (y0 - y1) / denom;
    if (te > 0.0 && te < 1.0) bp[nb++] = te;
  }
  bp[nb++] = 1.0;

  int w = 0;
  for (int s = 0; s < nb - 1; s++) {
    float ta = bp[s], tb = bp[s+1];
    float ya = quadEvalY(y0, y1, y2, ta);
    float yb = quadEvalY(y0, y1, y2, tb);
    bool A = ya > fpy;
    if (A == (yb > fpy)) continue;              // span does not straddle fpy
    // Monotonic in y on [ta,tb] → bisect for the unique crossing parameter.
    float lo = ta, hi = tb;
    for (int it = 0; it < 12; it++) {
      float tm = 0.5*(lo+hi);
      if ((quadEvalY(y0, y1, y2, tm) > fpy) == A) lo = tm; else hi = tm;
    }
    float tc = 0.5*(lo+hi);
    float xi = quadEvalY(x0, x1, x2, tc);       // same basis works for x
    if (xi > fpx) { nearestX = min(nearestX, xi); w += (yb > ya) ? 1 : -1; }
  }
  return w;
}

// ── Cubic Bézier ─────────────────────────────────────────────────────────────

int windingContribCubic(uint base, float fpx, float fpy, inout float nearestX) {
  float x0 = curves[base+1], y0 = curves[base+2];
  float x1 = curves[base+3], y1 = curves[base+4];
  float x2 = curves[base+5], y2 = curves[base+6];
  float x3 = curves[base+7], y3 = curves[base+8];

  // dy/dt = 3(A t^2 + B t + C); collect its interior roots as breakpoints.
  float A = (y3 - 3.0*y2 + 3.0*y1 - y0);
  float B = 2.0*(y0 - 2.0*y1 + y2);
  float C = (y1 - y0);

  float e0 = -1.0, e1 = -1.0;
  if (abs(A) < 1e-6) {
    if (abs(B) > 1e-6) e0 = -C / B;
  } else {
    float disc = B*B - 4.0*A*C;
    if (disc >= 0.0) {
      float sq = sqrt(disc);
      e0 = (-B + sq) / (2.0*A);
      e1 = (-B - sq) / (2.0*A);
    }
  }

  // Build sorted breakpoints in [0,1]: 0, e0, e1, 1.
  float bp[4];
  int nb = 0;
  bp[nb++] = 0.0;
  float lo2 = min(e0, e1), hi2 = max(e0, e1);
  if (lo2 > 0.0 && lo2 < 1.0) bp[nb++] = lo2;
  if (hi2 > 0.0 && hi2 < 1.0 && hi2 != lo2) bp[nb++] = hi2;
  bp[nb++] = 1.0;

  int w = 0;
  for (int s = 0; s < nb - 1; s++) {
    float ta = bp[s], tb = bp[s+1];
    float ya = cubicEval(y0, y1, y2, y3, ta);
    float yb = cubicEval(y0, y1, y2, y3, tb);
    bool a = ya > fpy;
    if (a == (yb > fpy)) continue;
    float lo = ta, hi = tb;
    for (int it = 0; it < 12; it++) {
      float tm = 0.5*(lo+hi);
      if ((cubicEval(y0, y1, y2, y3, tm) > fpy) == a) lo = tm; else hi = tm;
    }
    float tc = 0.5*(lo+hi);
    float xi = cubicEval(x0, x1, x2, x3, tc);
    if (xi > fpx) { nearestX = min(nearestX, xi); w += (yb > ya) ? 1 : -1; }
  }
  return w;
}

// ── SDF curve evaluation (types 0-3) ───────────────────────────────────────

float evaluateCurve(uint curveIndex, float fpx, float fpy)
{
  uint base = curveIndex * 20;
  uint type = uint(curves[base + 0]);
  float a   = curves[base + 1];
  float b   = curves[base + 2];
  float c   = curves[base + 3];
  float d   = curves[base + 4];
  float result = 1.0;

  if (type == 0u) {
    result = (a * fpx + b * fpy + c) / sqrt(a * a + b * b);
  };
  if (type == 1u) {
    result = fpy - (a * fpx * fpx + b * fpx + c);
  };
  if (type == 2u) {
    float radius = curves[base + 5];
    float2 d = float2(abs(fpx - a) - c + radius, abs(fpy - b) - d + radius);
    result = length(max(d, 0.0)) + min(max(d.x, d.y), 0.0) - radius;
  };
  if (type == 3u) {
    float2 pp = float2(fpx, fpy);
    float2 v0 = float2(a, b);
    float2 v1 = float2(c, d);
    float2 e  = v1 - v0;
    // Zero-length segment guard: dot(e,e)==0 would make t = 0/0 = NaN, which
    // poisons the whole tile (visible garbage squares). Degenerate → a dot.
    float  ee = dot(e, e);
    float  t  = (ee > 0.0) ? saturate(dot(pp - v0, e) / ee) : 0.0;
    float2 q  = v0 + e * t;
    result    = length(pp - q);
  };

  return result;
}

// ── Main ────────────────────────────────────────────────────────────────────

// Composite all curves at one sub-pixel sample position (fpx, fpy).  Tile/cell
// lookups use the integer pixel (px, py); the sample position only feeds the
// geometric tests, so every sub-sample stays inside the pixel's own tile.
float4 shadePixel(uint px, uint py, float fpx, float fpy)
{
  float4 color = float4(0.0, 0.0, 0.0, 0.0);

  // TODO: passes are split by curve TYPE, not by curve INDEX. Painter's
  // ordering across the two passes is only correct when all SDF curves
  // (types 0-3) are emitted with lower curve indices than any winding
  // curves (types 4-6) in the scene. A future refactor should iterate all
  // overlapping curves in curve-index order per pixel, dispatching by type.

  // ── Pass 1: SDF curves (types 0-3) via tile buffer ───────────────────────
  {
    uint tileIndex  = (py / 16u) * push.tileCountX + (px / 16u);
    uint tileOffset = tileIndex * TILE_STRIDE;
    uint count      = tiles[tileOffset];

    uint n = min(count, MAX_PER_TILE);

    // Insertion-sort by curve index for painter's-algorithm ordering.
    uint sortedCurves[96];
    for (uint si = 0u; si < n; si++) sortedCurves[si] = tiles[tileOffset + 1u + si];
    for (uint si = 1u; si < n; si++) {
      uint key = sortedCurves[si];
      int  sj  = int(si) - 1;
      while (sj >= 0 && sortedCurves[sj] > key) {
        sortedCurves[sj + 1] = sortedCurves[sj];
        sj--;
      }
      sortedCurves[sj + 1] = key;
    }

    for (uint i = 0u; i < n; i++) {
      uint curveIndex = sortedCurves[i];
      uint base = curveIndex * 20u;
      if (uint(curves[base + 0]) == 4u) continue; // type-4 is in row buffer

      float4 curveColor = float4(curves[base+9], curves[base+10],
                                 curves[base+11], curves[base+12]);
      float lineWidth = curves[base + 13];
      float dist = evaluateCurve(curveIndex, fpx, fpy);

      float alpha = (lineWidth > 0.0)
        ? (1.0 - smoothstep(lineWidth - 1.0, lineWidth + 1.0, abs(dist)))
        : (1.0 - smoothstep(-0.5, 0.5, dist));

      float effA = curveColor.a * alpha;
      float4 src = float4(curveColor.rgb * effA, effA);
      color = src + color * (1.0 - src.a);
    }
  }

  // ── Pass 2: winding fill (types 4/5/6) via winding-tile buffer ───────────
  // Winding curves were registered into the RIGHTMOST tile of their X-bbox
  // per tile-row.  A curve whose rightmost-tile is < this pixel's tile column
  // has its maxX strictly less than fpx, so the rightward ray cannot cross
  // it.  Start the walk at the pixel's own column to skip all such curves.
  if (push.hasWinding != 0u) {
    uint ty = py / 16u;
    uint pixelTx = px / 16u;

    int    totalWinding = 0;
    float4 windingColor = float4(1.0, 1.0, 1.0, 1.0);
    float  nearestX     = 1e30;   // X of the closest crossing right of the pixel

    for (uint tx = pixelTx; tx < push.tileCountX; tx++) {
      uint cellOffset = (ty * push.tileCountX + tx) * WIND_STRIDE;
      uint cellCount  = min(winds[cellOffset], MAX_PER_WIND_TILE);

      for (uint i = 0u; i < cellCount; i++) {
        uint ci      = winds[cellOffset + 1u + i];
        uint base    = ci * 20u;

        // Bbox reject: the rightward ray at fpy can only cross curves whose
        // Y-range contains the scanline and whose right edge lies to the right
        // of the pixel.  Skips the bisection work for the common misses.
        if (fpy < curves[base+15] || fpy > curves[base+17] ||
            curves[base+16] <= fpx) continue;

        uint  ciType  = uint(curves[base]);
        float curveNearX = 1e30;
        int   contrib = 0;
        if (ciType == 4u) contrib = windingContribCubic(base, fpx, fpy, curveNearX);
        if (ciType == 5u) contrib = windingContribQuadratic(base, fpx, fpy, curveNearX);
        if (ciType == 6u) contrib = windingContribLine(base, fpx, fpy, curveNearX);
        if (contrib != 0) {
          totalWinding += contrib;
          // Adopt the colour of the curve whose crossing is closest to the
          // right of the pixel — that is the right edge of the glyph the pixel
          // belongs to, so adjacent glyphs of other colours don't bleed in.
          if (curveNearX < nearestX) {
            nearestX = curveNearX;
            windingColor = float4(curves[base+9],  curves[base+10],
                                  curves[base+11], curves[base+12]);
          }
        }
      }
    }

    if (totalWinding != 0) {
      float effA = windingColor.a;
      float4 src = float4(windingColor.rgb * effA, effA);
      color = src + color * (1.0 - src.a);
    }
  }

  return color;
}

// ── Main ────────────────────────────────────────────────────────────────────
//
// Supersampling (SS×SS) only matters for the binary winding-fill path (types
// 4–6), which needs it for smooth glyph edges. SDF shapes (types 0–3) already
// get analytic 1px anti-aliasing from their smoothstep, so a single centre
// sample is enough. Consumers that animate the overlay (so the compute pass
// runs every frame, not once per scene change) and render text via MSDF quads
// instead of winding fills — e.g. the calculator — keep SS=1 to stay at 60fps;
// cost scales as SS*SS. Raise SS for static scenes that use winding-fill text.
[numthreads(16, 16, 1)]
void main(uint3 groupId : SV_GroupID, uint3 localId : SV_GroupThreadID)
{
  uint px = groupId.x * 16 + localId.x;
  uint py = groupId.y * 16 + localId.y;

  if (px >= push.screenWidth || py >= push.screenHeight) return;

  const uint SS = 1u;   // sub-samples per axis (1 = centre sample only)
  float4 acc = float4(0.0, 0.0, 0.0, 0.0);
  for (uint sy = 0u; sy < SS; sy++) {
    for (uint sx = 0u; sx < SS; sx++) {
      float fpx = float(px) + (float(sx) + 0.5) / float(SS);
      float fpy = float(py) + (float(sy) + 0.5) / float(SS);
      acc += shadePixel(px, py, fpx, fpy);
    }
  }

  outputImage[uint2(px, py)] = acc / float(SS * SS);
}
