#pragma once
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <android/native_window.h>
#include <android/asset_manager.h>
#include <vector>
#include "curve_rasterizer.hh"
#include "font.hh"
#include "demo.hh"
#include "android_platform.hh"

class App {
 private:
  VkInstance                   instance        = VK_NULL_HANDLE;
  VkDebugUtilsMessengerEXT     debugMessenger  = VK_NULL_HANDLE;
  VkSurfaceKHR     surface         = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice  = VK_NULL_HANDLE;
  uint32_t         graphicsFamily  = UINT32_MAX;
  uint32_t         presentFamily   = UINT32_MAX;
  VkDevice         logicalDevice   = VK_NULL_HANDLE;
  VkSwapchainKHR   swapchain       = VK_NULL_HANDLE;
  VkFormat         swapchainFormat;
  VkExtent2D       swapchainExtent;
  std::vector<VkImage>       swapchainImages;
  std::vector<VkImageView>   swapchainImageViews;
  VkRenderPass     renderPass      = VK_NULL_HANDLE;
  std::vector<VkFramebuffer> framebuffers;
  VkPipelineLayout pipelineLayout  = VK_NULL_HANDLE;
  VkPipeline       graphicsPipeline= VK_NULL_HANDLE;
  VkCommandPool    commandPool     = VK_NULL_HANDLE;
  VkCommandBuffer  commandBuffer   = VK_NULL_HANDLE;
  VkSemaphore      imageAvailableSemaphore = VK_NULL_HANDLE;
  VkSemaphore      renderFinishedSemaphore = VK_NULL_HANDLE;
  VkFence          inFlightFence   = VK_NULL_HANDLE;
  VkQueue          graphicsQueue   = VK_NULL_HANDLE;
  VkQueue          presentQueue    = VK_NULL_HANDLE;

  VkDescriptorSetLayout compositeSetLayout = VK_NULL_HANDLE;
  VkDescriptorPool      compositePool      = VK_NULL_HANDLE;
  VkDescriptorSet       compositeSet       = VK_NULL_HANDLE;

  // Must outlive rasterizer, which keeps a pointer to it across init().
  AndroidAssetReader assetReader{nullptr};
  CurveRasterizer rasterizer;
  Font     font;
  Demo     demo;
  std::vector<float> scratchCurves;

  uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags required);

 public:
  void init(ANativeWindow* window, AAssetManager* mgr);
  void cleanup();
  void drawFrame();
  void setInsets(uint32_t top, uint32_t bottom, uint32_t left, uint32_t right);

  // A redraw must refresh every swapchain image, not just one — otherwise the
  // images we skip stay black and the static scene appears to "blink" as the
  // compositor cycles through them.  requestRedraw() queues one frame per
  // swapchain image; drawFrame() consumes one each call until idle again.
  void requestRedraw();
  bool needsDraw() const { return pendingFrames > 0; }

  bool     initialized   = false;
  bool     dirty         = false;
  uint32_t pendingFrames = 0;

  // The compute pass produces a pixel-identical image for a static scene, so it
  // runs only when the curves change (computeDirty); the cached result in
  // rasterizer.outputImage() is re-composited to each swapchain image on the other
  // pendingFrames. outputLayout tracks that image's layout across frames so we
  // stop bouncing it GENERAL<->READ every frame.
  bool          computeDirty = true;
  VkImageLayout outputLayout = VK_IMAGE_LAYOUT_GENERAL;
};
