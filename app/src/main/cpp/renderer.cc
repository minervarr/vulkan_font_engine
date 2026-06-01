#include "renderer.hh"
#include "msdf.hh"
#include <android/log.h>
#include <cstring>
#include <vector>

#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "Renderer", __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  "Renderer", __VA_ARGS__)

uint32_t Renderer::findMemoryType(uint32_t typeBits,
                                  VkMemoryPropertyFlags required) {
  VkPhysicalDeviceMemoryProperties props{};
  vkGetPhysicalDeviceMemoryProperties(physicalDevice, &props);
  for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
    bool compatible = (typeBits & (1u << i)) != 0;
    bool hasFlags   = (props.memoryTypes[i].propertyFlags & required) == required;
    if (compatible && hasFlags) return i;
  }
  return UINT32_MAX;
}

VkShaderModule Renderer::loadShader(const char* path) {
  AAsset* asset = AAssetManager_open(assetManager, path, AASSET_MODE_BUFFER);
  if (!asset) {
    LOGE("Could not open shader asset: %s", path);
    return VK_NULL_HANDLE;
  }
  size_t size = AAsset_getLength(asset);
  std::vector<uint32_t> code(size / sizeof(uint32_t));
  AAsset_read(asset, code.data(), size);
  AAsset_close(asset);

  VkShaderModuleCreateInfo info{};
  info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  info.codeSize = size;
  info.pCode    = code.data();

  VkShaderModule mod = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device, &info, nullptr, &mod) != VK_SUCCESS) {
    LOGE("Could not create shader module: %s", path);
    return VK_NULL_HANDLE;
  }
  return mod;
}

void Renderer::createDescriptorLayoutAndPool() {
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

  if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr,
                                  &descriptorSetLayout) != VK_SUCCESS) {
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

  if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptorPool) !=
      VK_SUCCESS) {
    LOGE("Failed to create descriptor pool");
    return;
  }

  VkDescriptorSetAllocateInfo allocInfo{};
  allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool     = descriptorPool;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts        = &descriptorSetLayout;

  if (vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet) !=
      VK_SUCCESS) {
    LOGE("Failed to allocate descriptor set");
    return;
  }
}

