#include "glyph_baker.hh"

#include "gpu_util.hh"
#include "log.hh"

#include <cstring>

namespace {
constexpr uint32_t kBindings = 5;
}

// ── Setup ───────────────────────────────────────────────────────────────────

bool GlyphBaker::makeBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage,
                            bool hostVisible, Buf& out) {
  VkBufferCreateInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bi.size  = bytes;
  bi.usage = usage;
  if (vkCreateBuffer(device_, &bi, nullptr, &out.buf) != VK_SUCCESS) return false;

  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(device_, out.buf, &req);
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = vfeFindMemoryType(
      physical_, req.memoryTypeBits,
      hostVisible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                  : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(device_, &ai, nullptr, &out.mem) != VK_SUCCESS) {
    vkDestroyBuffer(device_, out.buf, nullptr);
    out.buf = VK_NULL_HANDLE;
    return false;
  }
  vkBindBufferMemory(device_, out.buf, out.mem, 0);
  if (hostVisible) vkMapMemory(device_, out.mem, 0, bytes, 0, &out.mapped);
  return true;
}

void GlyphBaker::dropBuffer(Buf& b) {
  if (b.mapped) { vkUnmapMemory(device_, b.mem); b.mapped = nullptr; }
  if (b.buf) { vkDestroyBuffer(device_, b.buf, nullptr); b.buf = VK_NULL_HANDLE; }
  if (b.mem) { vkFreeMemory(device_, b.mem, nullptr); b.mem = VK_NULL_HANDLE; }
}

bool GlyphBaker::init(VkDevice device, VkPhysicalDevice physicalDevice,
                      AssetReader& assets, VkCommandPool pool, VkQueue queue) {
  device_ = device;
  physical_ = physicalDevice;
  pool_ = pool;
  queue_ = queue;

  const VkBufferUsageFlags kStorage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  if (!makeBuffer((VkDeviceSize)kMaxEdges * 16, kStorage, true, edges_) ||
      !makeBuffer((VkDeviceSize)kMaxEdges * 4, kStorage, true, edgeCell_) ||
      !makeBuffer((VkDeviceSize)kMaxCells * sizeof(CellDesc), kStorage, true, cells_) ||
      !makeBuffer((VkDeviceSize)kAccInts * 4,
                  kStorage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, acc_) ||
      !makeBuffer((VkDeviceSize)kCovBytes,
                  kStorage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, cov_) ||
      !makeBuffer(4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, blit_)) {
    VFE_LOGE("Baker", "GlyphBaker: cannot allocate scratch buffers");
    destroy();
    return false;
  }

  VkDescriptorSetLayoutBinding binds[kBindings]{};
  for (uint32_t i = 0; i < kBindings; i++) {
    binds[i].binding = i;
    binds[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binds[i].descriptorCount = 1;
    binds[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
  }
  VkDescriptorSetLayoutCreateInfo sli{};
  sli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  sli.bindingCount = kBindings;
  sli.pBindings = binds;
  if (vkCreateDescriptorSetLayout(device_, &sli, nullptr, &setLayout_) != VK_SUCCESS) {
    destroy();
    return false;
  }

  VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kBindings};
  VkDescriptorPoolCreateInfo pi{};
  pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pi.maxSets = 1;
  pi.poolSizeCount = 1;
  pi.pPoolSizes = &ps;
  if (vkCreateDescriptorPool(device_, &pi, nullptr, &descPool_) != VK_SUCCESS) {
    destroy();
    return false;
  }
  VkDescriptorSetAllocateInfo dai{};
  dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dai.descriptorPool = descPool_;
  dai.descriptorSetCount = 1;
  dai.pSetLayouts = &setLayout_;
  if (vkAllocateDescriptorSets(device_, &dai, &descSet_) != VK_SUCCESS) {
    destroy();
    return false;
  }

  const VkBuffer bufs[kBindings] = {edges_.buf, edgeCell_.buf, cells_.buf,
                                    acc_.buf, cov_.buf};
  VkDescriptorBufferInfo dbi[kBindings]{};
  VkWriteDescriptorSet writes[kBindings]{};
  for (uint32_t i = 0; i < kBindings; i++) {
    dbi[i].buffer = bufs[i];
    dbi[i].offset = 0;
    dbi[i].range = VK_WHOLE_SIZE;
    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[i].dstSet = descSet_;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[i].pBufferInfo = &dbi[i];
  }
  vkUpdateDescriptorSets(device_, kBindings, writes, 0, nullptr);

  VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(uint32_t) * 4};
  VkPipelineLayoutCreateInfo pli{};
  pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &setLayout_;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &pcr;
  if (vkCreatePipelineLayout(device_, &pli, nullptr, &pipeLayout_) != VK_SUCCESS) {
    destroy();
    return false;
  }

  VkShaderModule msArea = vfeLoadShaderModule(device_, assets, "shaders/glyph_area.spv");
  VkShaderModule msRes  = vfeLoadShaderModule(device_, assets, "shaders/glyph_resolve.spv");
  if (!msArea || !msRes) {
    VFE_LOGE("Baker", "GlyphBaker: cannot load glyph_area.spv / glyph_resolve.spv");
    if (msArea) vkDestroyShaderModule(device_, msArea, nullptr);
    if (msRes)  vkDestroyShaderModule(device_, msRes, nullptr);
    destroy();
    return false;
  }
  VkComputePipelineCreateInfo cpi[2]{};
  for (int i = 0; i < 2; i++) {
    cpi[i].sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi[i].stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi[i].stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi[i].stage.pName = "main";
    cpi[i].layout = pipeLayout_;
  }
  cpi[0].stage.module = msArea;
  cpi[1].stage.module = msRes;
  VkPipeline pipes[2]{};
  const VkResult pr = vkCreateComputePipelines(device_, VK_NULL_HANDLE, 2, cpi,
                                               nullptr, pipes);
  vkDestroyShaderModule(device_, msArea, nullptr);
  vkDestroyShaderModule(device_, msRes, nullptr);
  if (pr != VK_SUCCESS) {
    VFE_LOGE("Baker", "GlyphBaker: cannot create compute pipelines");
    destroy();
    return false;
  }
  pipeArea_ = pipes[0];
  pipeResolve_ = pipes[1];
  VFE_LOGI("Baker", "GlyphBaker ready (scratch %u MB acc, %u MB cov)",
       (kAccInts * 4) >> 20, kCovBytes >> 20);
  return true;
}

