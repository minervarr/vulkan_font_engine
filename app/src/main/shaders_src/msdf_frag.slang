// MSDF text: reconstructs a crisp, anti-aliased glyph edge from the multi-channel
// signed distance field (median of RGB), per Chlumsky.
struct Push {
  float2 screen;
  float  pxRange;
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
  float3 msd = msdf.Sample(i.uv).rgb;
  float  sd  = median(msd.r, msd.g, msd.b);

  // Convert the signed distance to screen-pixel coverage. unitRange is the
  // distance range expressed in texture-UV units; screenTexSize is how many
  // atlas texels map to one screen pixel here (via derivatives).
  float2 unitRange     = float2(push.pxRange, push.pxRange) / push.atlasSize;
  float2 screenTexSize = float2(1.0, 1.0) / fwidth(i.uv);
  float  screenPxRange = max(0.5 * dot(unitRange, screenTexSize), 1.0);

  float screenPxDist = screenPxRange * (sd - 0.5);
  float a = clamp(screenPxDist + 0.5, 0.0, 1.0);
  return float4(i.color.rgb, i.color.a * a);
}
