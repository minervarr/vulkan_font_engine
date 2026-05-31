[[vk::binding(0, 0)]] Sampler2D inputImage;

[shader("fragment")]
float4 main(float2 uv : TEXCOORD0) : SV_Target {
  return inputImage.Sample(uv);
}
