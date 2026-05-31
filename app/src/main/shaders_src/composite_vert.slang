struct VertexOutput {
  float4 position : SV_Position;
  float2 uv       : TEXCOORD0;
};

[shader("vertex")]
VertexOutput main(uint vertexId : SV_VertexID) {
  VertexOutput o;
  float2 uv  = float2((vertexId << 1) & 2, vertexId & 2);
  o.uv       = uv;
  o.position = float4(uv * 2.0 - 1.0, 0.0, 1.0);
  return o;
}