void Renderer::createBuffers() {
  const VkDeviceSize curveBufferSize =
      static_cast<VkDeviceSize>(MAX_CURVES) * CURVE_FLOATS * sizeof(float);

  VkBufferCreateInfo cbInfo{};
  cbInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  cbInfo.size        = curveBufferSize;
  cbInfo.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  cbInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(device, &cbInfo, nullptr, &curveBuffer) != VK_SUCCESS) {
    LOGE("Failed to create curve buffer");
    return;
  }

  VkMemoryRequirements cbReqs{};
  vkGetBufferMemoryRequirements(device, curveBuffer, &cbReqs);

  uint32_t cbType = findMemoryType(cbReqs.memoryTypeBits,
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
  if (vkAllocateMemory(device, &cbAlloc, nullptr, &curveBufferMemory) !=
      VK_SUCCESS) {
    LOGE("Failed to allocate curve buffer memory");
    return;
  }
  vkBindBufferMemory(device, curveBuffer, curveBufferMemory, 0);

  if (vkMapMemory(device, curveBufferMemory, 0, curveBufferSize, 0,
                  &curveBufferMapped) != VK_SUCCESS) {
    LOGE("Failed to map curve buffer memory");
    return;
  }
  std::memset(curveBufferMapped, 0, static_cast<size_t>(curveBufferSize));

  const uint32_t tileCountX = (screenWidth  + TILE_SIZE - 1) / TILE_SIZE;
  const uint32_t tileCountY = (screenHeight + TILE_SIZE - 1) / TILE_SIZE;
  const VkDeviceSize tileBufferSize = static_cast<VkDeviceSize>(tileCountX) *
                                      tileCountY * TILE_STRIDE_U32 *
                                      sizeof(uint32_t);

  VkBufferCreateInfo tbInfo{};
  tbInfo.sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  tbInfo.size        = tileBufferSize;
  tbInfo.usage       = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                       VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  tbInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateBuffer(device, &tbInfo, nullptr, &tileBuffer) != VK_SUCCESS) {
    LOGE("Failed to create tile buffer");
    return;
  }

  VkMemoryRequirements tbReqs{};
  vkGetBufferMemoryRequirements(device, tileBuffer, &tbReqs);

  uint32_t tbType =
      findMemoryType(tbReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (tbType == UINT32_MAX) {
    LOGE("No DEVICE_LOCAL memory type for tile buffer");
    return;
  }

  VkMemoryAllocateInfo tbAlloc{};
  tbAlloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  tbAlloc.allocationSize  = tbReqs.size;
  tbAlloc.memoryTypeIndex = tbType;
  if (vkAllocateMemory(device, &tbAlloc, nullptr, &tileBufferMemory) !=
      VK_SUCCESS) {
    LOGE("Failed to allocate tile buffer memory");
    return;
  }
  vkBindBufferMemory(device, tileBuffer, tileBufferMemory, 0);

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
  if (vkCreateBuffer(device, &rbInfo, nullptr, &rowBuffer) != VK_SUCCESS) {
    LOGE("Failed to create row buffer");
    return;
  }

  VkMemoryRequirements rbReqs{};
  vkGetBufferMemoryRequirements(device, rowBuffer, &rbReqs);

  uint32_t rbType =
      findMemoryType(rbReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (rbType == UINT32_MAX) {
    LOGE("No DEVICE_LOCAL memory type for row buffer");
    return;
  }

  VkMemoryAllocateInfo rbAlloc{};
  rbAlloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  rbAlloc.allocationSize  = rbReqs.size;
  rbAlloc.memoryTypeIndex = rbType;
  if (vkAllocateMemory(device, &rbAlloc, nullptr, &rowBufferMemory) != VK_SUCCESS) {
    LOGE("Failed to allocate row buffer memory");
    return;
  }
  vkBindBufferMemory(device, rowBuffer, rowBufferMemory, 0);
}

void Renderer::createOutputImage() {
  VkImageCreateInfo imgInfo{};
  imgInfo.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  imgInfo.imageType     = VK_IMAGE_TYPE_2D;
  imgInfo.format        = VK_FORMAT_R8G8B8A8_UNORM;
  imgInfo.extent        = {screenWidth, screenHeight, 1};
  imgInfo.mipLevels     = 1;
  imgInfo.arrayLayers   = 1;
  imgInfo.samples       = VK_SAMPLE_COUNT_1_BIT;
  imgInfo.tiling        = VK_IMAGE_TILING_OPTIMAL;
  imgInfo.usage         = VK_IMAGE_USAGE_STORAGE_BIT |
                          VK_IMAGE_USAGE_SAMPLED_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imgInfo.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;

  if (vkCreateImage(device, &imgInfo, nullptr, &outputImage) != VK_SUCCESS) {
    LOGE("Failed to create output image");
    return;
  }

  VkMemoryRequirements imgReqs{};
  vkGetImageMemoryRequirements(device, outputImage, &imgReqs);

  uint32_t imgType =
      findMemoryType(imgReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (imgType == UINT32_MAX) {
    LOGE("No DEVICE_LOCAL memory type for output image");
    return;
  }

  VkMemoryAllocateInfo imgAlloc{};
  imgAlloc.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  imgAlloc.allocationSize  = imgReqs.size;
  imgAlloc.memoryTypeIndex = imgType;
  if (vkAllocateMemory(device, &imgAlloc, nullptr, &outputImageMemory) !=
      VK_SUCCESS) {
    LOGE("Failed to allocate output image memory");
    return;
  }
  vkBindImageMemory(device, outputImage, outputImageMemory, 0);

  VkImageViewCreateInfo viewInfo{};
  viewInfo.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image            = outputImage;
  viewInfo.viewType         = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format           = VK_FORMAT_R8G8B8A8_UNORM;
  viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  if (vkCreateImageView(device, &viewInfo, nullptr, &outputImageView) !=
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
  if (vkCreateSampler(device, &sampInfo, nullptr, &outputSampler) !=
      VK_SUCCESS) {
    LOGE("Failed to create output sampler");
    return;
  }
}

void Renderer::createPipelines() {
  VkPushConstantRange pushRange{};
  pushRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  pushRange.offset     = 0;
  pushRange.size       = sizeof(uint32_t) * 5;

  VkPipelineLayoutCreateInfo layoutInfo{};
  layoutInfo.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  layoutInfo.setLayoutCount         = 1;
  layoutInfo.pSetLayouts            = &descriptorSetLayout;
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges    = &pushRange;

  if (vkCreatePipelineLayout(device, &layoutInfo, nullptr,
                             &computePipelineLayout) != VK_SUCCESS) {
    LOGE("Failed to create compute pipeline layout");
    return;
  }

  VkShaderModule tilingShader   = loadShader("shaders/tiling.spv");
  VkShaderModule coverageShader = loadShader("shaders/coverage.spv");
  LOGI("Shaders loaded: tiling=%p coverage=%p",
       (void*)tilingShader, (void*)coverageShader);
  if (tilingShader == VK_NULL_HANDLE || coverageShader == VK_NULL_HANDLE) {
    LOGE("Failed to load compute shader(s)");
    if (tilingShader)   vkDestroyShaderModule(device, tilingShader,   nullptr);
    if (coverageShader) vkDestroyShaderModule(device, coverageShader, nullptr);
    return;
  }

  VkComputePipelineCreateInfo infos[2]{};
  for (int i = 0; i < 2; i++) {
    infos[i].sType        = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    infos[i].layout       = computePipelineLayout;
    infos[i].stage.sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    infos[i].stage.stage  = VK_SHADER_STAGE_COMPUTE_BIT;
    infos[i].stage.pName  = "main";
  }
  infos[0].stage.module = tilingShader;
  infos[1].stage.module = coverageShader;

  VkPipeline pipelines[2]{};
  if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 2, infos, nullptr,
                               pipelines) != VK_SUCCESS) {
    LOGE("Failed to create compute pipelines");
    vkDestroyShaderModule(device, tilingShader,   nullptr);
    vkDestroyShaderModule(device, coverageShader, nullptr);
    return;
  }
  tilingPipeline   = pipelines[0];
  coveragePipeline = pipelines[1];
  LOGI("Compute pipelines created OK");

  vkDestroyShaderModule(device, tilingShader,   nullptr);
  vkDestroyShaderModule(device, coverageShader, nullptr);
}

void Renderer::writeDescriptors() {
  const VkDeviceSize curveBufferSize =
      static_cast<VkDeviceSize>(MAX_CURVES) * CURVE_FLOATS * sizeof(float);
  const uint32_t tileCountX = (screenWidth  + TILE_SIZE - 1) / TILE_SIZE;
  const uint32_t tileCountY = (screenHeight + TILE_SIZE - 1) / TILE_SIZE;
  const VkDeviceSize tileBufferSize = static_cast<VkDeviceSize>(tileCountX) *
                                      tileCountY * TILE_STRIDE_U32 *
                                      sizeof(uint32_t);
  const VkDeviceSize rowBufferSize  = static_cast<VkDeviceSize>(tileCountX) *
                                      tileCountY * WIND_STRIDE_U32 *
                                      sizeof(uint32_t);

  VkDescriptorBufferInfo curveDesc{curveBuffer, 0, curveBufferSize};
  VkDescriptorBufferInfo tileDesc {tileBuffer,  0, tileBufferSize};
  VkDescriptorBufferInfo rowDesc  {rowBuffer,   0, rowBufferSize};

  VkDescriptorImageInfo imageDesc{};
  imageDesc.imageView   = outputImageView;
  imageDesc.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  imageDesc.sampler     = VK_NULL_HANDLE;

  VkWriteDescriptorSet writes[4]{};
  writes[0].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[0].dstSet          = descriptorSet;
  writes[0].dstBinding      = 0;
  writes[0].descriptorCount = 1;
  writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[0].pBufferInfo     = &curveDesc;

  writes[1].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[1].dstSet          = descriptorSet;
  writes[1].dstBinding      = 1;
  writes[1].descriptorCount = 1;
  writes[1].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[1].pBufferInfo     = &tileDesc;

  writes[2].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[2].dstSet          = descriptorSet;
  writes[2].dstBinding      = 2;
  writes[2].descriptorCount = 1;
  writes[2].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  writes[2].pImageInfo      = &imageDesc;

  writes[3].sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  writes[3].dstSet          = descriptorSet;
  writes[3].dstBinding      = 3;
  writes[3].descriptorCount = 1;
  writes[3].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[3].pBufferInfo     = &rowDesc;

  vkUpdateDescriptorSets(device, 4, writes, 0, nullptr);
}

void Renderer::init(VkDevice dev, VkPhysicalDevice phyDev, AAssetManager* mgr,
                    uint32_t width, uint32_t height) {
  device         = dev;
  physicalDevice = phyDev;
  assetManager   = mgr;
  screenWidth    = width;
  screenHeight   = height;

  LOGI("Renderer::init %ux%u", width, height);
  createDescriptorLayoutAndPool();
  createBuffers();
  createOutputImage();
  createPipelines();
  writeDescriptors();
  LOGI("Renderer::init done");
}

void Renderer::uploadCurves(const float* curveData, uint32_t count) {
  if (count > MAX_CURVES) {
    LOGE("uploadCurves: count %u exceeds MAX_CURVES %u; truncating",
         count, MAX_CURVES);
    count = MAX_CURVES;
  }
  static bool firstUpload = true;
  if (firstUpload) { LOGI("uploadCurves: %u curves", count); firstUpload = false; }
  if (count > 0 && curveBufferMapped != nullptr) {
    std::memcpy(curveBufferMapped, curveData,
                static_cast<size_t>(count) * CURVE_FLOATS * sizeof(float));
  }
  curveCount = count;
}

void Renderer::transitionOutputImageInitial(VkCommandBuffer cmd) {
  VkImageMemoryBarrier b{};
  b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image               = outputImage;
  b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  b.srcAccessMask       = 0;
  b.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;

  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &b);
}

