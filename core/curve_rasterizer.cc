#include "curve_rasterizer.hh"
#include "gpu_util.hh"
#include "log.hh"
#include <cstring>
#include <vector>

#define LOGE(...) VFE_LOGE("CurveRasterizer", __VA_ARGS__)
#define LOGI(...) VFE_LOGI("CurveRasterizer", __VA_ARGS__)

void CurveRasterizer::createDescriptorLayoutAndPool() {
  VkDescriptorSetLayoutBinding bindings[4]{};

  bindings[0].binding         = 0;
  bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[0].descriptorCount = 1;
  bindings[0].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

  bindings[1].binding         = 1;
  bindings[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[1].descriptorCount = 1;
  bindings[1].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

  bindings[2].binding         = 2;
  bindings[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  bindings[2].descriptorCount = 1;
  bindings[2].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

  bindings[3].binding         = 3;
  bindings[3].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  bindings[3].descriptorCount = 1;
  bindings[3].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;

  VkDescriptorSetLayoutCreateInfo layoutInfo{};
  layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = 4;
  layoutInfo.pBindings    = bindings;

  if (vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr,
                                  &descriptor_set_layout_) != VK_SUCCESS) {
    LOGE("Failed to create descriptor set layout");
    return;
  }

  VkDescriptorPoolSize poolSizes[2]{};
  poolSizes[0].type            = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  poolSizes[0].descriptorCount = 3;
  poolSizes[1].type            = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  poolSizes[1].descriptorCount = 1;

  VkDescriptorPoolCreateInfo poolInfo{};
  poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.poolSizeCount = 2;
  poolInfo.pPoolSizes    = poolSizes;
  poolInfo.maxSets       = 1;

  if (vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptor_pool_) !=
      VK_SUCCESS) {
    LOGE("Failed to create descriptor pool");
    return;
  }

  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool     = descriptor_pool_;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts        = &descriptor_set_layout_;

  if (vkAllocateDescriptorSets(device_, &allocInfo, &descriptor_set_) !=
      VK_SUCCESS) {
    LOGE("Failed to allocate descriptor set");
    return;
  }
}

