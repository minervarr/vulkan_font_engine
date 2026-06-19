// Motion-adaptive spatial-temporal Bayer denoise (RAW domain).
//
// Two stages, fused into one pass that produces a clean Bayer frame for the ISP:
//
//   Stage 1 (temporal): for a STATIC pixel, blend the current raw value with an
//   accumulated history estimate (IIR). Averaging the same static pixel over
//   time drops its noise by ~sqrt(N) while touching spatial sharpness ZERO — a
//   static edge stays razor sharp, only its noise falls. Motion is detected per
//   pixel as the local temporal difference normalized by the sensor's shot-noise
//   sigma, so noise itself never reads as motion; where motion is present the
//   blend backs off to the current frame (no ghosting).
//
//   Stage 2 (spatial): a gentle edge-preserving bilateral on the current frame
//   (the exact denoise_bayer math), used only where motion blocked Stage 1.
//
// Both history and Bayer share the SAME uint16-packed layout (2 px / uint32,
// row-major, tight rawDim.x stride), and history sites line up 1:1 with raw
// sites (same color), so no CFA bookkeeping is needed. The clean output goes to
// `denoisedBuf` (consumed by green_isp/debayer_isp unchanged); the temporal
// estimate goes to `histWrite` (ping-pong target) to accumulate next frame.

[[vk::binding(0, 0)]] StructuredBuffer<uint>   rawBuf;       // current Bayer
[[vk::binding(1, 0)]] StructuredBuffer<uint>   histRead;     // prev temporal estimate
[[vk::binding(2, 0)]] RWStructuredBuffer<uint> denoisedBuf;  // clean Bayer -> ISP
[[vk::binding(3, 0)]] RWStructuredBuffer<uint> histWrite;    // new temporal estimate

struct PushConstants {
    uint2  rawDim;
    float  noiseK;       // normalized shot-noise coefficient (matches denoise_bayer)
    float  noiseFloor;   // normalized read-noise floor
    float4 black;        // black level per 2x2 CFA position
    float4 params;       // x=white, y=staticSigma, z=temporalMax, w=dtFactor
    float4 params2;      // x=motionSigma, yzw=pad
    uint4  flags;        // x=histValid (0 on the first frame), yzw=pad
};
[[vk::push_constant]] ConstantBuffer<PushConstants> push;

int mirror(int v, int dim) {
    if (v < 0)    v = -v;
    if (v >= dim) v = 2 * dim - 2 - v;   // even reflection preserves Bayer parity
    return v;
}

// Unpack a uint16-packed sample at (x, y) from `buf` (mirror-padded).
float fetchFrom(StructuredBuffer<uint> buf, int2 p) {
    p.x = mirror(p.x, int(push.rawDim.x));
    p.y = mirror(p.y, int(push.rawDim.y));
    uint idx  = uint(p.y) * push.rawDim.x + uint(p.x);   // rawDim.x is even
    uint word = buf[idx >> 1];
    return float(((idx & 1u) != 0u) ? (word >> 16) : (word & 0xFFFFu));
}

// Same-color, same-position neighbors (distance 2) — these land on the same
// Bayer color AND 2x2 position, so the black/white normalization is shared.
static const int2 kOff[8] = {
    int2(-2, 0), int2(2, 0), int2(0, -2), int2(0, 2),
    int2(-2, -2), int2(2, -2), int2(-2, 2), int2(2, 2)
};

// Compute the clean output (fOut) and the temporal estimate (tOut) for one site.
void process(int x, int y, out uint fOut, out uint tOut) {
    float white = push.params.x;
    float blk   = push.black[(uint(y) & 1u) * 2u + (uint(x) & 1u)];
    float span  = max(1.0, white - blk);

    float c  = fetchFrom(rawBuf,   int2(x, y));
    float h  = fetchFrom(histRead, int2(x, y));
    float cn = max(0.0, (c - blk) / span);
    float hn = max(0.0, (h - blk) / span);

    float sigma  = push.noiseK * sqrt(max(cn, 1e-4)) + push.noiseFloor;
    float inv2s2 = 1.0 / (2.0 * sigma * sigma);

    // One pass over the same-color taps: build the bilateral spatial estimate
    // (Stage 2) AND the PEAK local temporal difference (motion) at once. Using
    // the max — not the mean — is what makes the gate safe: a sharp moving edge
    // touches only a few taps, so a mean dilutes it and the gate under-reads
    // motion (-> ghosting). The max instead flags the pixel as moving if ANY
    // same-color neighbour moved, which also DILATES the motion region so a
    // moving edge disables temporal denoise for the swath it sweeps (no trail).
    float wsum = 1.0;             // center weight
    float accum = c;             // center value
    float mMax  = abs(cn - hn);  // peak normalized temporal difference
    [unroll] for (int i = 0; i < 8; ++i) {
        float nr  = fetchFrom(rawBuf,   int2(x, y) + kOff[i]);
        float hr  = fetchFrom(histRead, int2(x, y) + kOff[i]);
        float nn  = max(0.0, (nr - blk) / span);
        float hnn = max(0.0, (hr - blk) / span);
        float d   = nn - cn;
        float w   = exp(-d * d * inv2s2);
        wsum  += w;
        accum += w * nr;
        mMax   = max(mMax, abs(nn - hnn));
    }
    float S = accum / wsum;          // spatial-denoised raw value (Stage 2)

    // Stage 1: temporal retention, gated decisively by motion. Below
    // staticSigma*sigma the peak difference is just noise -> full retention;
    // above motionSigma*sigma it is clearly moving -> full passthrough. The
    // smoothstep errs toward passthrough (no ghosting) at the cost of less
    // denoise on ambiguous slow/low-contrast motion.
    float motion = smoothstep(push.params.y * sigma, push.params2.x * sigma, mMax);
    float at = push.params.z * (1.0 - motion);
    at *= push.params.w;                              // dropped-frame trust factor
    if (push.flags.x == 0u) at = 0.0;                 // first frame: no valid history
    at = clamp(at, 0.0, push.params.z);

    float T = lerp(c, h, at);                         // temporal estimate (raw units)

    // Blend spatial vs temporal by how static we are: static -> T (sharp +
    // temporally denoised), motion -> S (gentle edge-preserving spatial).
    float staticConf = (push.params.z > 1e-6) ? clamp(at / push.params.z, 0.0, 1.0) : 0.0;
    float F = lerp(S, T, staticConf);

    fOut = uint(clamp(round(F), 0.0, white));
    tOut = uint(clamp(round(T), 0.0, white));
}

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int x0 = int(tid.x) * 2;   // each thread owns a horizontal pair (no pack race)
    int y  = int(tid.y);
    if (x0 >= int(push.rawDim.x) || y >= int(push.rawDim.y)) return;

    uint f0, t0, f1, t1;
    process(x0,     y, f0, t0);
    process(x0 + 1, y, f1, t1);

    uint idx = uint(y) * push.rawDim.x + uint(x0);   // even -> word boundary
    denoisedBuf[idx >> 1] = (f0 & 0xFFFFu) | (f1 << 16);
    histWrite[idx >> 1]   = (t0 & 0xFFFFu) | (t1 << 16);
}
