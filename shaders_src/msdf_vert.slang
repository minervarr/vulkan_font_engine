// MSDF text: transforms screen-pixel glyph quads into clip space.
struct Push {
  float2 screen;     // framebuffer size in px
  float  pxRange;    // atlas distance range (px)
  float  mtsdf;      // 1 = alpha channel is a true SDF (MTSDF atlas); was pad
  float2 atlasSize;  // atlas texture size in px
  float2 scroll;     // px offset applied to vertex positions (scroll the layer)
};
[[vk::push_constant]] ConstantBuffer<Push> push;

struct VSIn {
  float2 pos   : POSITION;    // location 0: screen px (y down)
  float2 uv    : TEXCOORD0;   // location 1
  float4 color : COLOR0;      // location 2
  float  page  : TEXCOORD1;   // location 3: atlas array layer
};

struct VSOut {
  float4 position : SV_Position;
  float2 uv       : TEXCOORD0;
  float4 color    : COLOR0;
  // Flat, and an integer. All six vertices of a quad carry the same page, but
  // interpolating a constant is still a float sum of barycentric terms and can
  // land a ULP off the value that went in — and SPIR-V *requires* an integer
  // fragment input to be Flat, so declaring it this way is the rule enforcing
  // itself rather than a comment asking to be believed.
  nointerpolation uint layer : TEXCOORD1;
};

[shader("vertex")]
VSOut main(VSIn i) {
  VSOut o;
  float2 pos = i.pos + push.scroll;              // scroll handled on the GPU
  float2 ndc = pos / push.screen * 2.0 - 1.0;    // Vulkan NDC: y points down
  o.position = float4(ndc, 0.0, 1.0);
  o.uv       = i.uv;
  o.color    = i.color;
  o.layer    = (uint)(i.page + 0.5);   // the attribute is a float; the layer is not
  return o;
}
