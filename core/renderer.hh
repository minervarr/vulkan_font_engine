#pragma once
#include "asset_loader.hh"
#include <vulkan/vulkan.h>
#include <cstdint>

class MsdfFont;

struct Renderer {
  static constexpr uint32_t MAX_CURVES          = 8192;
  static constexpr uint32_t CURVE_FLOATS        = 20;
  static constexpr uint32_t TILE_SIZE           = 16;
  static constexpr uint32_t MAX_CURVES_PER_TILE = 64;
  static constexpr uint32_t TILE_STRIDE_U32     = MAX_CURVES_PER_TILE + 1;
  // Winding-tile buffer: 2D grid (tileCountX × tileCountY). Each winding curve
  // (types 4/5/6) registers in exactly one tile per tile-row it spans — the
  // RIGHTMOST X overlap. The coverage shader walks tile columns from the
  // pixel's own column rightward, so curves whose rightmost tile is left of the
  // pixel are out of reach of the rightward ray and skipped for free.
  static constexpr uint32_t MAX_WINDING_PER_TILE = 64;
  static constexpr uint32_t WIND_STRIDE_U32      = MAX_WINDING_PER_TILE + 1;

  VkDevice         device         = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  AssetLoader      assetLoader;

  uint32_t screenWidth  = 0;
  uint32_t screenHeight = 0;
  uint32_t curveCount   = 0;

  VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool      descriptorPool      = VK_NULL_HANDLE;
  VkDescriptorSet       descriptorSet       = VK_NULL_HANDLE;

  VkBuffer       curveBuffer       = VK_NULL_HANDLE;
  VkDeviceMemory curveBufferMemory = VK_NULL_HANDLE;
  void*          curveBufferMapped = nullptr;
  VkBuffer       tileBuffer        = VK_NULL_HANDLE;
  VkDeviceMemory tileBufferMemory  = VK_NULL_HANDLE;
  VkBuffer       rowBuffer         = VK_NULL_HANDLE;
  VkDeviceMemory rowBufferMemory   = VK_NULL_HANDLE;

  VkImage        outputImage       = VK_NULL_HANDLE;
  VkDeviceMemory outputImageMemory = VK_NULL_HANDLE;
  VkImageView    outputImageView   = VK_NULL_HANDLE;
  VkSampler      outputSampler     = VK_NULL_HANDLE;

  VkPipelineLayout computePipelineLayout = VK_NULL_HANDLE;
  VkPipeline       tilingPipeline        = VK_NULL_HANDLE;
  VkPipeline       coveragePipeline      = VK_NULL_HANDLE;

  // ── MSDF text path ─────────────────────────────────────────────────────────
  static constexpr uint32_t MAX_MSDF_VERTS   = 96000; // total verts for all weights
  static constexpr uint32_t MSDF_VERT_FLOATS = 8;     // pos.xy uv.xy rgba
  static constexpr int      MAX_FONT_WEIGHTS = 4;      // Regular/Bold/Italic/BoldItalic

  // Per-weight atlas resources (one atlas texture + descriptor set per weight)
  VkImage          msdfAtlasImage [MAX_FONT_WEIGHTS] = {};
  VkDeviceMemory   msdfAtlasMemory[MAX_FONT_WEIGHTS] = {};
  VkImageView      msdfAtlasView  [MAX_FONT_WEIGHTS] = {};
  VkSampler        msdfSampler    [MAX_FONT_WEIGHTS] = {};
  VkBuffer         msdfStaging    [MAX_FONT_WEIGHTS] = {};
  VkDeviceMemory   msdfStagingMem [MAX_FONT_WEIGHTS] = {};
  VkDescriptorSet  msdfSet        [MAX_FONT_WEIGHTS] = {};
  uint32_t         msdfAtlasW     [MAX_FONT_WEIGHTS] = {};
  uint32_t         msdfAtlasH     [MAX_FONT_WEIGHTS] = {};
  float            msdfPxRange    [MAX_FONT_WEIGHTS] = {};
  uint32_t         msdfVertCount  [MAX_FONT_WEIGHTS] = {};

  // Shared across all weights (one pipeline, one vertex buffer, one layout)
  VkBuffer         msdfVertBuffer      = VK_NULL_HANDLE;
  VkDeviceMemory   msdfVertMemory      = VK_NULL_HANDLE;
  void*            msdfVertMapped      = nullptr;
  VkDescriptorSetLayout msdfSetLayout  = VK_NULL_HANDLE;
  VkDescriptorPool      msdfPool       = VK_NULL_HANDLE;
  VkPipelineLayout msdfPipelineLayout  = VK_NULL_HANDLE;
  VkPipeline       msdfPipeline        = VK_NULL_HANDLE;

  // Vertex offset for weight w: w * (MAX_MSDF_VERTS / MAX_FONT_WEIGHTS)
  uint32_t msdfVertOffset(int w) const {
      return (uint32_t)w * (MAX_MSDF_VERTS / (uint32_t)MAX_FONT_WEIGHTS);
  }

  void init(VkDevice dev, VkPhysicalDevice phyDev, AssetLoader loader,
            uint32_t width, uint32_t height);
  void cleanup();

  void uploadCurves(const float* curveData, uint32_t count);
  void transitionOutputImageInitial(VkCommandBuffer cmd);
  void dispatch(VkCommandBuffer cmd);

  // Creates atlas + descriptor for one weight. Shared pipeline/vertex-buffer/layout
  // are created on the first call (weightIdx==0 or whichever is first).
  void createMsdfResources(VkRenderPass renderPass, const MsdfFont& font, int weightIdx = 0);
  // Records atlas upload (staging → device image) for a specific weight.
  void recordAtlasUpload(VkCommandBuffer cmd, int weightIdx = 0);
  bool msdfReady(int weightIdx = 0) const { return msdfPipeline != VK_NULL_HANDLE &&
                                                    msdfAtlasImage[weightIdx] != VK_NULL_HANDLE; }
  void uploadGlyphQuads(const float* verts, uint32_t vertCount, int weightIdx = 0);
  uint32_t msdfVerts(int weightIdx = 0) const { return msdfVertCount[weightIdx]; }
  void drawMsdfRange(VkCommandBuffer cmd, uint32_t firstVert, uint32_t vertCount,
                     float scrollX, float scrollY,
                     int32_t sx, int32_t sy, uint32_t sw, uint32_t sh,
                     int weightIdx = 0);

 private:
  void createDescriptorLayoutAndPool();
  void createBuffers();
  void createOutputImage();
  void createPipelines();
  void writeDescriptors();

  uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required);
  VkShaderModule loadShader(const char* path);
};
