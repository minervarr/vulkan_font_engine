#pragma once
// MSDF text renderer: uploads an MsdfFont's atlas (one texture + descriptor
// set per font weight) and draws pre-laid-out glyph quads with a single
// alpha-blended graphics pipeline. One of the engine's two GPU units; the
// curve/coverage compute path lives in curve_rasterizer.hh.

#include "asset_reader.hh"
#include <vulkan/vulkan.h>
#include <cstdint>

class MsdfFont;

class MsdfTextRenderer {
 public:
  static constexpr uint32_t MAX_MSDF_VERTS   = 96000; // total verts for all weights
  static constexpr uint32_t MSDF_VERT_FLOATS = 8;     // pos.xy uv.xy rgba
  static constexpr int      MAX_FONT_WEIGHTS = 4;     // Regular/Bold/Italic/BoldItalic

  // Stores device/asset/extent state only; GPU resources are created by
  // createResources() once a render pass and font are available.
  void init(VkDevice device, VkPhysicalDevice physicalDevice,
            AssetReader& assets, uint32_t screenWidth, uint32_t screenHeight);
  void cleanup();

  // Creates atlas + descriptor for one weight. Shared pipeline/vertex-buffer/
  // layout are created on the first call (weightIdx==0 or whichever is first).
  void createResources(VkRenderPass renderPass, const MsdfFont& font, int weightIdx = 0);
  // Records atlas upload (staging → device image) for a specific weight.
  void recordAtlasUpload(VkCommandBuffer cmd, int weightIdx = 0);
  bool ready(int weightIdx = 0) const {
    return pipeline_ != VK_NULL_HANDLE &&
           atlas_image_[weightIdx] != VK_NULL_HANDLE;
  }
  void uploadGlyphQuads(const float* verts, uint32_t vertCount, int weightIdx = 0);
  uint32_t vertCount(int weightIdx = 0) const { return vert_count_[weightIdx]; }
  // Vertex offset for weight w: w * (MAX_MSDF_VERTS / MAX_FONT_WEIGHTS)
  uint32_t vertOffset(int w) const {
    return (uint32_t)w * (MAX_MSDF_VERTS / (uint32_t)MAX_FONT_WEIGHTS);
  }
  void draw(VkCommandBuffer cmd, uint32_t firstVert, uint32_t vertCount,
            float scrollX, float scrollY,
            int32_t sx, int32_t sy, uint32_t sw, uint32_t sh,
            int weightIdx = 0);

 private:
  VkDevice         device_          = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  AssetReader*     assets_          = nullptr;
  uint32_t         screen_width_    = 0;
  uint32_t         screen_height_   = 0;

  // Per-weight atlas resources (one atlas texture + descriptor set per weight)
  VkImage          atlas_image_ [MAX_FONT_WEIGHTS] = {};
  VkDeviceMemory   atlas_memory_[MAX_FONT_WEIGHTS] = {};
  VkImageView      atlas_view_  [MAX_FONT_WEIGHTS] = {};
  VkSampler        sampler_     [MAX_FONT_WEIGHTS] = {};
  VkBuffer         staging_     [MAX_FONT_WEIGHTS] = {};
  VkDeviceMemory   staging_mem_ [MAX_FONT_WEIGHTS] = {};
  VkDescriptorSet  set_         [MAX_FONT_WEIGHTS] = {};
  uint32_t         atlas_w_     [MAX_FONT_WEIGHTS] = {};
  uint32_t         atlas_h_     [MAX_FONT_WEIGHTS] = {};
  float            px_range_    [MAX_FONT_WEIGHTS] = {};
  uint32_t         vert_count_  [MAX_FONT_WEIGHTS] = {};

  // Shared across all weights (one pipeline, one vertex buffer, one layout)
  VkBuffer              vert_buffer_     = VK_NULL_HANDLE;
  VkDeviceMemory        vert_memory_     = VK_NULL_HANDLE;
  void*                 vert_mapped_     = nullptr;
  VkDescriptorSetLayout set_layout_      = VK_NULL_HANDLE;
  VkDescriptorPool      pool_            = VK_NULL_HANDLE;
  VkPipelineLayout      pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline            pipeline_        = VK_NULL_HANDLE;
};
