// Non-Local Means denoise, RAW/Bayer domain — shared-memory tiled, SPATIO-TEMPORAL.
//
// A pixel is averaged with others whose whole *neighbourhood* looks like its own,
// so repeating texture and edges survive where a bilateral would soften. Works on
// SAME-COLOUR samples only — every offset is a step of 2 in x and y, landing on
// the same Bayer colour AND 2x2 position, so no CFA bookkeeping and no colour
// cross-contamination. The weight uses the sensor's shot-noise sigma (variance ~
// signal), so shadows smooth hard while edges/highlights stay crisp.
//
// TEMPORAL (the big lever for video): the candidate set spans not just the current
// frame but a causal window of kPast recent frames (same Bayer phase, so the same
// same-colour stencil works across frames). Static content sits at the SAME
// position in past frames, so even a tight co-located search finds it — averaging
// it drops noise ~sqrt(N) with ZERO spatial softening (the only truly detail-
// lossless denoising). Moving content simply doesn't match → low weight → ignored,
// so it never ghosts; it just gets the spatial estimate. The current frame gets the
// full spatial search; each past frame a tight co-located search (kNarrowR) — cheap,
// and it captures all the temporal redundancy that matters (static/slow regions).
//
// PERF: all kPast+1 frames are loaded into a groupshared tile ONCE; every patch
// read hits on-chip memory. Radii are compile-time so loops unroll and the centre
// patch lives in registers (dynamic radii collapse to off-chip local memory).
//
// In/out are uint16-packed buffers (2 px per uint32, tight rawDim.x stride). Each
// thread owns a horizontal pixel pair so it writes a whole uint32 race-free. Output
// is the denoised CURRENT (centre) frame; everything downstream is unchanged.

[[vk::binding(0, 0)]] StructuredBuffer<uint>   rawBuf;       // current frame (Bayer)
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> denoisedBuf;  // denoised current frame
[[vk::binding(2, 0)]] StructuredBuffer<uint>   pastBuf0;     // t-1
[[vk::binding(3, 0)]] StructuredBuffer<uint>   pastBuf1;     // t-2

struct PushConstants {
    uint2  rawDim;
    float  noiseK;       // normalized shot-noise coefficient
    float  noiseFloor;   // normalized read-noise floor
    float4 black;        // black level per 2x2 CFA position
    float4 nlm;          // x=white, y/z ignored (radii static below), w=hScale
};
[[vk::push_constant]] ConstantBuffer<PushConstants> push;

// ── Tunable window (compile-time) ────────────────────────────────────────────
static const int kSearchR = 3;                  // current-frame search half-window -> 48 candidates
static const int kPatchR  = 2;                  // patch half-window -> 5x5 patch
static const int kPast    = 2;                  // past frames in the causal temporal window (must match bindings)
static const int kNarrowR = 1;                  // co-located search half-window in each past frame -> 9 candidates
static const int kFrames  = kPast + 1;          // total frames (current + past)
static const int kPatchTaps = (2 * kPatchR + 1) * (2 * kPatchR + 1);

// ── Workgroup geometry ───────────────────────────────────────────────────────
static const int GX = 8, GY = 8;
static const int OUT_W = GX * 2;                // 16 output px per group (pair/thread)
static const int OUT_H = GY;                    // 8
static const int REACH  = 2 * (kSearchR + kPatchR);   // farthest tap in px (current-frame search dominates)
static const int TILE_W = OUT_W + 2 * REACH;
static const int TILE_H = OUT_H + 2 * REACH;

groupshared float gs[kFrames][TILE_H][TILE_W];  // raw tiles: [0]=current, [1..kPast]=past

int mirror(int v, int dim) {
    if (v < 0)    v = -v;
    if (v >= dim) v = 2 * dim - 2 - v;   // even reflection preserves Bayer parity
    return v;
}