void CurveRasterizer::createBuffers() {
  const VkDeviceSize curveBufferSize =
      static_cast<VkDeviceSize>(MAX_CURVES) * CURVE_FLOATS * sizeof(float);

  VkBufferCreateInfo cbInfo{};
  cbInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  cbInfo.size        = curveBufferSize;
  cbInfo.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  cbInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(device_, &cbInfo, nullptr, &curve_buffer_) != VK_SUCCESS) {
    LOGE("Failed to create curve buffer");
    return;
  }

  VkMemoryRequirements cbReqs{};
  vkGetBufferMemoryRequirements(device_, curve_buffer_, &cbReqs);

  uint32_t cbType = vfeFindMemoryType(physical_device_, cbReqs.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (cbType == UINT32_MAX) {
    LOGE("No HOST_VISIBLE|HOST_COHERENT memory type for curve buffer");
    return;
  }

  VkMemoryAllocateInfo cbAlloc{};
  cbAlloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  cbAlloc.allocationSize  = cbReqs.size;
  cbAlloc.memoryTypeIndex = cbType;
  if (vkAllocateMemory(device_, &cbAlloc, nullptr, &curve_buffer_memory_) !=
      VK_SUCCESS) {
    LOGE("Failed to allocate curve buffer memory");
    return;
  }
  vkBindBufferMemory(device_, curve_buffer_, curve_buffer_memory_, 0);

  if (vkMapMemory(device_, curve_buffer_memory_, 0, curveBufferSize, 0,
                  &curve_buffer_mapped_) != VK_SUCCESS) {
    LOGE("Failed to map curve buffer memory");
    return;
  }
  std::memset(curve_buffer_mapped_, 0, static_cast<size_t>(curveBufferSize));

  const uint32_t tileCountX = (screen_width_  + TILE_SIZE - 1) / TILE_SIZE;
  const uint32_t tileCountY = (screen_height_ + TILE_SIZE - 1) / TILE_SIZE;
  const VkDeviceSize tileBufferSize = static_cast<VkDeviceSize>(tileCountX) *
                                      tileCountY * TILE_STRIDE_U32 *
                                      sizeof(uint32_t);

  VkBufferCreateInfo tbInfo{};
  tbInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  tbInfo.size        = tileBufferSize;
  tbInfo.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  tbInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(device_, &tbInfo, nullptr, &tile_buffer_) != VK_SUCCESS) {
    LOGE("Failed to create tile buffer");
    return;
  }

  VkMemoryRequirements tbReqs{};
  vkGetBufferMemoryRequirements(device_, tile_buffer_, &tbReqs);

  uint32_t tbType = vfeFindMemoryType(physical_device_, tbReqs.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (tbType == UINT32_MAX) {
    LOGE("No DEVICE_LOCAL memory type for tile buffer");
    return;
  }

  VkMemoryAllocateInfo tbAlloc{};
  tbAlloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  tbAlloc.allocationSize  = tbReqs.size;
  tbAlloc.memoryTypeIndex = tbType;
  if (vkAllocateMemory(device_, &tbAlloc, nullptr, &tile_buffer_memory_) !=
      VK_SUCCESS) {
    LOGE("Failed to allocate tile buffer memory");
    return;
  }
  vkBindBufferMemory(device_, tile_buffer_, tile_buffer_memory_, 0);

  // Winding-tile buffer: 2D grid (tileCountX × tileCountY); each cell holds up
  // to MAX_WINDING_PER_TILE winding (type 4/5/6) curve indices (plus a count).
  const VkDeviceSize rowBufferSize = static_cast<VkDeviceSize>(tileCountX) *
                                     tileCountY * WIND_STRIDE_U32 *
                                     sizeof(uint32_t);

  VkBufferCreateInfo rbInfo{};
  rbInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  rbInfo.size        = rowBufferSize;
  rbInfo.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  rbInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(device_, &rbInfo, nullptr, &row_buffer_) != VK_SUCCESS) {
    LOGE("Failed to create row buffer");
    return;
  }

  VkMemoryRequirements rbReqs{};
  vkGetBufferMemoryRequirements(device_, row_buffer_, &rbReqs);

  uint32_t rbType = vfeFindMemoryType(physical_device_, rbReqs.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (rbType == UINT32_MAX) {
    LOGE("No DEVICE_LOCAL memory type for row buffer");
    return;
  }

  VkMemoryAllocateInfo rbAlloc{};
  rbAlloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  rbAlloc.allocationSize  = rbReqs.size;
  rbAlloc.memoryTypeIndex = rbType;
  if (vkAllocateMemory(device_, &rbAlloc, nullptr, &row_buffer_memory_) != VK_SUCCESS) {
    LOGE("Failed to allocate row buffer memory");
    return;
  }
  vkBindBufferMemory(device_, row_buffer_, row_buffer_memory_, 0);
}