void Renderer::dispatch(VkCommandBuffer cmd) {
  const uint32_t tileCountX = (screenWidth  + TILE_SIZE - 1) / TILE_SIZE;
  const uint32_t tileCountY = (screenHeight + TILE_SIZE - 1) / TILE_SIZE;

  uint32_t pushData[5] = {screenWidth, screenHeight, curveCount, tileCountX, tileCountY};

  static bool firstDispatch = true;
  if (firstDispatch) {
    LOGI("dispatch: %u curves, screen %ux%u, tiles %ux%u",
         curveCount, screenWidth, screenHeight, tileCountX, tileCountY);
    firstDispatch = false;
  }

  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                          computePipelineLayout, 0, 1, &descriptorSet, 0,
                          nullptr);

  const VkDeviceSize tileBufferSize = static_cast<VkDeviceSize>(tileCountX) *
                                      tileCountY * TILE_STRIDE_U32 *
                                      sizeof(uint32_t);
  const VkDeviceSize rowBufferSize  = static_cast<VkDeviceSize>(tileCountX) *
                                      tileCountY * WIND_STRIDE_U32 *
                                      sizeof(uint32_t);
  vkCmdFillBuffer(cmd, tileBuffer, 0, tileBufferSize, 0);
  vkCmdFillBuffer(cmd, rowBuffer,  0, rowBufferSize,  0);

  {
    VkBufferMemoryBarrier bb[2]{};
    bb[0].sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bb[0].srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
    bb[0].dstAccessMask       = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    bb[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb[0].buffer              = tileBuffer;
    bb[0].offset              = 0;
    bb[0].size                = VK_WHOLE_SIZE;
    bb[1]                     = bb[0];
    bb[1].buffer              = rowBuffer;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         2, bb, 0, nullptr);
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, tilingPipeline);
  vkCmdPushConstants(cmd, computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                     0, sizeof(uint32_t) * 5, pushData);

  if (curveCount > 0) {
    uint32_t groupsX = (curveCount + 63) / 64;
    vkCmdDispatch(cmd, groupsX, 1, 1);

    VkBufferMemoryBarrier bb[2]{};
    bb[0].sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bb[0].srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    bb[0].dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    bb[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb[0].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb[0].buffer              = tileBuffer;
    bb[0].offset              = 0;
    bb[0].size                = VK_WHOLE_SIZE;
    bb[1]                     = bb[0];
    bb[1].buffer              = rowBuffer;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         2, bb, 0, nullptr);
  }

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, coveragePipeline);
  vkCmdPushConstants(cmd, computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                     0, sizeof(uint32_t) * 5, pushData);

  if (tileCountX > 0 && tileCountY > 0) {
    vkCmdDispatch(cmd, tileCountX, tileCountY, 1);
  }
}

