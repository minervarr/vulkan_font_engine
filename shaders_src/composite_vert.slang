struct VertexOutput {
  float4 position : SV_Position;
  float2 uv       : TEXCOORD0;
};

[shader("vertex")]
VertexOutput main(uint vertexId : SV_VertexID) {
  VertexOutput o;
  float2 uv  = float2((vertexId << 1) & 2, vertexId & 2);
  // The rotation used to live HERE, hardcoded to the 90 degrees an Android
  // camera preview needs. It moved to the fragment stage, where it is a push
  // constant, because a video file's rotation comes from its container and is
  // any of the four quadrants — including none at all.
  o.uv       = uv;
  o.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
  return o;
}
