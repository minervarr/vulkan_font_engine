#include "msdf_renderer.hh"
#include "gpu_util.hh"
#include "log.hh"
#include "msdf.hh"
#include <cstring>

#define LOGE(...) VFE_LOGE("MsdfTextRenderer", __VA_ARGS__)
#define LOGI(...) VFE_LOGI("MsdfTextRenderer", __VA_ARGS__)

void MsdfTextRenderer::init(VkDevice device, VkPhysicalDevice physicalDevice,
                            AssetReader& assets, uint32_t screenWidth,
                            uint32_t screenHeight) {
  device_          = device;
  physical_device_ = physicalDevice;
  assets_          = &assets;
  screen_width_    = screenWidth;
  screen_height_   = screenHeight;
}

void MsdfTextRenderer::createResources(VkRenderPass renderPass,
                                       const MsdfFont& font, int w) {
  if (!font.valid()) { LOGE("MSDF font invalid (weight %d); skipping", w); return; }
  if (w < 0 || w >= MAX_FONT_WEIGHTS) { LOGE("Invalid weight index %d", w); return; }

  atlas_w_ [w] = font.atlasW();
  atlas_h_ [w] = font.atlasH();
  px_range_[w] = font.distanceRange();

  // ── Per-weight: atlas image (device-local, sampled) ───────────────────────
  VkImageCreateInfo img{};
  img.sType     = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  img.imageType = VK_IMAGE_TYPE_2D;
  img.format    = VK_FORMAT_R8G8B8A8_UNORM;
  img.extent    = {atlas_w_[w], atlas_h_[w], 1};
  img.mipLevels = 1; img.arrayLayers = 1; img.samples = VK_SAMPLE_COUNT_1_BIT;
  img.tiling    = VK_IMAGE_TILING_OPTIMAL;
  img.usage     = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  img.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  vkCreateImage(device_, &img, nullptr, &atlas_image_[w]);

  VkMemoryRequirements ir{};
  vkGetImageMemoryRequirements(device_, atlas_image_[w], &ir);
  VkMemoryAllocateInfo ia{};
  ia.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ia.allocationSize  = ir.size;
  ia.memoryTypeIndex = vfeFindMemoryType(physical_device_, ir.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  vkAllocateMemory(device_, &ia, nullptr, &atlas_memory_[w]);
  vkBindImageMemory(device_, atlas_image_[w], atlas_memory_[w], 0);

  VkImageViewCreateInfo iv{};
  iv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  iv.image = atlas_image_[w]; iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
  iv.format = VK_FORMAT_R8G8B8A8_UNORM;
  iv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCreateImageView(device_, &iv, nullptr, &atlas_view_[w]);

  VkSamplerCreateInfo sm{};
  sm.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sm.magFilter = VK_FILTER_LINEAR; sm.minFilter = VK_FILTER_LINEAR;
  sm.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  sm.addressModeU = sm.addressModeV = sm.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  vkCreateSampler(device_, &sm, nullptr, &sampler_[w]);

  // ── Per-weight: staging buffer (for one-shot atlas upload) ────────────────
  VkDeviceSize atlasBytes = (VkDeviceSize)atlas_w_[w] * atlas_h_[w] * 4;
  VkBufferCreateInfo sb{};
  sb.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  sb.size  = atlasBytes; sb.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  vkCreateBuffer(device_, &sb, nullptr, &staging_[w]);
  VkMemoryRequirements sr{};
  vkGetBufferMemoryRequirements(device_, staging_[w], &sr);
  VkMemoryAllocateInfo sa{};
  sa.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  sa.allocationSize  = sr.size;
  sa.memoryTypeIndex = vfeFindMemoryType(physical_device_, sr.memoryTypeBits,
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  vkAllocateMemory(device_, &sa, nullptr, &staging_mem_[w]);
  vkBindBufferMemory(device_, staging_[w], staging_mem_[w], 0);
  void* sp = nullptr;
  vkMapMemory(device_, staging_mem_[w], 0, atlasBytes, 0, &sp);
  std::memcpy(sp, font.atlas().data(), (size_t)atlasBytes);
  vkUnmapMemory(device_, staging_mem_[w]);

  // ── Shared: descriptor layout + pool (created once) ──────────────────────
  if (set_layout_ == VK_NULL_HANDLE) {
    VkDescriptorSetLayoutBinding b{};
    b.binding = 0; b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    b.descriptorCount = 1; b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dl{};
    dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount = 1; dl.pBindings = &b;
    vkCreateDescriptorSetLayout(device_, &dl, nullptr, &set_layout_);
  }
  if (pool_ == VK_NULL_HANDLE) {
    // Pool sized for all weights at once
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)MAX_FONT_WEIGHTS};
    VkDescriptorPoolCreateInfo dp{};
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.poolSizeCount = 1; dp.pPoolSizes = &ps;
    dp.maxSets = (uint32_t)MAX_FONT_WEIGHTS;
    vkCreateDescriptorPool(device_, &dp, nullptr, &pool_);
  }

  // ── Per-weight: allocate + write descriptor set ───────────────────────────
  VkDescriptorSetAllocateInfo das{};
  das.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  das.descriptorPool = pool_; das.descriptorSetCount = 1;
  das.pSetLayouts = &set_layout_;
  vkAllocateDescriptorSets(device_, &das, &set_[w]);

  VkDescriptorImageInfo di{};
  di.sampler = sampler_[w]; di.imageView = atlas_view_[w];
  di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet wr{};
  wr.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  wr.dstSet = set_[w]; wr.dstBinding = 0; wr.descriptorCount = 1;
  wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  wr.pImageInfo = &di;
  vkUpdateDescriptorSets(device_, 1, &wr, 0, nullptr);

  // ── Shared: vertex buffer (created once, split into per-weight regions) ────
  if (vert_buffer_ == VK_NULL_HANDLE) {
    VkDeviceSize vbBytes = (VkDeviceSize)MAX_MSDF_VERTS * MSDF_VERT_FLOATS * sizeof(float);
    VkBufferCreateInfo vb{};
    vb.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    vb.size  = vbBytes; vb.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    vkCreateBuffer(device_, &vb, nullptr, &vert_buffer_);
    VkMemoryRequirements vr{};
    vkGetBufferMemoryRequirements(device_, vert_buffer_, &vr);
    VkMemoryAllocateInfo va{};
    va.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    va.allocationSize  = vr.size;
    va.memoryTypeIndex = vfeFindMemoryType(physical_device_, vr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(device_, &va, nullptr, &vert_memory_);
    vkBindBufferMemory(device_, vert_buffer_, vert_memory_, 0);
    vkMapMemory(device_, vert_memory_, 0, vbBytes, 0, &vert_mapped_);
  }

  // ── Shared: pipeline (created once) ───────────────────────────────────────
  if (pipeline_ == VK_NULL_HANDLE) {
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pcr.size = sizeof(float) * 8;  // screen.xy pxRange pad atlas.xy scroll.xy
    VkPipelineLayoutCreateInfo pl{};
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1; pl.pSetLayouts = &set_layout_;
    pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(device_, &pl, nullptr, &pipeline_layout_);

    VkShaderModule vs = vfeLoadShaderModule(device_, *assets_, "shaders/msdf_vert.spv");
    VkShaderModule fs = vfeLoadShaderModule(device_, *assets_, "shaders/msdf_frag.spv");
    if (vs == VK_NULL_HANDLE || fs == VK_NULL_HANDLE) { LOGE("MSDF shaders missing"); return; }

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_VERTEX_BIT,   vs, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr};

    VkVertexInputBindingDescription bind{0, MSDF_VERT_FLOATS * sizeof(float), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attrs[3]{
      {0, 0, VK_FORMAT_R32G32_SFLOAT,       0},
      {1, 0, VK_FORMAT_R32G32_SFLOAT,       2 * sizeof(float)},
      {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 4 * sizeof(float)},
    };
    VkPipelineVertexInputStateCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vi.vertexBindingDescriptionCount = 1; vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 3; vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia2{};
    ia2.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia2.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp        = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp        = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
                         VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &cba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2; ds.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo gp{};
    gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gp.stageCount = 2; gp.pStages = stages;
    gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia2;
    gp.pViewportState    = &vp; gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms; gp.pColorBlendState    = &cb;
    gp.pDynamicState = &ds; gp.layout = pipeline_layout_;
    gp.renderPass = renderPass; gp.subpass = 0;
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp,
                                  nullptr, &pipeline_) != VK_SUCCESS) {
      LOGE("Failed to create MSDF pipeline"); pipeline_ = VK_NULL_HANDLE;
    } else { LOGI("MSDF pipeline created OK"); }
    vkDestroyShaderModule(device_, vs, nullptr);
    vkDestroyShaderModule(device_, fs, nullptr);
  }
  LOGI("MSDF weight %d: atlas %ux%u ready", w, atlas_w_[w], atlas_h_[w]);
}

