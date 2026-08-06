// Pass 2 of GPU glyph rasterization: turn the accumulated deltas into
// coverage, one thread per CELL ROW.
//
// Transliteration of areaResolveRow() in core/area_raster.hh — keep the two in
// step; area_raster_test is what validates the arithmetic.
//
// Rows are packed four coverage bytes to a uint so the result can go straight
// into an R8 atlas layer with vkCmdCopyBufferToImage. Each cell's row stride is
// rounded up to a multiple of 4 for that packing, and its byte offset is
// 4-aligned, which is also what VkBufferImageCopy::bufferOffset requires.

struct CellDesc {
  uint w;
  uint h;
  uint accOff;
  uint covOff;
};

struct Push {
  uint cellCount;
  uint pad0, pad1, pad2;
};
[[vk::push_constant]] ConstantBuffer<Push> push;

[[vk::binding(0, 0)]] StructuredBuffer<float4>   edges;
[[vk::binding(1, 0)]] StructuredBuffer<uint>     edgeCell;
[[vk::binding(2, 0)]] StructuredBuffer<CellDesc> cells;
[[vk::binding(3, 0)]] RWStructuredBuffer<int>    acc;
[[vk::binding(4, 0)]] RWStructuredBuffer<uint>   cov;

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint3 tid : SV_DispatchThreadID) {
  const uint cellIdx = tid.y;
  if (cellIdx >= push.cellCount) return;

  const CellDesc c = cells[cellIdx];
  const uint row = tid.x;
  if (row >= c.h || c.w == 0) return;

  const uint accBase   = c.accOff + row * (c.w + 1);
  const uint rowStride = (c.w + 3u) & ~3u;          // bytes, multiple of 4
  const uint outBase   = (c.covOff + row * rowStride) >> 2;

  int sum = 0;
  for (uint xb = 0; xb < rowStride; xb += 4) {
    uint packed = 0;
    for (uint k = 0; k < 4; ++k) {
      const uint x = xb + k;
      uint b = 0;
      if (x < c.w) {
        sum += acc[accBase + x];
        const int v = abs(sum);
        // Fixed-point 1.0 is 1<<16; scale to 0..255 with rounding.
        const int cvg = min((v * 255 + 32768) >> 16, 255);
        b = uint(cvg);
      }
      packed |= b << (k * 8);
    }
    cov[outBase + (xb >> 2)] = packed;
  }
}
