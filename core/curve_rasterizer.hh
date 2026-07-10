#pragma once
// GPU curve rasterizer: uploads 20-float CurveRecords and rasterizes them with
// a two-pass compute pipeline (tiling assigns curves to 16×16 tiles, coverage
// integrates each tile) into an R8G8B8A8 output image the host composites.
// One of the engine's two GPU units; MSDF text lives in msdf_renderer.hh.

#include "asset_reader.hh"
#include <vulkan/vulkan.h>
#include <cstdint>

class CurveRasterizer {
 public:
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
  // See shaders_src/coverage.slang's MAX_PER_WIND_TILE comment: raised from
  // 64 after tools/coverage_test proved silent registration drops past
  // capacity can flip winding parity for a busy tile-row cell.
  static constexpr uint32_t MAX_WINDING_PER_TILE = 256;
  static constexpr uint32_t WIND_STRIDE_U32      = MAX_WINDING_PER_TILE + 1;

  void init(VkDevice device, VkPhysicalDevice physicalDevice,
            AssetReader& assets, uint32_t width, uint32_t height);
  void cleanup();

  void uploadCurves(const float* curveData, uint32_t count);
  void transitionOutputImageInitial(VkCommandBuffer cmd);
  void dispatch(VkCommandBuffer cmd);

  // The rasterized coverage image the host samples in its composite pass.
  VkImage     outputImage()     const { return output_image_; }
  VkImageView outputImageView() const { return output_image_view_; }
  VkSampler   outputSampler()   const { return output_sampler_; }

 private:
  void createDescriptorLayoutAndPool();
  void createBuffers();
  void createOutputImage();
  void createPipelines();
  void writeDescriptors();

  VkDevice         device_          = VK_NULL_HANDLE;
  VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
  AssetReader*     assets_          = nullptr;

  uint32_t screen_width_  = 0;
  uint32_t screen_height_ = 0;
  uint32_t curve_count_   = 0;

  VkDescriptorSetLayout descriptor_set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool      descriptor_pool_       = VK_NULL_HANDLE;
  VkDescriptorSet       descriptor_set_        = VK_NULL_HANDLE;

  VkBuffer       curve_buffer_        = VK_NULL_HANDLE;
  VkDeviceMemory curve_buffer_memory_ = VK_NULL_HANDLE;
  void*          curve_buffer_mapped_ = nullptr;
  VkBuffer       tile_buffer_         = VK_NULL_HANDLE;
  VkDeviceMemory tile_buffer_memory_  = VK_NULL_HANDLE;
  VkBuffer       row_buffer_          = VK_NULL_HANDLE;
  VkDeviceMemory row_buffer_memory_   = VK_NULL_HANDLE;

  VkImage        output_image_        = VK_NULL_HANDLE;
  VkDeviceMemory output_image_memory_ = VK_NULL_HANDLE;
  VkImageView    output_image_view_   = VK_NULL_HANDLE;
  VkSampler      output_sampler_      = VK_NULL_HANDLE;

  VkPipelineLayout compute_pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline       tiling_pipeline_         = VK_NULL_HANDLE;
  VkPipeline       coverage_pipeline_       = VK_NULL_HANDLE;
};