void CurveRasterizer::createOutputImage() {
  VkImageCreateInfo imgInfo{};
  imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imgInfo.imageType     = VK_IMAGE_TYPE_2D;
  imgInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
  imgInfo.extent        = {screen_width_, screen_height_, 1};
  imgInfo.mipLevels     = 1;
  imgInfo.arrayLayers   = 1;
  imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
  imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
  imgInfo.usage         = VK_IMAGE_USAGE_STORAGE_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imgInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

  if (vkCreateImage(device_, &imgInfo, nullptr, &output_image_) != VK_SUCCESS) {
    LOGE("Failed to create output image");
    return;
  }

  VkMemoryRequirements imgReqs{};
  vkGetImageMemoryRequirements(device_, output_image_, &imgReqs);

  uint32_t imgType = vfeFindMemoryType(physical_device_, imgReqs.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (imgType == UINT32_MAX) {
    LOGE("No DEVICE_LOCAL memory type for output image");
    return;
  }

  VkMemoryAllocateInfo imgAlloc{};
  imgAlloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  imgAlloc.allocationSize  = imgReqs.size;
  imgAlloc.memoryTypeIndex = imgType;
  if (vkAllocateMemory(device_, &imgAlloc, nullptr, &output_image_memory_) !=
      VK_SUCCESS) {
    LOGE("Failed to allocate output image memory");
    return;
  }
  vkBindImageMemory(device_, output_image_, output_image_memory_, 0);

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image            = output_image_;
  viewInfo.viewType         = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format           = VK_FORMAT_R8G8B8A8_UNORM;
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device_, &viewInfo, nullptr, &output_image_view_) !=
      VK_SUCCESS) {
    LOGE("Failed to create output image view");
    return;
  }

  VkSamplerCreateInfo sampInfo{};
  sampInfo.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sampInfo.magFilter    = VK_FILTER_NEAREST;
  sampInfo.minFilter    = VK_FILTER_NEAREST;
  sampInfo.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sampInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sampInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  if (vkCreateSampler(device_, &sampInfo, nullptr, &output_sampler_) !=
      VK_SUCCESS) {
    LOGE("Failed to create output sampler");
    return;
  }
}

void CurveRasterizer::createPipelines() {
  VkPushConstantRange pushRange{};
  pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pushRange.offset     = 0;
  pushRange.size       = sizeof(uint32_t) * 5;

  VkPipelineLayoutCreateInfo layoutInfo{};
  layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutInfo.setLayoutCount         = 1;
  layoutInfo.pSetLayouts            = &descriptor_set_layout_;
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges    = &pushRange;

  if (vkCreatePipelineLayout(device_, &layoutInfo, nullptr,
                             &compute_pipeline_layout_) != VK_SUCCESS) {
    LOGE("Failed to create compute pipeline layout");
    return;
  }

  VkShaderModule tilingShader   = vfeLoadShaderModule(device_, *assets_, "shaders/tiling.spv");
  VkShaderModule coverageShader = vfeLoadShaderModule(device_, *assets_, "shaders/coverage.spv");
  LOGI("Shaders loaded: tiling=%p coverage=%p",
       (void*)tilingShader, (void*)coverageShader);
  if (tilingShader == VK_NULL_HANDLE || coverageShader == VK_NULL_HANDLE) {
    LOGE("Failed to load compute shader(s)");
    if (tilingShader)   vkDestroyShaderModule(device_, tilingShader,   nullptr);
    if (coverageShader) vkDestroyShaderModule(device_, coverageShader, nullptr);
    return;
  }

  VkComputePipelineCreateInfo infos[2]{};
  for (int i = 0; i < 2; i++) {
    infos[i].sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    infos[i].layout       = compute_pipeline_layout_;
    infos[i].stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    infos[i].stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    infos[i].stage.pName  = "main";
  }
  infos[0].stage.module = tilingShader;
  infos[1].stage.module = coverageShader;

  VkPipeline pipelines[2]{};
  if (vkCreateComputePipelines(device_, VK_NULL_HANDLE, 2, infos, nullptr,
                               pipelines) != VK_SUCCESS) {
    LOGE("Failed to create compute pipelines");
    vkDestroyShaderModule(device_, tilingShader,   nullptr);
    vkDestroyShaderModule(device_, coverageShader, nullptr);
    return;
  }
  tiling_pipeline_   = pipelines[0];
  coverage_pipeline_ = pipelines[1];
  LOGI("Compute pipelines created OK");

  vkDestroyShaderModule(device_, tilingShader,   nullptr);
  vkDestroyShaderModule(device_, coverageShader, nullptr);
}