// ── MSDF text path ───────────────────────────────────────────────────────────

void Renderer::createMsdfResources(VkRenderPass renderPass, const MsdfFont& font) {
  if (!font.valid()) { LOGE("MSDF font invalid; skipping MSDF setup"); return; }
  msdfAtlasW  = font.atlasW();
  msdfAtlasH  = font.atlasH();
  msdfPxRange = font.distanceRange();

  // Atlas image (device-local, sampled).
  VkImageCreateInfo img{};
  img.sType         = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  img.imageType     = VK_IMAGE_TYPE_2D;
  img.format        = VK_FORMAT_R8G8B8A8_UNORM;
  img.extent        = {msdfAtlasW, msdfAtlasH, 1};
  img.mipLevels     = 1;
  img.arrayLayers   = 1;
  img.samples       = VK_SAMPLE_COUNT_1_BIT;
  img.tiling        = VK_IMAGE_TILING_OPTIMAL;
  img.usage         = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  img.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  vkCreateImage(device, &img, nullptr, &msdfAtlasImage);

  VkMemoryRequirements ir{};
  vkGetImageMemoryRequirements(device, msdfAtlasImage, &ir);
  VkMemoryAllocateInfo ia{};
  ia.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ia.allocationSize  = ir.size;
  ia.memoryTypeIndex = findMemoryType(ir.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  vkAllocateMemory(device, &ia, nullptr, &msdfAtlasMemory);
  vkBindImageMemory(device, msdfAtlasImage, msdfAtlasMemory, 0);

  VkImageViewCreateInfo iv{};
  iv.sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  iv.image            = msdfAtlasImage;
  iv.viewType         = VK_IMAGE_VIEW_TYPE_2D;
  iv.format           = VK_FORMAT_R8G8B8A8_UNORM;
  iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCreateImageView(device, &iv, nullptr, &msdfAtlasView);

  VkSamplerCreateInfo sm{};
  sm.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sm.magFilter    = VK_FILTER_LINEAR;
  sm.minFilter    = VK_FILTER_LINEAR;
  sm.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sm.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sm.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sm.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  vkCreateSampler(device, &sm, nullptr, &msdfSampler);

  // Staging buffer holding the atlas bytes (uploaded later via recordAtlasUpload).
  VkDeviceSize atlasBytes = (VkDeviceSize)msdfAtlasW * msdfAtlasH * 4;
  VkBufferCreateInfo sb{};
  sb.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  sb.size  = atlasBytes;
  sb.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  vkCreateBuffer(device, &sb, nullptr, &msdfStaging);
  VkMemoryRequirements sr{};
  vkGetBufferMemoryRequirements(device, msdfStaging, &sr);
  VkMemoryAllocateInfo sa{};
  sa.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  sa.allocationSize  = sr.size;
  sa.memoryTypeIndex = findMemoryType(sr.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  vkAllocateMemory(device, &sa, nullptr, &msdfStagingMem);
  vkBindBufferMemory(device, msdfStaging, msdfStagingMem, 0);
  void* sp = nullptr;
  vkMapMemory(device, msdfStagingMem, 0, atlasBytes, 0, &sp);
  std::memcpy(sp, font.atlas().data(), (size_t)atlasBytes);
  vkUnmapMemory(device, msdfStagingMem);

  // Descriptor set: one combined image sampler.
  VkDescriptorSetLayoutBinding b{};
  b.binding         = 0;
  b.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  b.descriptorCount = 1;
  b.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo dl{};
  dl.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  dl.bindingCount = 1;
  dl.pBindings    = &b;
  vkCreateDescriptorSetLayout(device, &dl, nullptr, &msdfSetLayout);

  VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
  VkDescriptorPoolCreateInfo dp{};
  dp.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dp.poolSizeCount = 1;
  dp.pPoolSizes    = &ps;
  dp.maxSets       = 1;
  vkCreateDescriptorPool(device, &dp, nullptr, &msdfPool);

  VkDescriptorSetAllocateInfo das{};
  das.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  das.descriptorPool     = msdfPool;
  das.descriptorSetCount = 1;
  das.pSetLayouts        = &msdfSetLayout;
  vkAllocateDescriptorSets(device, &das, &msdfSet);

  VkDescriptorImageInfo di{};
  di.sampler     = msdfSampler;
  di.imageView   = msdfAtlasView;
  di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet w{};
  w.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w.dstSet          = msdfSet;
  w.dstBinding      = 0;
  w.descriptorCount = 1;
  w.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w.pImageInfo      = &di;
  vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);

  // Vertex buffer (host-visible, rewritten each scene change).
  VkDeviceSize vbBytes = (VkDeviceSize)MAX_MSDF_VERTS * MSDF_VERT_FLOATS * sizeof(float);
  VkBufferCreateInfo vb{};
  vb.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  vb.size  = vbBytes;
  vb.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
  vkCreateBuffer(device, &vb, nullptr, &msdfVertBuffer);
  VkMemoryRequirements vr{};
  vkGetBufferMemoryRequirements(device, msdfVertBuffer, &vr);
  VkMemoryAllocateInfo va{};
  va.sType           = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  va.allocationSize  = vr.size;
  va.memoryTypeIndex = findMemoryType(vr.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  vkAllocateMemory(device, &va, nullptr, &msdfVertMemory);
  vkBindBufferMemory(device, msdfVertBuffer, msdfVertMemory, 0);
  vkMapMemory(device, msdfVertMemory, 0, vbBytes, 0, &msdfVertMapped);

  // Pipeline.
  VkPushConstantRange pcr{};
  pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  pcr.offset     = 0;
  pcr.size       = sizeof(float) * 8;  // screen.xy pxRange pad atlas.xy scroll.xy
  VkPipelineLayoutCreateInfo pl{};
  pl.sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pl.setLayoutCount         = 1;
  pl.pSetLayouts            = &msdfSetLayout;
  pl.pushConstantRangeCount = 1;
  pl.pPushConstantRanges    = &pcr;
  vkCreatePipelineLayout(device, &pl, nullptr, &msdfPipelineLayout);

  VkShaderModule vs = loadShader("shaders/msdf_vert.spv");
  VkShaderModule fs = loadShader("shaders/msdf_frag.spv");
  if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) { LOGE("MSDF shaders missing"); return; }

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName  = "main";
  stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs;
  stages[1].pName  = "main";

  VkVertexInputBindingDescription bind{};
  bind.binding   = 0;
  bind.stride    = MSDF_VERT_FLOATS * sizeof(float);
  bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription attrs[3]{};
  attrs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT,       0};
  attrs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT,       2 * sizeof(float)};
  attrs[2] = {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 4 * sizeof(float)};
  VkPipelineVertexInputStateCreateInfo vi{};
  vi.sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vi.vertexBindingDescriptionCount   = 1;
  vi.pVertexBindingDescriptions      = &bind;
  vi.vertexAttributeDescriptionCount = 3;
  vi.pVertexAttributeDescriptions    = attrs;

  VkPipelineInputAssemblyStateCreateInfo ia2{};
  ia2.sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  ia2.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

  VkPipelineViewportStateCreateInfo vp{};
  vp.sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  vp.viewportCount = 1;
  vp.scissorCount  = 1;

  VkPipelineRasterizationStateCreateInfo rs{};
  rs.sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode    = VK_CULL_MODE_NONE;
  rs.frontFace   = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth   = 1.0f;

  VkPipelineMultisampleStateCreateInfo ms{};
  ms.sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState cba{};
  cba.blendEnable         = VK_TRUE;
  cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  cba.colorBlendOp        = VK_BLEND_OP_ADD;
  cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  cba.alphaBlendOp        = VK_BLEND_OP_ADD;
  cba.colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cb{};
  cb.sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  cb.attachmentCount = 1;
  cb.pAttachments    = &cba;

  VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds{};
  ds.sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  ds.dynamicStateCount = 2;
  ds.pDynamicStates    = dyn;

  VkGraphicsPipelineCreateInfo gp{};
  gp.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  gp.stageCount          = 2;
  gp.pStages             = stages;
  gp.pVertexInputState   = &vi;
  gp.pInputAssemblyState = &ia2;
  gp.pViewportState      = &vp;
  gp.pRasterizationState = &rs;
  gp.pMultisampleState   = &ms;
  gp.pColorBlendState    = &cb;
  gp.pDynamicState       = &ds;
  gp.layout              = msdfPipelineLayout;
  gp.renderPass          = renderPass;
  gp.subpass             = 0;
  if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr,
                                &msdfPipeline) != VK_SUCCESS) {
    LOGE("Failed to create MSDF pipeline");
    msdfPipeline = VK_NULL_HANDLE;
  } else {
    LOGI("MSDF pipeline created OK");
  }
  vkDestroyShaderModule(device, vs, nullptr);
  vkDestroyShaderModule(device, fs, nullptr);
}