void GlyphBaker::destroy() {
  if (!device_) return;
  if (pipeArea_)    { vkDestroyPipeline(device_, pipeArea_, nullptr); pipeArea_ = VK_NULL_HANDLE; }
  if (pipeResolve_) { vkDestroyPipeline(device_, pipeResolve_, nullptr); pipeResolve_ = VK_NULL_HANDLE; }
  if (pipeLayout_)  { vkDestroyPipelineLayout(device_, pipeLayout_, nullptr); pipeLayout_ = VK_NULL_HANDLE; }
  if (descPool_)    { vkDestroyDescriptorPool(device_, descPool_, nullptr); descPool_ = VK_NULL_HANDLE; descSet_ = VK_NULL_HANDLE; }
  if (setLayout_)   { vkDestroyDescriptorSetLayout(device_, setLayout_, nullptr); setLayout_ = VK_NULL_HANDLE; }
  dropBuffer(edges_); dropBuffer(edgeCell_); dropBuffer(cells_);
  dropBuffer(acc_); dropBuffer(cov_); dropBuffer(blit_);
}

// ── Barriers ────────────────────────────────────────────────────────────────

void GlyphBaker::transitionAtlas(VkCommandBuffer cmd, VkImage atlas,
                                 uint32_t layers, VkImageLayout from,
                                 VkImageLayout to) {
  VkImageMemoryBarrier b{};
  b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.oldLayout = from;
  b.newLayout = to;
  b.image = atlas;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
  const bool toDst = to == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  b.srcAccessMask = toDst ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
  b.dstAccessMask = toDst ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(
      cmd,
      toDst ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT,
      toDst ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      0, 0, nullptr, 0, nullptr, 1, &b);
}

// ── One batch ───────────────────────────────────────────────────────────────