void MsdfTextRenderer::recordAtlasUpload(VkCommandBuffer cmd, int w) {
  if (atlas_image_[w] == VK_NULL_HANDLE) return;
  VkImageMemoryBarrier toDst{};
  toDst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  toDst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  toDst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toDst.image = atlas_image_[w];
  toDst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  toDst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toDst);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent      = {atlas_w_[w], atlas_h_[w], 1};
  vkCmdCopyBufferToImage(cmd, staging_[w], atlas_image_[w],
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  VkImageMemoryBarrier toRead = toDst;
  toRead.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toRead.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  toRead.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toRead.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &toRead);
}

void MsdfTextRenderer::uploadGlyphQuads(const float* verts, uint32_t vertCount, int w) {
  if (w < 0 || w >= MAX_FONT_WEIGHTS || !vert_mapped_) return;
  uint32_t maxPerWeight = MAX_MSDF_VERTS / (uint32_t)MAX_FONT_WEIGHTS;
  if (vertCount > maxPerWeight) vertCount = maxPerWeight;
  size_t byteOffset = (size_t)vertOffset(w) * MSDF_VERT_FLOATS * sizeof(float);
  if (vertCount > 0)
    std::memcpy((char*)vert_mapped_ + byteOffset, verts,
                (size_t)vertCount * MSDF_VERT_FLOATS * sizeof(float));
  vert_count_[w] = vertCount;
}

