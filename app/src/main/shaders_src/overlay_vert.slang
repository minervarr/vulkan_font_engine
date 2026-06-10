// Fullscreen-triangle vertex shader for compositing a screen-space overlay
// image (identity UVs — unlike composite_vert which rotates the camera UVs).
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