void CurveRasterizer::writeDescriptors() {
  const VkDeviceSize curveBufferSize =
      static_cast<VkDeviceSize>(MAX_CURVES) * CURVE_FLOATS * sizeof(float);
  const uint32_t tileCountX = (screen_width_  + TILE_SIZE - 1) / TILE_SIZE;
  const uint32_t tileCountY = (screen_height_ + TILE_SIZE - 1) / TILE_SIZE;
  const VkDeviceSize tileBufferSize = static_cast<VkDeviceSize>(tileCountX) *
                                      tileCountY * TILE_STRIDE_U32 *
                                      sizeof(uint32_t);
  const VkDeviceSize rowBufferSize  = static_cast<VkDeviceSize>(tileCountX) *
                                      tileCountY * WIND_STRIDE_U32 *
                                      sizeof(uint32_t);

  VkDescriptorBufferInfo curveDesc{curve_buffer_, 0, curveBufferSize};
  VkDescriptorBufferInfo tileDesc {tile_buffer_,  0, tileBufferSize};
  VkDescriptorBufferInfo rowDesc  {row_buffer_,   0, rowBufferSize};

  VkDescriptorImageInfo imageDesc{};
  imageDesc.imageView   = output_image_view_;
  imageDesc.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  imageDesc.sampler     = VK_NULL_HANDLE;

  VkWriteDescriptorSet writes[4]{};
  writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet          = descriptor_set_;
  writes[0].dstBinding      = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[0].pBufferInfo     = &curveDesc;

  writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet          = descriptor_set_;
  writes[1].dstBinding      = 1;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[1].pBufferInfo     = &tileDesc;

  writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[2].dstSet          = descriptor_set_;
  writes[2].dstBinding      = 2;
  writes[2].descriptorCount = 1;
  writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[2].pImageInfo      = &imageDesc;

  writes[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[3].dstSet          = descriptor_set_;
  writes[3].dstBinding      = 3;
  writes[3].descriptorCount = 1;
  writes[3].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[3].pBufferInfo     = &rowDesc;

  vkUpdateDescriptorSets(device_, 4, writes, 0, nullptr);
}

void CurveRasterizer::init(VkDevice device, VkPhysicalDevice physicalDevice,
                           AssetReader& assets, uint32_t width, uint32_t height) {
  device_          = device;
  physical_device_ = physicalDevice;
  assets_          = &assets;
  screen_width_    = width;
  screen_height_   = height;

  LOGI("CurveRasterizer::init %ux%u", width, height);
  createDescriptorLayoutAndPool();
  createBuffers();
  createOutputImage();
  createPipelines();
  writeDescriptors();
  LOGI("CurveRasterizer::init done");
}

void CurveRasterizer::uploadCurves(const float* curveData, uint32_t count) {
  if (count > MAX_CURVES) {
    LOGE("uploadCurves: count %u exceeds MAX_CURVES %u; truncating",
         count, MAX_CURVES);
    count = MAX_CURVES;
  }
  static bool firstUpload = true;
  if (firstUpload) { LOGI("uploadCurves: %u curves", count); firstUpload = false; }
  if (count > 0 && curve_buffer_mapped_ != nullptr) {
    std::memcpy(curve_buffer_mapped_, curveData,
                static_cast<size_t>(count) * CURVE_FLOATS * sizeof(float));
  }
  curve_count_ = count;
}

void CurveRasterizer::transitionOutputImageInitial(VkCommandBuffer cmd) {
  VkImageMemoryBarrier b{};
  b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image               = output_image_;
  b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  b.srcAccessMask       = 0;
  b.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;

  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &b);
}

