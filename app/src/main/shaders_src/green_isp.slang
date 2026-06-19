// Green-plane pass for the high-quality (RCD-style) demosaic path.
//
// Computes a full-resolution green channel with Hamilton-Adams DIRECTIONAL
// interpolation at R/B sites (green sites pass through unchanged). For each
// red/blue site it forms a horizontal and a vertical green estimate (each with
// the second-order correction from the known same-color channel, as in Malvar),
// then blends them weighted by the OPPOSITE gradient — so the smoother direction
// dominates and green is interpolated ALONG edges, not across them. That soft
// directional selection is what removes the zipper/maze artifacts the
// non-directional Malvar kernel leaves on edges.
//
// Output is normalized (black-subtracted, white-normalized) linear green, one
// float per pixel, consumed by debayer_isp.slang's HQ branch to reconstruct R/B
// via residual color differences (raw - green). Only dispatched when DM:HQ is
// on; the Malvar path ignores this buffer.
//
// Input is the same uint16-packed Bayer buffer the ISP reads (2 px per uint32,
// tight rawDim.x stride) — either the camera-written staging slot or, when the
// denoise prepass is on, the denoised buffer.

[[vk::binding(0, 0)]] StructuredBuffer<uint>    rawBuf;    // uint16 pairs (Bayer)
[[vk::binding(1, 0)]] RWStructuredBuffer<float> greenBuf;  // full-res green (normalized)

struct PushConstants {
    uint2  rawDim;   // raw Bayer WxH
    uint   cfa;      // 0=RGGB 1=GRBG 2=GBRG 3=BGGR (low 2 bits)
    float  white;    // sensor white level (raw units)
    float4 black;    // black level per 2x2 CFA position (row-major)
};
[[vk::push_constant]] ConstantBuffer<PushConstants> push;

static const uint kCfaColor[4][4] = {
    { 0, 1, 1, 2 },   // RGGB
    { 1, 0, 2, 1 },   // GRBG
    { 1, 2, 0, 1 },   // GBRG
    { 2, 1, 1, 0 },   // BGGR
};
uint colorAt(int2 p) {
    return kCfaColor[push.cfa & 3u][(uint(p.y) & 1u) * 2u + (uint(p.x) & 1u)];
}

int mirrorCoord(int v, int dim) {
    if (v < 0)    v = -v;
    if (v >= dim) v = 2 * dim - 2 - v;   // even reflection preserves Bayer parity
    return v;
}

uint rawAt(int2 p) {
    uint idx  = uint(p.y) * push.rawDim.x + uint(p.x);   // rawDim.x is even
    uint word = rawBuf[idx >> 1];
    return ((idx & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
}

// Black-subtracted, white-normalized linear sample at (x, y) — must match the
// fetch() in debayer_isp.slang so residual color differences stay consistent.
float fetch(int2 p) {
    p.x = mirrorCoord(p.x, int(push.rawDim.x));
    p.y = mirrorCoord(p.y, int(push.rawDim.y));
    float blk = push.black[(uint(p.y) & 1u) * 2u + (uint(p.x) & 1u)];
    return max(0.0, (float(rawAt(p)) - blk)) / max(1.0, push.white - blk);
}

// Hamilton-Adams directional green at a red/blue site (site value C is the known
// R or B). gH/gV: green from the axial neighbors plus the second-order term from
// C; dH/dV: the matching gradient magnitudes. Weighting each estimate by the
// opposite gradient picks the smoother direction without hard switching.
float greenHA(int2 p) {
    float C  = fetch(p);
    float l1 = fetch(p + int2(-1, 0)), r1 = fetch(p + int2(1, 0));
    float u1 = fetch(p + int2(0, -1)), d1 = fetch(p + int2(0, 1));
    float l2 = fetch(p + int2(-2, 0)), r2 = fetch(p + int2(2, 0));
    float u2 = fetch(p + int2(0, -2)), d2 = fetch(p + int2(0, 2));

    float gH = 0.5 * (l1 + r1) + 0.25 * (2.0 * C - l2 - r2);
    float gV = 0.5 * (u1 + d1) + 0.25 * (2.0 * C - u2 - d2);
    float dH = abs(l1 - r1) + abs(2.0 * C - l2 - r2);
    float dV = abs(u1 - d1) + abs(2.0 * C - u2 - d2);

    float wH = dV + 1e-5;   // large vertical gradient -> horizontal is smoother
    float wV = dH + 1e-5;
    return (wH * gH + wV * gV) / (wH + wV);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int x = int(tid.x), y = int(tid.y);
    if (x >= int(push.rawDim.x) || y >= int(push.rawDim.y)) return;
    int2 p = int2(x, y);
    float g = (colorAt(p) == 1u) ? fetch(p) : greenHA(p);
    greenBuf[uint(y) * push.rawDim.x + uint(x)] = g;
}
