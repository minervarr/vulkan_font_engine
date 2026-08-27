[[vk::binding(0, 0)]] Sampler2D inputImage;

// hlg > 0.5 while recording: the camera then delivers HLG-encoded (BT.2020)
// frames, which look washed on the SDR sRGB swapchain. Tone-map them back to
// display so the preview matches what it looked like before recording.
// zoom > 1 draws the focus-check loupe: the same frame magnified about
// (cx, cy) in UV space, so an inset viewport can show real sensor detail
// while the main pass keeps the wide view. zoom <= 1 is the normal pass.
//
// peak > 0 enables focus peaking: pixels sitting on a hard luminance edge are
// painted a flat colour, so what is critically sharp is unmistakable. It is a
// gradient detector, so it is only as honest as the texture it runs on — the
// texel steps below are the BOUND IMAGE's, not the screen's, which is what
// makes peaking inside the magnified inset (a full-sensor stream) trustworthy.
[[vk::push_constant]] cbuffer PC {
    float hlg; float zoom; float cx; float cy;
    float peak; float texelX; float texelY; float peakColor;
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

// Linear -> sRGB (display) transfer.
float3 linearToSrgb(float3 l) {
    float3 o;
    [unroll] for (int i = 0; i < 3; ++i) {
        float x = saturate(l[i]);
        o[i] = (x <= 0.0031308) ? (12.92 * x) : (1.055 * pow(x, 1.0 / 2.4) - 0.055);
    }
    return o;
}

float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

[shader("fragment")]
float4 main(float2 uv : TEXCOORD0) : SV_Target {
    if (pc.zoom > 1.0) {
        const float2 c = float2(pc.cx, pc.cy);
        uv = c + (uv - c) / pc.zoom;
    }
    float4 s = inputImage.Sample(uv);
    float3 c = s.rgb;
    if (pc.hlg > 0.5) {
        c = linearToSrgb(hlgToLinear(c));
    }

    if (pc.peak > 0.0) {
        // Forward differences on luma, one texel apart. Two taps rather than a
        // full Sobel: at preview rates the cheaper detector is indistinguishable
        // in use, and an in-focus edge is high-contrast in SOME direction, which
        // the sum of both axes captures.
        float l0 = luma(c);
        float lx = luma(inputImage.Sample(uv + float2(pc.texelX, 0.0)).rgb);
        float ly = luma(inputImage.Sample(uv + float2(0.0, pc.texelY)).rgb);
        float g  = abs(l0 - lx) + abs(l0 - ly);
        if (g > pc.peak) {
            // Flat, fully saturated, and nothing like a real scene colour, so
            // it reads as an overlay rather than as part of the image.
            float3 pk = (pc.peakColor > 0.5) ? float3(0.0, 1.0, 0.2)
                                             : float3(1.0, 0.1, 0.2);
            c = pk;
        }
    }
    return float4(c, s.a);
}