// Raw value at (x,y) in frame f (no mirror; caller passes in-range coords).
float rawAtFrame(int f, int x, int y) {
    uint idx  = uint(y) * push.rawDim.x + uint(x);
    uint word;
    if      (f == 0) word = rawBuf[idx >> 1];
    else if (f == 1) word = pastBuf0[idx >> 1];
    else             word = pastBuf1[idx >> 1];
    return float(((idx & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu));
}

// Spatio-temporal NLM for one tile pixel. (tlx,tly) = tile-local coords; (x,y) the colour.
uint nlmPixel(int tlx, int tly, int x, int y) {
    float white    = push.nlm.x;
    float hScale   = push.nlm.w;
    float blk      = push.black[(uint(y) & 1u) * 2u + (uint(x) & 1u)];
    float span     = max(1.0, white - blk);
    float invSpan2 = 1.0 / (span * span);

    float c     = gs[0][tly][tlx];
    float cn    = max(0.0, (c - blk) / span);
    float sigma = push.noiseK * sqrt(max(cn, 1e-4)) + push.noiseFloor;
    float h2    = max(1e-8, (hScale * sigma) * (hScale * sigma));
    float bias  = 2.0 * sigma * sigma;
    float invTaps = 1.0 / float(kPatchTaps);

    // Centre patch (current frame) -> registers (static indices via unrolled loops).
    float cpatch[kPatchTaps];
    [unroll] for (int py = -kPatchR; py <= kPatchR; ++py)
        [unroll] for (int px = -kPatchR; px <= kPatchR; ++px)
            cpatch[(py + kPatchR) * (2 * kPatchR + 1) + (px + kPatchR)] =
                gs[0][tly + 2 * py][tlx + 2 * px];

    float wsum = 0.0, accum = 0.0, wmax = 0.0;

    // Spatial search in the current frame (full window; skip the centre — it's the
    // self term, added at the end with the strongest weight).
    for (int dy = -kSearchR; dy <= kSearchR; ++dy) {
        for (int dx = -kSearchR; dx <= kSearchR; ++dx) {
            if (dx == 0 && dy == 0) continue;
            float ssd = 0.0;
            [unroll] for (int py = -kPatchR; py <= kPatchR; ++py)
                [unroll] for (int px = -kPatchR; px <= kPatchR; ++px) {
                    float b = gs[0][tly + 2 * (dy + py)][tlx + 2 * (dx + px)];
                    float d = cpatch[(py + kPatchR) * (2 * kPatchR + 1) + (px + kPatchR)] - b;
                    ssd += d * d;
                }
            ssd *= invTaps * invSpan2;
            float wgt = exp(-max(0.0, ssd - bias) / h2);
            wmax  = max(wmax, wgt);
            wsum  += wgt;
            accum += wgt * gs[0][tly + 2 * dy][tlx + 2 * dx];
        }
    }

    // Temporal: tight co-located search in each past frame. The (0,0) tap is kept
    // (it's the static-content match that drives the sqrt(N) noise drop).
    [unroll] for (int f = 1; f < kFrames; ++f) {
        for (int dy = -kNarrowR; dy <= kNarrowR; ++dy) {
            for (int dx = -kNarrowR; dx <= kNarrowR; ++dx) {
                float ssd = 0.0;
                [unroll] for (int py = -kPatchR; py <= kPatchR; ++py)
                    [unroll] for (int px = -kPatchR; px <= kPatchR; ++px) {
                        float b = gs[f][tly + 2 * (dy + py)][tlx + 2 * (dx + px)];
                        float d = cpatch[(py + kPatchR) * (2 * kPatchR + 1) + (px + kPatchR)] - b;
                        ssd += d * d;
                    }
                ssd *= invTaps * invSpan2;
                float wgt = exp(-max(0.0, ssd - bias) / h2);
                wmax  = max(wmax, wgt);
                wsum  += wgt;
                accum += wgt * gs[f][tly + 2 * dy][tlx + 2 * dx];
            }
        }
    }

    float cw = max(wmax, 1e-3);                          // self-weight = best match found
    wsum  += cw;
    accum += cw * c;
    return uint(clamp(round(accum / wsum), 0.0, white));
}

[numthreads(8, 8, 1)]
void main(uint3 gid  : SV_GroupID,
          uint3 gtid : SV_GroupThreadID,
          uint  lidx : SV_GroupIndex)
{
    int baseX = int(gid.x) * OUT_W - REACH;
    int baseY = int(gid.y) * OUT_H - REACH;
    // Cooperatively load all kFrames tiles into shared memory.
    int per = TILE_W * TILE_H;
    for (int i = int(lidx); i < kFrames * per; i += GX * GY) {
        int f  = i / per;
        int r  = i - f * per;
        int ty = r / TILE_W;
        int tx = r - ty * TILE_W;
        gs[f][ty][tx] = rawAtFrame(f, mirror(baseX + tx, int(push.rawDim.x)),
                                      mirror(baseY + ty, int(push.rawDim.y)));
    }
    GroupMemoryBarrierWithGroupSync();

    int x0 = int(gid.x) * OUT_W + int(gtid.x) * 2;
    int y  = int(gid.y) * OUT_H + int(gtid.y);
    if (y >= int(push.rawDim.y)) return;                 // safe: barrier already passed

    int tlx = x0 - baseX;
    int tly = y  - baseY;
    uint v0 = (x0     < int(push.rawDim.x)) ? nlmPixel(tlx,     tly, x0,     y) : 0u;
    uint v1 = (x0 + 1 < int(push.rawDim.x)) ? nlmPixel(tlx + 1, tly, x0 + 1, y) : 0u;
    uint idx = uint(y) * push.rawDim.x + uint(x0);
    denoisedBuf[idx >> 1] = (v0 & 0xFFFFu) | (v1 << 16);
}
