// RAW16 Bayer -> P010 ISP compute pass.
//
// One thread handles one 2x2 output quad (4 luma pixels + 1 subsampled
// chroma pair -> natural 4:2:0). Per pixel:
//   black-level subtract + normalize -> Malvar-He-Cutler 5x5 demosaic ->
//   white balance -> CCM (sensor -> BT.2020 linear) -> PQ (ST 2084) OETF ->
//   BT.2020 NCL RGB->YCbCr, 10-bit FULL range -> P010 (10 bits in the high
//   bits of each uint16).
//
// The output buffer is the encoder's P010 frame: Y plane (stridePixels
// uint16 per row), then interleaved CbCr plane at uvWordOffset (in uint32
// words from the buffer start).

// Bayer input as a raw uint16-packed buffer (2 pixels per uint32, row-major,
// tight rawDim.x stride) — the camera writes it directly, so there is no upload
// copy. (Was a sampled R16 image; switched to a buffer to drop the per-frame
// staging->image DMA. Identical values, identical ISP math.)
[[vk::binding(0, 0)]] StructuredBuffer<uint>   rawBuf;     // uint16 pairs (Bayer)
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> outP010;    // packed uint16 pairs
// Full-res green plane from green_isp.slang's directional interpolation. Only
// read on the HQ demosaic path (cfa bit 8 set); the Malvar path ignores it, but
// it stays bound so the descriptor set is valid in either mode.
[[vk::binding(2, 0)]] StructuredBuffer<float>  greenBuf;   // normalized green
// Chroma scratch for the chroma-denoise (CD) path (cfa bit 9 set). When CD is on
// the per-site (Cb,Cr) word is written here (tight, one word per site) instead of
// into outP010's CbCr plane; chroma_denoise.slang then denoises it and writes the
// final CbCr into outP010. Stays bound (unwritten) when CD is off.
[[vk::binding(3, 0)]] RWStructuredBuffer<uint>  chromaOut;  // packed (Cb,Cr) per site

struct PushConstants {
    uint2  outDim;          // output WxH (even)
    uint   stridePixels;    // encoder Y stride in pixels (uint16 units, even)
    uint   uvWordOffset;    // uint32 word index where the UV plane starts

    uint2  rawDim;          // raw Bayer buffer WxH (mirror bounds)
    uint   cfa;             // low 2 bits: 0=RGGB 1=GRBG 2=GBRG 3=BGGR;
                            // bit 8: demosaic mode (0=Malvar, 1=HQ directional);
                            // bit 9: chroma denoise (1=divert CbCr to chromaOut)
    float  white;           // sensor white level (raw units)

