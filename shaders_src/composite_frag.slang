[[vk::binding(0, 0)]] Sampler2D inputImage;

// hlg > 0.5 while recording: the camera then delivers HLG-encoded (BT.2020)
// frames, which look washed on the SDR sRGB swapchain. Tone-map them back to
// display so the preview matches what it looked like before recording.
[[vk::push_constant]] cbuffer PC { float hlg; } pc;

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

[shader("fragment")]
float4 main(float2 uv : TEXCOORD0) : SV_Target {
    float4 s = inputImage.Sample(uv);
    float3 c = s.rgb;
    if (pc.hlg > 0.5) {
        c = linearToSrgb(hlgToLinear(c));
    }
    return float4(c, s.a);
}
