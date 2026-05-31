#pragma once
#include <vulkan/vulkan.h>
#include <android/asset_manager.h>
#include <cstdint>

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

  void init(VkDevice dev, VkPhysicalDevice phyDev, AAssetManager* mgr,
            uint32_t width, uint32_t height);
  void cleanup();

  void uploadCurves(const float* curveData, uint32_t count);

  void transitionOutputImageInitial(VkCommandBuffer cmd);
  void dispatch(VkCommandBuffer cmd);

 private:
  void createDescriptorLayoutAndPool();
  void createBuffers();
  void createOutputImage();
  void createPipelines();
  void writeDescriptors();

  uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required);
  VkShaderModule loadShader(const char* path);
};