    float4 black;           // black level per 2x2 CFA position (row-major)
    float4 wb;              // .rgb = white-balance gains, .w = pqScale
                            // (pqScale: linear 1.0 -> pqScale * 10000 nits)
    float4 ccm0;            // sensor RGB -> BT.2020 linear RGB, rows
    float4 ccm1;
    float4 ccm2;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> push;

// Color of the Bayer site at absolute position (x, y): 0=R 1=G 2=B.
// Tables are the 2x2 pattern row-major: [ (0,0) (1,0) (0,1) (1,1) ].
static const uint kCfaColor[4][4] = {
    { 0, 1, 1, 2 },   // RGGB
    { 1, 0, 2, 1 },   // GRBG
    { 1, 2, 0, 1 },   // GBRG
    { 2, 1, 1, 0 },   // BGGR
};

uint colorAt(int2 p) {
    return kCfaColor[push.cfa & 3u][(uint(p.y) & 1u) * 2u + (uint(p.x) & 1u)];
}

// Mirror-reflect out-of-range coordinates. Reflection is by an even offset in
// both cases (dims are even), so the Bayer parity of the site is preserved.
int mirrorCoord(int v, int dim) {
    if (v < 0)    v = -v;
    if (v >= dim) v = 2 * dim - 2 - v;
    return v;
}

// Raw Bayer value at (x, y), unpacked from the uint16-packed buffer.
uint rawAt(int2 p) {
    uint idx  = uint(p.y) * push.rawDim.x + uint(p.x);   // rawDim.x is even
    uint word = rawBuf[idx >> 1];
    return ((idx & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
}

// Black-subtracted, white-normalized linear sample at (x, y).
float fetch(int2 p) {
    p.x = mirrorCoord(p.x, int(push.rawDim.x));
    p.y = mirrorCoord(p.y, int(push.rawDim.y));
    uint  raw   = rawAt(p);
    float blk   = push.black[(uint(p.y) & 1u) * 2u + (uint(p.x) & 1u)];
    return max(0.0, (float(raw) - blk)) / max(1.0, push.white - blk);
}

// SMPTE ST 2084 (PQ) OETF. `lin` is display-light, 1.0 = 10000 nits.
float pqOetf(float lin) {
    const float m1 = 0.1593017578125;   // 2610/16384
    const float m2 = 78.84375;          // 2523/4096*128
    const float c1 = 0.8359375;         // 3424/4096
    const float c2 = 18.8515625;        // 2413/4096*32
    const float c3 = 18.6875;           // 2392/4096*32
    float y  = clamp(lin, 0.0, 1.0);
    float ym = pow(y, m1);
    return pow((c1 + c2 * ym) / (1.0 + c3 * ym), m2);
}

// Malvar-He-Cutler 5x5 demosaic at `p` (site color `c`) -> linear sensor RGB.
float3 demosaic(int2 p, uint c) {
    float C = fetch(p);

    // 5x5 neighborhood taps (axial at 1 and 2, diagonal at 1).
    float n1 = fetch(p + int2( 0, -1)), s1 = fetch(p + int2( 0, 1));
    float w1 = fetch(p + int2(-1,  0)), e1 = fetch(p + int2( 1, 0));
    float n2 = fetch(p + int2( 0, -2)), s2 = fetch(p + int2( 0, 2));
    float w2 = fetch(p + int2(-2,  0)), e2 = fetch(p + int2( 2, 0));
    float nw = fetch(p + int2(-1, -1)), ne = fetch(p + int2( 1, -1));
    float sw = fetch(p + int2(-1,  1)), se = fetch(p + int2( 1,  1));

    float axial2 = n2 + s2 + w2 + e2;
    float diag   = nw + ne + sw + se;

    if (c == 1u) {
        // Green site: interpolate R and B. One lives in this row, the other in
        // this column; which is which depends on the row's CFA phase.
        float xRow = (5.0 * C + 4.0 * (w1 + e1) - diag - (w2 + e2)
                      + 0.5 * (n2 + s2)) * 0.125;
        float xCol = (5.0 * C + 4.0 * (n1 + s1) - diag - (n2 + s2)
                      + 0.5 * (w2 + e2)) * 0.125;
        bool redInRow = (colorAt(p + int2(1, 0)) == 0u);
        float r = redInRow ? xRow : xCol;
        float b = redInRow ? xCol : xRow;
        return float3(r, C, b);
    }

    // Red or blue site: green from the axial cross, the opposite color from
    // the diagonals.
    float g   = (4.0 * C + 2.0 * (n1 + s1 + w1 + e1) - axial2) * 0.125;
    float opp = (6.0 * C + 2.0 * diag - 1.5 * axial2) * 0.125;
    return (c == 0u) ? float3(C, g, opp) : float3(opp, g, C);
}

// Green from the precomputed directional plane (green_isp.slang), mirror-padded.
float greenBufAt(int2 p) {
    p.x = mirrorCoord(p.x, int(push.rawDim.x));
    p.y = mirrorCoord(p.y, int(push.rawDim.y));
    return greenBuf[uint(p.y) * push.rawDim.x + uint(p.x)];
}

// HQ (RCD-style) demosaic: green is read from the directional plane; R and B are
// reconstructed from residual color differences (raw - green) interpolated over
// the relevant same-color neighbors. Color-difference interpolation keeps chroma
// low-frequency, which is what suppresses the false color a fixed-kernel
// (Malvar) interpolation produces on edges.
float3 demosaicHQ(int2 p, uint c) {
    float G = greenBufAt(p);

    if (c == 1u) {
        // Green site: one of R/B lives in this row, the other in this column.
        float cdL = fetch(p + int2(-1, 0)) - greenBufAt(p + int2(-1, 0));
        float cdR = fetch(p + int2( 1, 0)) - greenBufAt(p + int2( 1, 0));
        float cdU = fetch(p + int2( 0,-1)) - greenBufAt(p + int2( 0,-1));
        float cdD = fetch(p + int2( 0, 1)) - greenBufAt(p + int2( 0, 1));
        float xRow = G + 0.5 * (cdL + cdR);
        float xCol = G + 0.5 * (cdU + cdD);
        bool redInRow = (colorAt(p + int2(1, 0)) == 0u);
        float r = redInRow ? xRow : xCol;
        float b = redInRow ? xCol : xRow;
        return float3(r, G, b);
    }

    // Red or blue site: the opposite color sits on the four diagonals.
    float cd = (fetch(p + int2(-1,-1)) - greenBufAt(p + int2(-1,-1)))
             + (fetch(p + int2( 1,-1)) - greenBufAt(p + int2( 1,-1)))
             + (fetch(p + int2(-1, 1)) - greenBufAt(p + int2(-1, 1)))
             + (fetch(p + int2( 1, 1)) - greenBufAt(p + int2( 1, 1)));
    float opp = G + 0.25 * cd;
    float C   = fetch(p);
    return (c == 0u) ? float3(C, G, opp) : float3(opp, G, C);
}

// Full ISP for one pixel -> PQ-encoded non-linear BT.2020 R'G'B' in [0,1].
float3 processPixel(int2 p) {
    uint c = colorAt(p);
    float3 rgb = (((push.cfa >> 8) & 1u) != 0u) ? demosaicHQ(p, c) : demosaic(p, c);
    rgb = max(rgb * push.wb.rgb, 0.0);                    // white balance
    float3 bt2020 = float3(dot(push.ccm0.xyz, rgb),       // sensor -> BT.2020
                           dot(push.ccm1.xyz, rgb),
                           dot(push.ccm2.xyz, rgb));
    bt2020 = max(bt2020, 0.0);
    float s = push.wb.w;                                   // pqScale
    return float3(pqOetf(bt2020.r * s),
                  pqOetf(bt2020.g * s),
                  pqOetf(bt2020.b * s));
}

// BT.2020 NCL luma + chroma from PQ-encoded R'G'B'.
float lumaOf(float3 rgb)  { return dot(rgb, float3(0.2627, 0.6780, 0.0593)); }

uint quant10(float v) {           // full-range luma: [0,1] -> [0,1023]
    return uint(clamp(round(v * 1023.0), 0.0, 1023.0));
}
uint quant10c(float v) {          // full-range chroma: [-0.5,0.5] -> [0,1023]
    return uint(clamp(round(v * 1023.0 + 512.0), 0.0, 1023.0));
}

// Two 10-bit values -> one uint32 of two P010 uint16s (10 bits in high bits).
uint packP010(uint lo, uint hi) {
    return (lo << 6) | (hi << 22);
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    uint x = tid.x * 2u;
    uint y = tid.y * 2u;
    if (x >= push.outDim.x || y >= push.outDim.y) return;

    float3 p00 = processPixel(int2(int(x),     int(y)));
    float3 p10 = processPixel(int2(int(x + 1), int(y)));
    float3 p01 = processPixel(int2(int(x),     int(y + 1)));
    float3 p11 = processPixel(int2(int(x + 1), int(y + 1)));

    // Luma plane: each thread owns two horizontally-adjacent pairs.
    uint yRow0 = (y * push.stridePixels + x) / 2u;
    uint yRow1 = ((y + 1u) * push.stridePixels + x) / 2u;
    outP010[yRow0] = packP010(quant10(lumaOf(p00)), quant10(lumaOf(p10)));
    outP010[yRow1] = packP010(quant10(lumaOf(p01)), quant10(lumaOf(p11)));

    // Chroma: average the quad's PQ R'G'B', derive one CbCr pair (4:2:0).
    float3 avg = (p00 + p10 + p01 + p11) * 0.25;
    float  yAvg = lumaOf(avg);
    float  cb = (avg.b - yAvg) / 1.8814;
    float  cr = (avg.r - yAvg) / 1.4746;
    uint cWord = packP010(quant10c(cb), quant10c(cr));
    if (((push.cfa >> 9) & 1u) != 0u) {
        // CD on: divert chroma to the tight scratch buffer (one word per site);
        // chroma_denoise.slang writes the final CbCr into outP010. The index is
        // the same expression minus the UV-plane offset (stridePixels/2 stride).
        chromaOut[(tid.y * push.stridePixels + x) / 2u] = cWord;
    } else {
        uint uvWord = push.uvWordOffset + (tid.y * push.stridePixels + x) / 2u;
        outP010[uvWord] = cWord;
    }
}