void CurveRasterizer::dispatch(VkCommandBuffer cmd) {
  const uint32_t tileCountX = (screen_width_  + TILE_SIZE - 1) / TILE_SIZE;
  const uint32_t tileCountY = (screen_height_ + TILE_SIZE - 1) / TILE_SIZE;

  uint32_t pushData[5] = {screen_width_, screen_height_, curve_count_, tileCountX, tileCountY};

  static bool firstDispatch = true;
  if (firstDispatch) {
    LOGI("dispatch: %u curves, screen %ux%u, tiles %ux%u",
         curve_count_, screen_width_, screen_height_, tileCountX, tileCountY);
    firstDispatch = false;
  }

  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          compute_pipeline_layout_, 0, 1, &descriptor_set_, 0,
                          nullptr);

  const VkDeviceSize tileBufferSize = static_cast<VkDeviceSize>(tileCountX) *
                                      tileCountY * TILE_STRIDE_U32 *
                                      sizeof(uint32_t);
  const VkDeviceSize rowBufferSize  = static_cast<VkDeviceSize>(tileCountX) *
                                      tileCountY * WIND_STRIDE_U32 *
                                      sizeof(uint32_t);
  vkCmdFillBuffer(cmd, tile_buffer_, 0, tileBufferSize, 0);
  vkCmdFillBuffer(cmd, row_buffer_,  0, rowBufferSize,  0);

  {
    VkBufferMemoryBarrier bb[2]{};
    bb[0].sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bb[0].srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    bb[0].dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    bb[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb[0].buffer              = tile_buffer_;
    bb[0].offset              = 0;
    bb[0].size                = VK_WHOLE_SIZE;
    bb[1]                     = bb[0];
    bb[1].buffer              = row_buffer_;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         2, bb, 0, nullptr);
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tiling_pipeline_);
  vkCmdPushConstants(cmd, compute_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                     0, sizeof(uint32_t) * 5, pushData);

  if (curve_count_ > 0) {
    uint32_t groupsX = (curve_count_ + 63) / 64;
    vkCmdDispatch(cmd, groupsX, 1, 1);

    VkBufferMemoryBarrier bb[2]{};
    bb[0].sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bb[0].srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    bb[0].dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    bb[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb[0].buffer              = tile_buffer_;
    bb[0].offset              = 0;
    bb[0].size                = VK_WHOLE_SIZE;
    bb[1]                     = bb[0];
    bb[1].buffer              = row_buffer_;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         2, bb, 0, nullptr);
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, coverage_pipeline_);
  vkCmdPushConstants(cmd, compute_pipeline_layout_, VK_SHADER_STAGE_COMPUTE_BIT,
                     0, sizeof(uint32_t) * 5, pushData);

  if (tileCountX > 0 && tileCountY > 0) {
    vkCmdDispatch(cmd, tileCountX, tileCountY, 1);
  }
}

void CurveRasterizer::cleanup() {
  vkDestroySampler  (device_, output_sampler_,      nullptr);
  vkDestroyImageView(device_, output_image_view_,   nullptr);
  vkDestroyImage    (device_, output_image_,        nullptr);
  vkFreeMemory      (device_, output_image_memory_, nullptr);

  vkDestroyPipeline      (device_, coverage_pipeline_,       nullptr);
  vkDestroyPipeline      (device_, tiling_pipeline_,         nullptr);
  vkDestroyPipelineLayout(device_, compute_pipeline_layout_, nullptr);

  if (curve_buffer_memory_ != VK_NULL_HANDLE && curve_buffer_mapped_ != nullptr) {
    vkUnmapMemory(device_, curve_buffer_memory_);
    curve_buffer_mapped_ = nullptr;
  }
  vkDestroyBuffer(device_, curve_buffer_,        nullptr);
  vkFreeMemory   (device_, curve_buffer_memory_, nullptr);
  vkDestroyBuffer(device_, tile_buffer_,         nullptr);
  vkFreeMemory   (device_, tile_buffer_memory_,  nullptr);
  vkDestroyBuffer(device_, row_buffer_,          nullptr);
  vkFreeMemory   (device_, row_buffer_memory_,   nullptr);

  vkDestroyDescriptorPool     (device_, descriptor_pool_,       nullptr);
  vkDestroyDescriptorSetLayout(device_, descriptor_set_layout_, nullptr);
}
