#include "output_encode.slang"

[[vk::binding(0, 0)]] Sampler2D inputImage;

// The external image arrives already in RGB: the sampler's
// VkSamplerYcbcrConversion applied the Y'CbCr matrix (BT.601/709/2020,
// limited or full range) before this stage ever ran. What is left here is the
// TRANSFER function, which a sampler cannot do.
//
// `transfer` selects it. The two non-SDR paths exist for opposite reasons:
// HLG because the camera delivers HLG-encoded BT.2020 while recording, and PQ
// because an HDR10 video file is PQ-encoded. Both look washed out or dark when
// handed to a display expecting something else, and both failures read as "the
// preview is broken" rather than as a colour bug.
//
// `peakNits` is the display's peak luminance for the PQ path's tone map, and
// is ignored otherwise.
static const int TRANSFER_SDR = 0;
static const int TRANSFER_HLG = 1;
static const int TRANSFER_PQ  = 2;

[[vk::push_constant]] cbuffer PC {
    // Kept as the FIRST float and kept meaning "HLG when > 0.5", because the
    // camera consumer pushes exactly this one float and must not have to
    // change. A zero-initialized push block is therefore SDR, which is the
    // behaviour every existing consumer already had.
    float hlg;
    int   transfer;
    float peakNits;
    float _pad0;

    // How much of the sampled image is actually PICTURE.
    //
    // A hardware decoder allocates its buffers aligned up to whatever its
    // hardware wants — a 2040x1530 video lands in a 2048x1536 buffer — and the
    // 8 columns and 6 rows past the edge hold whatever the codec left there.
    // Sampling 0..1 stretches that debris across the last millimetre of the
    // right and bottom edges, which is precisely the artefact this fixes.
    //
    // Applied to the FINAL uv, in texture space, so it is unaffected by the
    // rotation composite_vert applies for the camera.
    //
    // 0 means "not set" and is read as 1: a zero-initialized push block must
    // keep meaning "sample the whole image" for consumers that never set it.
    float2 uvScale;
    float2 _pad1;
} pc;

// HLG inverse-OETF: encoded signal [0,1] -> scene-linear [0,1].
float3 hlgToLinear(float3 e) {
    const float a = 0.17883277;
    const float b = 0.28466892;
    const float c = 0.55991073;
    float3 o;
    [unroll] for (int i = 0; i < 3; ++i) {
        float x = saturate(e[i]);
        o[i] = (x <= 0.5) ? (x * x / 3.0) : ((exp((x - c) / a) + b) / 12.0);
    }
    return o;
}

// ST 2084 EOTF: encoded signal -> absolute luminance, normalized so 1.0 is
// 10000 cd/m^2. This is the INVERSE of the OETF an HDR10 swapchain's output
// encode applies; the two are different functions and swapping them yields a
// picture that is plausibly wrong rather than obviously wrong.
float3 pqToLinear(float3 e) {
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;

    float3 ep  = pow(max(e, 0.0), 1.0 / m2);
    float3 num = max(ep - c1, 0.0);
    float3 den = max(c2 - c3 * ep, 1e-6);
    return pow(num / den, 1.0 / m1);
}

// Linear -> sRGB (display) transfer.
float3 linearToSrgb(float3 l) {
    float3 o;
    [unroll] for (int i = 0; i < 3; ++i) {
        float x = saturate(l[i]);
        o[i] = (x <= 0.0031308) ? (12.92 * x) : (1.055 * pow(x, 1.0 / 2.4) - 0.055);
    }
    return o;
}

[shader("fragment")]
float4 main(float2 uv : TEXCOORD0) : SV_Target {
    float2 scale = float2(pc.uvScale.x > 0.0 ? pc.uvScale.x : 1.0,
                          pc.uvScale.y > 0.0 ? pc.uvScale.y : 1.0);
    float4 s = inputImage.Sample(uv * scale);
    float3 c = s.rgb;

    // The legacy single-float contract: a consumer that pushes only `hlg` and
    // leaves the rest zeroed still gets its HLG path.
    int transfer = (pc.transfer != TRANSFER_SDR) ? pc.transfer
                                                 : (pc.hlg > 0.5 ? TRANSFER_HLG : TRANSFER_SDR);

    if (transfer == TRANSFER_HLG) {
        c = linearToSrgb(hlgToLinear(c));
        // ...and then fall through to the encode below, which is the identity
        // on the SDR swapchain this path has always run on.
        return float4(encodeUiColor(c), s.a);
    } else if (transfer == TRANSFER_PQ) {
        // To absolute luminance, then into the display's range. Values above
        // what the panel can show are compressed rather than clipped: clipping
        // specular highlights is what makes HDR content read as flat and grey.
        float peak = max(pc.peakNits, 1.0);
        float3 nits = pqToLinear(c) * 10000.0;
        // Reinhard with a knee at the display peak. Deliberately the simplest
        // curve that is monotonic and preserves hue ordering; a better
        // operator is a decision to make against real content on a real panel,
        // not a default to guess at here.
        nits = nits / (1.0 + nits / peak);

        // Hand LINEAR light to the engine's own encode, with 1.0 meaning
        // `peak` nits. THIS is what was missing and what made an HDR10 file
        // render as a black screen: output_encode.slang is included by
        // image/shape/overlay/msdf and was never included here, so this stage
        // wrote linear values into a swapchain whose colourspace says they are
        // PQ code values. The panel then reads 0.1 linear as about 0.6 nits,
        // and the whole picture collapses into black with no error anywhere.
        return float4(encodeImageLinear(nits / peak, peak), s.a);
    }

    // SDR content. On an SDR swapchain encodeUiColor() is the identity, so the
    // camera preview stays byte-for-byte what it was. On an HDR swapchain it
    // carries the sRGB numbers into the target's space instead of leaving them
    // to be misread, which is the same bug as above in its milder form.
    return float4(encodeUiColor(c), s.a);
}