bool GlyphBaker::flush(uint32_t cellCount, uint32_t edgeCount, uint32_t maxH,
                       uint32_t accInts,
                       const std::vector<VkBufferImageCopy>& copies,
                       VkImage atlas, uint32_t layers) {
  if (cellCount == 0) return true;

  VkCommandBufferAllocateInfo cba{};
  cba.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cba.commandPool = pool_;
  cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cba.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(device_, &cba, &cmd) != VK_SUCCESS) return false;

  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &bi);

  // The accumulator is additive, so it has to start at zero every batch.
  vkCmdFillBuffer(cmd, acc_.buf, 0, (VkDeviceSize)accInts * 4, 0);
  VkMemoryBarrier mb{};
  mb.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
  mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr,
                       0, nullptr);

  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeLayout_, 0, 1,
                          &descSet_, 0, nullptr);

  uint32_t push[4] = {edgeCount, 0, 0, 0};
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeArea_);
  vkCmdPushConstants(cmd, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
  vkCmdDispatch(cmd, (edgeCount + 63) / 64, 1, 1);

  mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr,
                       0, nullptr);

  push[0] = cellCount;
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeResolve_);
  vkCmdPushConstants(cmd, pipeLayout_, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
  vkCmdDispatch(cmd, (maxH + 63) / 64, cellCount, 1);

  mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr, 0,
                       nullptr);

  transitionAtlas(cmd, atlas, layers, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  vkCmdCopyBufferToImage(cmd, cov_.buf, atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         (uint32_t)copies.size(), copies.data());
  transitionAtlas(cmd, atlas, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  vkEndCommandBuffer(cmd);
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
  vkQueueWaitIdle(queue_);
  vkFreeCommandBuffers(device_, pool_, 1, &cmd);
  return true;
}

// ── The bake ────────────────────────────────────────────────────────────────

bool GlyphBaker::bake(const std::vector<Job>& jobs, VkImage atlas, uint32_t layers) {
  if (!ready() || jobs.empty() || atlas == VK_NULL_HANDLE) return false;

  auto* edgeDst = (float*)edges_.mapped;
  auto* cellDst = (uint32_t*)edgeCell_.mapped;
  auto* descDst = (CellDesc*)cells_.mapped;

  uint32_t nCells = 0, nEdges = 0, accUsed = 0, covUsed = 0, maxH = 0;
  std::vector<VkBufferImageCopy> copies;
  bool allOk = true;

  auto flushBatch = [&]() {
    if (nCells == 0) return;
    flush(nCells, nEdges, maxH, accUsed, copies, atlas, layers);
    nCells = nEdges = accUsed = covUsed = maxH = 0;
    copies.clear();
  };

  for (const Job& j : jobs) {
    if (!j.glyph || j.glyph->w <= 0 || j.glyph->h <= 0) continue;
    const uint32_t w = (uint32_t)j.glyph->w, h = (uint32_t)j.glyph->h;
    const uint32_t edgeN = (uint32_t)j.glyph->edges.size();
    const uint32_t accN  = (w + 1) * h;
    const uint32_t rowStride = (w + 3u) & ~3u;
    const uint32_t covN = rowStride * h;

    // One glyph that cannot fit an empty batch would loop forever.
    if (edgeN > kMaxEdges || accN > kAccInts || covN > kCovBytes) {
      VFE_LOGE("Baker", "GlyphBaker: %ux%u glyph with %u edges exceeds the batch scratch", w, h, edgeN);
      allOk = false;
      continue;
    }
    if (nCells + 1 > kMaxCells || nEdges + edgeN > kMaxEdges ||
        accUsed + accN > kAccInts || covUsed + covN > kCovBytes) {
      flushBatch();
    }

    descDst[nCells] = CellDesc{w, h, accUsed, covUsed};
    for (uint32_t e = 0; e < edgeN; ++e) {
      const vfe::AreaEdge& ed = j.glyph->edges[e];
      float* p = edgeDst + (size_t)(nEdges + e) * 4;
      p[0] = ed.x0; p[1] = ed.y0; p[2] = ed.x1; p[3] = ed.y1;
      cellDst[nEdges + e] = nCells;
    }

    VkBufferImageCopy c{};
    c.bufferOffset = covUsed;
    c.bufferRowLength = rowStride;      // texels; R8 is one byte each
    c.bufferImageHeight = h;
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, j.page, 1};
    c.imageOffset = {(int32_t)j.x, (int32_t)j.y, 0};
    c.imageExtent = {w, h, 1};
    copies.push_back(c);

    nCells++;
    nEdges += edgeN;
    accUsed += accN;
    covUsed += covN;
    if (h > maxH) maxH = h;
  }
  flushBatch();
  return allOk;
}

bool GlyphBaker::blit(const uint8_t* pixels, uint32_t w, uint32_t h, uint32_t page,
                      uint32_t x, uint32_t y, VkImage atlas, uint32_t layers) {
  if (!ready() || !pixels || w == 0 || h == 0) return false;
  const uint32_t rowStride = (w + 3u) & ~3u;
  if ((size_t)rowStride * h > 4096) return false;

  auto* dst = (uint8_t*)blit_.mapped;
  std::memset(dst, 0, (size_t)rowStride * h);
  for (uint32_t r = 0; r < h; ++r)
    std::memcpy(dst + (size_t)r * rowStride, pixels + (size_t)r * w, w);

  VkCommandBufferAllocateInfo cba{};
  cba.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cba.commandPool = pool_;
  cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cba.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(device_, &cba, &cmd) != VK_SUCCESS) return false;
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &bi);

  transitionAtlas(cmd, atlas, layers, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  VkBufferImageCopy c{};
  c.bufferRowLength = rowStride;
  c.bufferImageHeight = h;
  c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, page, 1};
  c.imageOffset = {(int32_t)x, (int32_t)y, 0};
  c.imageExtent = {w, h, 1};
  vkCmdCopyBufferToImage(cmd, blit_.buf, atlas, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
  transitionAtlas(cmd, atlas, layers, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  vkEndCommandBuffer(cmd);
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
  vkQueueWaitIdle(queue_);
  vkFreeCommandBuffers(device_, pool_, 1, &cmd);
  return true;
}
