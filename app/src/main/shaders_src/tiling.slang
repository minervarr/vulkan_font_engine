// First compute pass: assign curves to 16x16 pixel tiles.
// - SDF (types 0-3) go to `tiles` (full bbox overlap).
// - Winding (types 4/5/6) go to `winds`, registered in the RIGHTMOST tile of
//   their X-bbox per tile-row they span. The coverage shader walks tile columns
//   from the pixel's own column rightward to gather winding crossings.
[[vk::binding(0, 0)]] RWStructuredBuffer<float> curves;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint>  tiles;
[[vk::binding(3, 0)]] RWStructuredBuffer<uint>  winds;

struct PushConstants {
  uint screenWidth;
  uint screenHeight;
  uint curveCount;
  uint tileCountX;
  uint tileCountY;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> push;

static const uint TILE_STRIDE       = 97;    // 1 count + 96 curve indices
static const uint WIND_STRIDE       = 65;    // 1 count + 64 winding indices
static const uint MAX_PER_TILE      = 96;    // must match OverlayRasterizer::MAX_CURVES_PER_TILE
static const uint MAX_PER_WIND_TILE = 64;

[numthreads(64, 1, 1)]
void main(uint3 dispatchId : SV_DispatchThreadID)
{
  uint curveIndex = dispatchId.x;
  if (curveIndex >= push.curveCount) return;

  uint base   = curveIndex * 20;
  uint type   = uint(curves[base + 0]);
  float minX  = curves[base + 14];
  float minY  = curves[base + 15];
  float maxX  = curves[base + 16];
  float maxY  = curves[base + 17];

  uint tileMinY = uint(max(0.0, minY)) / 16;
  uint tileMaxY = uint(min(float(push.screenHeight - 1), maxY)) / 16;

  if (type == 4u || type == 5u || type == 6u) {
    // Winding curves: register in the RIGHTMOST overlapping tile of each
    // tile-row the Y-bbox spans.  The coverage consumer walks
    // tx = px/16 .. tileCountX-1, so any curve whose rightmost-tile is left
    // of the pixel's column is provably out of reach of the rightward ray
    // (its maxX < pixel.fpx) and can be skipped for free.
    uint tileMaxX = uint(min(float(push.screenWidth - 1), maxX)) / 16;
    for (uint ty = tileMinY; ty <= tileMaxY; ty++) {
      uint cellOffset = (ty * push.tileCountX + tileMaxX) * WIND_STRIDE;
      uint slot;
      InterlockedAdd(winds[cellOffset], 1, slot);
      if (slot < MAX_PER_WIND_TILE) {
        winds[cellOffset + 1 + slot] = curveIndex;
      }
    }
  } else {
    // SDF curves (types 0-3): assign to tiles overlapping the full bounding box.
    uint tileMinX = uint(max(0.0, minX)) / 16;
    uint tileMaxX = uint(min(float(push.screenWidth - 1), maxX)) / 16;

    for (uint ty = tileMinY; ty <= tileMaxY; ty++) {
      for (uint tx = tileMinX; tx <= tileMaxX; tx++) {
        uint tileIndex  = ty * push.tileCountX + tx;
        uint tileOffset = tileIndex * TILE_STRIDE;
        uint slot;
        InterlockedAdd(tiles[tileOffset], 1, slot);
        if (slot < MAX_PER_TILE) {
          tiles[tileOffset + 1 + slot] = curveIndex;
        }
      }
    }
  }
}
