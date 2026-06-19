// Bayer-domain bilateral denoise prepass.
//
// Averages the center with its 8 same-color neighbors — distance-2 taps land on
// the same Bayer color AND the same 2x2 position, so no CFA bookkeeping is
// needed and chroma isn't cross-contaminated. Bilateral weights use a
// shot-noise-adaptive sigma (noise variance ~ signal, so shadows smooth more
// while edges/highlights stay crisp). Output is raw Bayer in the SAME units and
// SAME uint16-packed buffer layout as the input, so the ISP debayer pass
// consumes either the raw or the denoised buffer unchanged.
//
// In/out are uint16-packed buffers (2 pixels per uint32, row-major, tight
// rawDim.x stride). Each thread owns a horizontal pixel pair so it can write a
// whole uint32 without a packing race.

[[vk::binding(0, 0)]] StructuredBuffer<uint>   rawBuf;       // uint16 pairs (Bayer)
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> denoisedBuf;  // uint16 pairs (denoised)

struct PushConstants {
    uint2  rawDim;
    float  noiseK;       // normalized shot-noise coefficient
    float  noiseFloor;   // normalized read-noise floor
    float4 black;        // black level per 2x2 CFA position
    float4 whitePad;     // .x = white level
};
[[vk::push_constant]] ConstantBuffer<PushConstants> push;

int mirror(int v, int dim) {
    if (v < 0)    v = -v;
    if (v >= dim) v = 2 * dim - 2 - v;   // even reflection preserves Bayer parity
    return v;
}

uint rawAt(int2 p) {
    uint idx  = uint(p.y) * push.rawDim.x + uint(p.x);   // rawDim.x is even
    uint word = rawBuf[idx >> 1];
    return ((idx & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu);
}

float fetchRaw(int2 p) {
    p.x = mirror(p.x, int(push.rawDim.x));
    p.y = mirror(p.y, int(push.rawDim.y));
    return float(rawAt(p));
}

// Bilateral denoise of one pixel -> raw value in the same units (<= white).
uint denoisePixel(int x, int y) {
    float white = push.whitePad.x;
    float blk   = push.black[(uint(y) & 1u) * 2u + (uint(x) & 1u)];
    float span  = max(1.0, white - blk);

    float c     = fetchRaw(int2(x, y));
    float cn    = max(0.0, (c - blk) / span);                       // normalized signal
    float sigma = push.noiseK * sqrt(max(cn, 1e-4)) + push.noiseFloor;
    float inv2s2 = 1.0 / (2.0 * sigma * sigma);

    // Same-color, same-position neighbors (distance 2).
    const int2 off[8] = {
        int2(-2, 0), int2(2, 0), int2(0, -2), int2(0, 2),
        int2(-2, -2), int2(2, -2), int2(-2, 2), int2(2, 2)
    };

    float wsum  = 1.0;   // center weight
    float accum = c;
    [unroll] for (int i = 0; i < 8; ++i) {
        float n  = fetchRaw(int2(x, y) + off[i]);
        float nn = max(0.0, (n - blk) / span);
        float d  = nn - cn;
        float w  = exp(-d * d * inv2s2);
        wsum  += w;
        accum += w * n;
    }
    return uint(clamp(round(accum / wsum), 0.0, white));
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int x0 = int(tid.x) * 2;   // each thread owns a horizontal pair
    int y  = int(tid.y);
    if (x0 >= int(push.rawDim.x) || y >= int(push.rawDim.y)) return;

    uint v0 = denoisePixel(x0,     y);
    uint v1 = denoisePixel(x0 + 1, y);
    uint idx = uint(y) * push.rawDim.x + uint(x0);   // even -> word boundary
    denoisedBuf[idx >> 1] = (v0 & 0xFFFFu) | (v1 << 16);
}