void Renderer::recordAtlasUpload(VkCommandBuffer cmd) {
  if (msdfAtlasImage == VK_NULL_HANDLE) return;

  VkImageMemoryBarrier toDst{};
  toDst.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  toDst.oldLayout           = VK_IMAGE_LAYOUT_UNDEFINED;
  toDst.newLayout           = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toDst.image               = msdfAtlasImage;
  toDst.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  toDst.srcAccessMask       = 0;
  toDst.dstAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
                       1, &toDst);

  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent      = {msdfAtlasW, msdfAtlasH, 1};
  vkCmdCopyBufferToImage(cmd, msdfStaging, msdfAtlasImage,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  VkImageMemoryBarrier toRead = toDst;
  toRead.oldLayout     = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toRead.newLayout     = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
                       nullptr, 1, &toRead);
}

void Renderer::uploadGlyphQuads(const float* verts, uint32_t vertCount) {
  if (vertCount > MAX_MSDF_VERTS) vertCount = MAX_MSDF_VERTS;
  if (vertCount > 0 && msdfVertMapped)
    std::memcpy(msdfVertMapped, verts, (size_t)vertCount * MSDF_VERT_FLOATS * sizeof(float));
  msdfVertCount = vertCount;
}

void Renderer::drawMsdfRange(VkCommandBuffer cmd, uint32_t firstVert,
                            uint32_t vertCount, float scrollX, float scrollY,
                            int32_t sx, int32_t sy, uint32_t sw, uint32_t sh) {
  if (msdfPipeline == VK_NULL_HANDLE || vertCount == 0) return;

  VkViewport vp{0.f, 0.f, (float)screenWidth, (float)screenHeight, 0.f, 1.f};
  VkRect2D   sc{{sx, sy}, {sw, sh}};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor(cmd, 0, 1, &sc);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, msdfPipeline);
  float push[8] = {(float)screenWidth, (float)screenHeight, msdfPxRange, 0.0f,
                   (float)msdfAtlasW, (float)msdfAtlasH, scrollX, scrollY};
  vkCmdPushConstants(cmd, msdfPipelineLayout,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(push), push);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, msdfPipelineLayout,
                          0, 1, &msdfSet, 0, nullptr);
  VkDeviceSize off = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &msdfVertBuffer, &off);
  vkCmdDraw(cmd, vertCount, 1, firstVert, 0);
}

