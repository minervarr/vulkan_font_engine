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
    float _pad;
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
    float4 s = inputImage.Sample(uv);
    float3 c = s.rgb;

    // The legacy single-float contract: a consumer that pushes only `hlg` and
    // leaves the rest zeroed still gets its HLG path.
    int transfer = (pc.transfer != TRANSFER_SDR) ? pc.transfer
                                                 : (pc.hlg > 0.5 ? TRANSFER_HLG : TRANSFER_SDR);

    if (transfer == TRANSFER_HLG) {
        c = linearToSrgb(hlgToLinear(c));
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
        c = nits / peak;
        // NOT sRGB-encoded: on an HDR10 swapchain the engine's OUTPUT_ENCODE
        // applies the ST 2084 OETF at the end. Encoding here as well would
        // apply the curve twice.
    }

    return float4(c, s.a);
}
