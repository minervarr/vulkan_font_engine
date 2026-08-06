#pragma once
#include "asset_reader.hh"
#include "glyph_raster.hh"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

// ── Glyph rasterization on the GPU ──────────────────────────────────────────
//
// Takes flattened outlines and writes 8-bit coverage straight into the atlas
// image, using the signed-area algorithm that core/area_raster.hh implements on
// the CPU and area_raster_test measures against FreeType.
//
// The split of work is the point:
//
//   CPU   load the glyph, flatten its curves (RasterFace::outline) — cheap,
//         and it is not what the bake spends its time on
//   GPU   scan conversion — the expensive part, and embarrassingly parallel
//   GPU   the copy into the atlas layers, with no readback anywhere
//
// Nothing comes back to the CPU. The atlas is written where it is used, which
// is what makes this worth doing at all: a version that rasterized on the GPU
// and then read the pixels back would spend more time moving them than
// FreeType spends making them.
//
// Batched, and the batch bound is about MEMORY. The accumulator is one int per
// pixel plus a column, so a full 8K bake would need ~700 MB of scratch at
// once; a batch at a time holds that to a fixed budget while still giving the
// device thousands of independent edges to chew on.

class GlyphBaker {
 public:
  // Where one glyph's coverage goes in the atlas.
  struct Job {
    const vfe::OutlineGlyph* glyph = nullptr;
    uint32_t page = 0;      // array layer
    uint32_t x = 0, y = 0;  // texel offset within that layer
  };

  bool init(VkDevice device, VkPhysicalDevice physicalDevice, AssetReader& assets,
            VkCommandPool pool, VkQueue queue);
  void destroy();
  bool ready() const { return pipeArea_ != VK_NULL_HANDLE; }

  // Rasterize every job and copy the result into `atlas`, an R8 2D-array image
  // with `layers` layers that is in SHADER_READ_ONLY_OPTIMAL on entry and is
  // left in it on exit. Returns false if anything was rejected; whatever DID
  // fit is still written.
  bool bake(const std::vector<Job>& jobs, VkImage atlas, uint32_t layers);

  // Write raw bytes into a cell — for the reserved opaque texel, which has no
  // outline to rasterize but still has to reach the atlas.
  bool blit(const uint8_t* pixels, uint32_t w, uint32_t h, uint32_t page,
            uint32_t x, uint32_t y, VkImage atlas, uint32_t layers);

 private:
  struct CellDesc { uint32_t w, h, accOff, covOff; };

  // Scratch budgets, per batch. Sized so a batch is a few tens of MB and a
  // large glyph still fits on its own.
  //
  // Do NOT raise these to make the bake one batch. That was tried, on the
  // theory that each batch boundary costs a device stall: at 4M edges the 8K
  // bake got SLOWER, 1450 -> 1950 ms. Batch count is not where the time goes,
  // and the extra host-visible memory costs more than the stalls it removes.
  static constexpr uint32_t kMaxEdges = 1u << 20;   // 16 MB of float4
  static constexpr uint32_t kMaxCells = 8192;
  static constexpr uint32_t kAccInts  = 24u << 20;  // 96 MB
  static constexpr uint32_t kCovBytes = 48u << 20;

  struct Buf {
    VkBuffer buf = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    void* mapped = nullptr;
  };

  bool makeBuffer(VkDeviceSize bytes, VkBufferUsageFlags usage, bool hostVisible,
                  Buf& out);
  void dropBuffer(Buf& b);
  bool flush(uint32_t cellCount, uint32_t edgeCount, uint32_t maxH,
             uint32_t accInts, const std::vector<VkBufferImageCopy>& copies,
             VkImage atlas, uint32_t layers);
  void transitionAtlas(VkCommandBuffer cmd, VkImage atlas, uint32_t layers,
                       VkImageLayout from, VkImageLayout to);

  VkDevice         device_ = VK_NULL_HANDLE;
  VkPhysicalDevice physical_ = VK_NULL_HANDLE;
  VkCommandPool    pool_ = VK_NULL_HANDLE;
  VkQueue          queue_ = VK_NULL_HANDLE;

  Buf edges_, edgeCell_, cells_, acc_, cov_, blit_;

  VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool      descPool_  = VK_NULL_HANDLE;
  VkDescriptorSet       descSet_   = VK_NULL_HANDLE;
  VkPipelineLayout      pipeLayout_ = VK_NULL_HANDLE;
  VkPipeline            pipeArea_   = VK_NULL_HANDLE;
  VkPipeline            pipeResolve_ = VK_NULL_HANDLE;
};