void MsdfTextRenderer::draw(VkCommandBuffer cmd, uint32_t firstVert,
                            uint32_t vertCount, float scrollX, float scrollY,
                            int32_t sx, int32_t sy, uint32_t sw, uint32_t sh,
                            int w) {
  if (pipeline_ == VK_NULL_HANDLE || vertCount == 0) return;
  if (w < 0 || w >= MAX_FONT_WEIGHTS || atlas_image_[w] == VK_NULL_HANDLE) return;

  VkViewport vp{0.f, 0.f, (float)screen_width_, (float)screen_height_, 0.f, 1.f};
  VkRect2D   sc{{sx, sy}, {sw, sh}};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  vkCmdSetScissor (cmd, 0, 1, &sc);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
  float push[8] = {(float)screen_width_, (float)screen_height_, px_range_[w], 0.f,
                   (float)atlas_w_[w], (float)atlas_h_[w], scrollX, scrollY};
  vkCmdPushConstants(cmd, pipeline_layout_,
                     VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(push), push);
  // Bind this weight's descriptor set (atlas texture)
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_,
                          0, 1, &set_[w], 0, nullptr);
  VkDeviceSize off = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vert_buffer_, &off);
  vkCmdDraw(cmd, vertCount, 1, firstVert, 0);
}

void MsdfTextRenderer::cleanup() {
  if (vert_mapped_) { vkUnmapMemory(device_, vert_memory_); vert_mapped_ = nullptr; }
  if (pipeline_)        vkDestroyPipeline      (device_, pipeline_,        nullptr);
  if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
  if (pool_)            vkDestroyDescriptorPool (device_, pool_,           nullptr);
  if (set_layout_)      vkDestroyDescriptorSetLayout(device_, set_layout_, nullptr);
  if (vert_buffer_)     vkDestroyBuffer(device_, vert_buffer_, nullptr);
  if (vert_memory_)     vkFreeMemory   (device_, vert_memory_, nullptr);
  for (int wi = 0; wi < MAX_FONT_WEIGHTS; wi++) {
    if (staging_[wi])      vkDestroyBuffer   (device_, staging_[wi],      nullptr);
    if (staging_mem_[wi])  vkFreeMemory      (device_, staging_mem_[wi],  nullptr);
    if (sampler_[wi])      vkDestroySampler  (device_, sampler_[wi],      nullptr);
    if (atlas_view_[wi])   vkDestroyImageView(device_, atlas_view_[wi],   nullptr);
    if (atlas_image_[wi])  vkDestroyImage    (device_, atlas_image_[wi],  nullptr);
    if (atlas_memory_[wi]) vkFreeMemory      (device_, atlas_memory_[wi], nullptr);
  }
}
