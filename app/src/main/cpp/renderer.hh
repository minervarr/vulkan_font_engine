#pragma once
#include <vulkan/vulkan.h>
#include <android/asset_manager.h>
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
  AAssetManager*   assetManager   = nullptr;

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

  // MSDF resources
  VkImage          msdfAtlasImage   = VK_NULL_HANDLE;
  VkDeviceMemory   msdfAtlasMemory  = VK_NULL_HANDLE;
  VkImageView      msdfAtlasView    = VK_NULL_HANDLE;
  VkSampler        msdfSampler      = VK_NULL_HANDLE;
  VkBuffer         msdfStaging      = VK_NULL_HANDLE;
  VkDeviceMemory   msdfStagingMem   = VK_NULL_HANDLE;
  VkBuffer         msdfVertBuffer   = VK_NULL_HANDLE;
  VkDeviceMemory   msdfVertMemory   = VK_NULL_HANDLE;
  void*            msdfVertMapped   = nullptr;
  VkDescriptorSetLayout msdfSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool      msdfPool      = VK_NULL_HANDLE;
  VkDescriptorSet       msdfSet       = VK_NULL_HANDLE;
  VkPipelineLayout msdfPipelineLayout = VK_NULL_HANDLE;
  VkPipeline       msdfPipeline       = VK_NULL_HANDLE;
  uint32_t         msdfAtlasW = 0, msdfAtlasH = 0;
  float            msdfPxRange = 4.0f;
  uint32_t         msdfVertCount = 0;

  void init(VkDevice dev, VkPhysicalDevice phyDev, AAssetManager* mgr,
            uint32_t width, uint32_t height);
  void cleanup();

  void uploadCurves(const float* curveData, uint32_t count);

  void transitionOutputImageInitial(VkCommandBuffer cmd);
  void dispatch(VkCommandBuffer cmd);

  // ── MSDF text path ─────────────────────────────────────────────────────────
  // Crisp, cheap text via an offline MSDF atlas (Chlumsky). Glyphs are drawn as
  // textured quads in the app's render pass, bypassing the Bézier coverage pass.
  static constexpr uint32_t MAX_MSDF_VERTS  = 96000;  // 16000 glyphs * 6 verts
  static constexpr uint32_t MSDF_VERT_FLOATS = 8;     // pos.xy uv.xy rgba

  // Creates the atlas image/sampler, descriptor, vertex buffer and graphics
  // pipeline. Call after the render pass exists and the font is loaded.
  void createMsdfResources(VkRenderPass renderPass, const MsdfFont& font);
  // Records the one-time atlas upload (staging -> device image) into an init
  // command buffer that the caller submits and waits on.
  void recordAtlasUpload(VkCommandBuffer cmd);
  bool msdfReady() const { return msdfPipeline != VK_NULL_HANDLE; }
  void uploadGlyphQuads(const float* verts, uint32_t vertCount);
  uint32_t msdfVerts() const { return msdfVertCount; }
  // Draw a sub-range of the uploaded glyph quads with a scroll offset (px) and a
  // scissor rectangle (px). The geometry is static; scrolling is just the offset.
  void drawMsdfRange(VkCommandBuffer cmd, uint32_t firstVert, uint32_t vertCount,
                     float scrollX, float scrollY,
                     int32_t sx, int32_t sy, uint32_t sw, uint32_t sh);

 private:
  void createDescriptorLayoutAndPool();
  void createBuffers();
  void createOutputImage();
  void createPipelines();
  void writeDescriptors();

  uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required);
  VkShaderModule loadShader(const char* path);
};