void Renderer::cleanup() {
  vkDestroySampler  (device, outputSampler,     nullptr);
  vkDestroyImageView(device, outputImageView,   nullptr);
  vkDestroyImage    (device, outputImage,       nullptr);
  vkFreeMemory      (device, outputImageMemory, nullptr);

  vkDestroyPipeline      (device, coveragePipeline,      nullptr);
  vkDestroyPipeline      (device, tilingPipeline,        nullptr);
  vkDestroyPipelineLayout(device, computePipelineLayout, nullptr);

  if (curveBufferMemory != VK_NULL_HANDLE && curveBufferMapped != nullptr) {
    vkUnmapMemory(device, curveBufferMemory);
    curveBufferMapped = nullptr;
  }
  vkDestroyBuffer(device, curveBuffer,       nullptr);
  vkFreeMemory   (device, curveBufferMemory, nullptr);
  vkDestroyBuffer(device, tileBuffer,        nullptr);
  vkFreeMemory   (device, tileBufferMemory,  nullptr);
  vkDestroyBuffer(device, rowBuffer,         nullptr);
  vkFreeMemory   (device, rowBufferMemory,   nullptr);

  vkDestroyDescriptorPool     (device, descriptorPool,      nullptr);
  vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);

  // MSDF resources
  if (msdfVertMapped) { vkUnmapMemory(device, msdfVertMemory); msdfVertMapped = nullptr; }
  if (msdfPipeline)       vkDestroyPipeline      (device, msdfPipeline,       nullptr);
  if (msdfPipelineLayout) vkDestroyPipelineLayout(device, msdfPipelineLayout, nullptr);
  if (msdfPool)           vkDestroyDescriptorPool (device, msdfPool,          nullptr);
  if (msdfSetLayout)      vkDestroyDescriptorSetLayout(device, msdfSetLayout, nullptr);
  if (msdfVertBuffer)     vkDestroyBuffer(device, msdfVertBuffer, nullptr);
  if (msdfVertMemory)     vkFreeMemory   (device, msdfVertMemory, nullptr);
  if (msdfStaging)        vkDestroyBuffer(device, msdfStaging,    nullptr);
  if (msdfStagingMem)     vkFreeMemory   (device, msdfStagingMem, nullptr);
  if (msdfSampler)        vkDestroySampler  (device, msdfSampler,     nullptr);
  if (msdfAtlasView)      vkDestroyImageView(device, msdfAtlasView,   nullptr);
  if (msdfAtlasImage)     vkDestroyImage    (device, msdfAtlasImage,  nullptr);
  if (msdfAtlasMemory)    vkFreeMemory      (device, msdfAtlasMemory, nullptr);
}
