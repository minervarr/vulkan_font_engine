// MSDF/MTSDF text: reconstructs a crisp, anti-aliased glyph edge from the
// multi-channel signed distance field (median of RGB), per Chlumsky. When the
// atlas is MTSDF (alpha = TRUE single-channel SDF; push.mtsdf == 1), heavily
// minified glyphs blend toward that true SDF — median(RGB) develops channel-
// conflict speckles below atlas resolution, the true SDF never does.
struct Push {
  float2 screen;
  float  pxRange;
  float  mtsdf;      // 1 = alpha is a true SDF (runtime MTSDF atlas), else 0
  float2 atlasSize;
  float2 scroll;     // (unused in fragment; keeps push layout identical)
};
[[vk::push_constant]] ConstantBuffer<Push> push;

[[vk::binding(0, 0)]] Sampler2D msdf;

float median(float r, float g, float b) {
  return max(min(r, g), min(max(r, g), b));
}

struct PSIn {
  float2 uv    : TEXCOORD0;
  float4 color : COLOR0;
};

[shader("fragment")]
float4 main(PSIn i) : SV_Target {
  float4 msd = msdf.Sample(i.uv);
  float  sd  = median(msd.r, msd.g, msd.b);

  // Canonical Chlumsky screen-px-range: convert the distance field's texel range to
  // the number of screen pixels it spans AT THIS fragment's scale, so the edge stays
  // exactly 1px-soft (razor sharp) at any zoom. `unitRange` is the field range in uv
  // units; `screenTexSize` is screen px per uv unit (from the per-axis derivative).
  // The 0.5*dot averages the two axes; the max(...,1) floor avoids over-sharpening
  // (shimmer) when the glyph is minified below the atlas resolution.
  float2 unitRange     = push.pxRange / push.atlasSize;
  float2 screenTexSize = 1.0 / max(fwidth(i.uv), 1e-8);
  float  rawRange      = 0.5 * dot(unitRange, screenTexSize);
  float  screenPxRange = max(rawRange, 1.0);

  // MTSDF small-size cleanup: once the field spans under ~2 screen px the
  // median's channel conflicts show as speckles; the alpha channel's true
  // SDF is artifact-free (its only cost — slightly rounded corners — is
  // invisible this small). Gated on push.mtsdf so pre-baked RGB-only
  // atlases (constant alpha) stay pixel-exact.
  //
  // Dense thin-stroked glyphs (CJK/Hangul/Kana) develop median channel-
  // conflict blobs in their margins over a much wider minification range
  // than the <2px threshold covers — visible as faint smudges that flicker
  // with sub-pixel scroll phase. The host tags those glyphs by emitting
  // vertex alpha just under 1.0 (see the emitter's kTrueSdfAlphaTag); they
  // render from the true SDF outright. Untagged (Latin) fragments keep the
  // original formula bit-exactly.
  bool trueSdfTagged = i.color.a < 0.9995;
  float t = push.mtsdf * (trueSdfTagged ? 1.0 : clamp(2.0 - rawRange, 0.0, 1.0));
  sd = lerp(sd, msd.a, t);

  float screenPxDist = screenPxRange * (sd - 0.5);
  float a = clamp(screenPxDist + 0.5, 0.0, 1.0);
  float cov = i.color.a * a;

  // ── Gamma-correct coverage blending ──────────────────────────────────────
  //
  // The swapchain is R8G8B8A8_UNORM presented as SRGB_NONLINEAR: the bytes we
  // write ARE sRGB-encoded, but fixed-function blending operates on them as if
  // they were linear. So the ordinary SRC_ALPHA blend mixes coverage in gamma
  // space, which is simply wrong — and wrong in a direction that is very
  // visible for a dark theme with light text.
  //
  // Concretely, light text (sRGB 0.9) at 50% coverage over the app's near-black
  // background: gamma-space blending yields sRGB 0.485, while correct
  // linear-light blending yields sRGB 0.66. Every antialiased edge pixel comes
  // out too dark, which reads as text that is too THIN.
  //
  // Fix: output PREMULTIPLIED colour that has already been composited in linear
  // light, and let the pipeline add it (srcColorBlendFactor is ONE — see
  // createMsdfPipeline in renderer.cc; the two must change together).
  //
  //   correct = srgbEncode(linear(colour) * coverage)
  //
  // This is EXACT when the destination is black and degrades smoothly as the
  // destination brightens, because the destination's own share is still
  // attenuated by (1 - cov) in gamma space. That assumption is a very good fit
  // here: the theme background is ~#0d0d0d, i.e. 0.4% linear — essentially
  // black. It is least accurate for text drawn over bright album art.
  //
  // The exact alternative is a VK_KHR_swapchain_mutable_format sRGB *view* of
  // the same swapchain image, which would make the hardware blend in linear
  // light against the real destination. That needs its own render pass and
  // framebuffers for the text draw alone; deliberately not done here.
  // select() rather than ?: — these are componentwise vector conditions, and
  // Slang deprecates the non-short-circuiting ?: for exactly this use.
  float3 lin = select(i.color.rgb <= 0.04045,
                      i.color.rgb / 12.92,
                      pow((i.color.rgb + 0.055) / 1.055, 2.4));
  float3 blended = lin * cov;
  float3 outRgb = select(blended <= 0.0031308,
                         blended * 12.92,
                         1.055 * pow(blended, 1.0 / 2.4) - 0.055);
  return float4(outRgb, cov);
}
