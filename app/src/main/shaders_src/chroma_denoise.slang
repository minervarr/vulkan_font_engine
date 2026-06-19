// Chroma-only spatial denoise on the P010 output, run after debayer_isp.slang
// when chroma denoise (CD) is enabled.
//
// In CD mode debayer_isp.slang writes the per-quad LUMA into out_buf exactly as
// on the no-CD path, but diverts each subsampled (Cb,Cr) word into a separate
// tightly-packed chroma scratch buffer instead of the final frame. This pass
// reads a neighborhood of that scratch, runs a luma-guided (joint-bilateral)
// average over chroma ONLY, and writes the result into out_buf's interleaved
// CbCr plane.
//
// The luma (Y) plane of out_buf is never touched here, so luminance detail is
// bit-for-bit identical to the no-CD path. What gets cleaned is colour noise —
// which the eye resolves poorly (so this costs little perceived detail) and
// which the encoder spends real bitrate on (so the file shrinks). The luma
// guidance keeps the average from smearing chroma across object boundaries
// (where the colour edge almost always coincides with a luma edge).

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> outP010;   // P010: Y plane + interleaved CbCr
[[vk::binding(1, 0)]] StructuredBuffer<uint>   chromaIn;  // tight (Cb,Cr) per site (noisy)

struct PushConstants {
    uint2  outDim;          // output WxH (even)
    uint   stridePixels;    // Y stride in pixels (uint16 units, even)
    uint   uvWordOffset;    // uint32 word index where the CbCr plane starts
    uint   chromaStride;    // chroma scratch stride in words (= outDim.x/2)
    int    radius;          // half window, in chroma sites
    float  sigmaS;          // spatial gaussian sigma (sites)
    float  sigmaL;          // luma range sigma (10-bit luma units)
};
[[vk::push_constant]] ConstantBuffer<PushConstants> push;

// Representative luma of chroma site (cx, cy): the two luma of the top row of
// its 2x2 quad, averaged. Cheap; used only for the edge-stopping weight, so a
// site-consistent estimate matters more than sub-LSB accuracy.
float siteLuma(int cx, int cy) {
    uint x = uint(cx) * 2u;
    uint y = uint(cy) * 2u;
    uint w = outP010[(y * push.stridePixels + x) / 2u];
    float l0 = float((w >> 6)  & 0x3FFu);   // P010: 10 bits in the high bits
    float l1 = float((w >> 22) & 0x3FFu);
    return 0.5 * (l0 + l1);
}

int clampi(int v, int lo, int hi) { return max(lo, min(hi, v)); }

[numthreads(8, 8, 1)]
void main(uint3 tid : SV_DispatchThreadID)
{
    int cw = int(push.outDim.x) / 2;   // chroma plane width  (sites)
    int ch = int(push.outDim.y) / 2;   // chroma plane height (sites)
    int cx = int(tid.x);
    int cy = int(tid.y);
    if (cx >= cw || cy >= ch) return;

    float Lc    = siteLuma(cx, cy);
    int   R     = push.radius;
    float invS2 = 1.0 / (2.0 * push.sigmaS * push.sigmaS);
    float invL2 = 1.0 / (2.0 * push.sigmaL * push.sigmaL);

    float wSum = 0.0, cbSum = 0.0, crSum = 0.0;
    for (int dy = -R; dy <= R; ++dy) {
        int ny = clampi(cy + dy, 0, ch - 1);
        for (int dx = -R; dx <= R; ++dx) {
            int nx = clampi(cx + dx, 0, cw - 1);
            uint  cword = chromaIn[uint(ny) * push.chromaStride + uint(nx)];
            float cb = float((cword >> 6)  & 0x3FFu);   // 10-bit, centred at 512
            float cr = float((cword >> 22) & 0x3FFu);
            float Ln = siteLuma(nx, ny);
            float ds = float(dx * dx + dy * dy);
            float dl = Lc - Ln;
            float wgt = exp(-ds * invS2 - dl * dl * invL2);
            wSum  += wgt;
            cbSum += wgt * cb;
            crSum += wgt * cr;
        }
    }

    uint cb = uint(clamp(round(cbSum / wSum), 0.0, 1023.0));
    uint cr = uint(clamp(round(crSum / wSum), 0.0, 1023.0));
    uint uvWord = push.uvWordOffset + (uint(cy) * push.stridePixels + uint(cx) * 2u) / 2u;
    outP010[uvWord] = (cb << 6) | (cr << 22);   // P010 pack (matches debayer_isp)
}
