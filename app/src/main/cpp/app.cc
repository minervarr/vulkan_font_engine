#include "app.hh"
#include <android/asset_manager.h>
#include <android/log.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <vector>

#define TAG  "App"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR,   TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,    TAG, __VA_ARGS__)

// ── Vulkan validation debug messenger ─────────────────────────────────────────

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT      severity,
    VkDebugUtilsMessageTypeFlagsEXT             /*type*/,
    const VkDebugUtilsMessengerCallbackDataEXT* data,
    void* /*user*/)
{
    int prio = (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)   ? ANDROID_LOG_ERROR
             : (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) ? ANDROID_LOG_WARN
             : (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT)    ? ANDROID_LOG_INFO
             :                                                                 ANDROID_LOG_DEBUG;
    __android_log_print(prio, "VkValid", "%s", data->pMessage);
    return VK_FALSE;
}

static std::vector<char> readAsset(AAssetManager* mgr, const std::string& filename) {
  AAsset* asset = AAssetManager_open(mgr, filename.c_str(), AASSET_MODE_BUFFER);
  if (!asset) { LOGE("Cannot open asset: %s", filename.c_str()); exit(1); }
  size_t size = AAsset_getLength(asset);
  std::vector<char> buf(size);
  AAsset_read(asset, buf.data(), size);
  AAsset_close(asset);
  return buf;
}

uint32_t App::findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags required) {
  VkPhysicalDeviceMemoryProperties memProps{};
  vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProps);
  for (uint32_t i = 0; i < memProps.memoryTypeCount; i++) {
    bool compatible = typeFilter & (1 << i);
    bool hasFlags   = (memProps.memoryTypes[i].propertyFlags & required) == required;
    if (compatible && hasFlags) return i;
  }
  LOGE("No suitable memory type"); exit(1);
}

void App::init(ANativeWindow* window, AAssetManager* mgr) {
  // ── Vulkan instance ────────────────────────────────────────────────────────
  VkApplicationInfo appInfo{};
  appInfo.sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO;
  appInfo.pApplicationName   = "VkFont";
  appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
  appInfo.apiVersion         = VK_API_VERSION_1_0;

  // Check which optional instance extensions are available
  bool hasDebugUtils = false;
  {
    uint32_t extCount = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
    std::vector<VkExtensionProperties> exts(extCount);
    vkEnumerateInstanceExtensionProperties(nullptr, &extCount, exts.data());
    for (auto& e : exts)
      if (std::string(e.extensionName) == VK_EXT_DEBUG_UTILS_EXTENSION_NAME)
        { hasDebugUtils = true; break; }
  }

  // Check which validation layers are available
  const char* validationLayer = "VK_LAYER_KHRONOS_validation";
  bool hasValidation = false;
  {
    uint32_t layerCount = 0;
    vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    std::vector<VkLayerProperties> layers(layerCount);
    vkEnumerateInstanceLayerProperties(&layerCount, layers.data());
    for (auto& l : layers)
      if (std::string(l.layerName) == validationLayer) { hasValidation = true; break; }
  }
  LOGI("Validation layer %s, VK_EXT_debug_utils %s",
       hasValidation ? "AVAILABLE" : "not available",
       hasDebugUtils ? "AVAILABLE" : "not available");

  std::vector<const char*> extensions = {VK_KHR_SURFACE_EXTENSION_NAME,
                                          VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
  if (hasDebugUtils) extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

  VkInstanceCreateInfo createInfo{};
  createInfo.sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
  createInfo.pApplicationInfo        = &appInfo;
  createInfo.enabledExtensionCount   = (uint32_t)extensions.size();
  createInfo.ppEnabledExtensionNames = extensions.data();
  if (hasValidation) {
    createInfo.enabledLayerCount   = 1;
    createInfo.ppEnabledLayerNames = &validationLayer;
  }
  if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
    LOGE("Failed to create Vulkan instance"); exit(1);
  }

  // Attach debug messenger if both extension and layer are present
  if (hasDebugUtils && hasValidation) {
    auto createFn = (PFN_vkCreateDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    if (createFn) {
      VkDebugUtilsMessengerCreateInfoEXT mci{};
      mci.sType           = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
      mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT    |
                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
      mci.messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT     |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT  |
                            VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
      mci.pfnUserCallback = debugCallback;
      createFn(instance, &mci, nullptr, &debugMessenger);
      LOGI("Debug messenger attached");
    }
  }

  // ── Surface ────────────────────────────────────────────────────────────────
  VkAndroidSurfaceCreateInfoKHR surfaceInfo{};
  surfaceInfo.sType  = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
  surfaceInfo.window = window;
  if (vkCreateAndroidSurfaceKHR(instance, &surfaceInfo, nullptr, &surface) != VK_SUCCESS) {
    LOGE("Failed to create Android surface"); exit(1);
  }

  // ── Physical device ────────────────────────────────────────────────────────
  uint32_t count{};
  vkEnumeratePhysicalDevices(instance, &count, nullptr);
  if (count == 0) { LOGE("No Vulkan-capable GPU"); exit(1); }
  std::vector<VkPhysicalDevice> devices(count);
  vkEnumeratePhysicalDevices(instance, &count, devices.data());
  for (VkPhysicalDevice dev : devices) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(dev, &props);
    if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
      physicalDevice = dev; break;
    }
    if (physicalDevice == VK_NULL_HANDLE) physicalDevice = dev;
  }
  if (physicalDevice == VK_NULL_HANDLE) { LOGE("No physical device selected"); exit(1); }

  // ── Queue families ─────────────────────────────────────────────────────────
  uint32_t qCount{};
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qCount, nullptr);
  std::vector<VkQueueFamilyProperties> families(qCount);
  vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &qCount, families.data());
  for (uint32_t i = 0; i < qCount; i++) {
    if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) graphicsFamily = i;
    VkBool32 supported = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice, i, surface, &supported);
    if (supported) presentFamily = i;
  }
  if (graphicsFamily == UINT32_MAX || presentFamily == UINT32_MAX) {
    LOGE("Missing queue families"); exit(1);
  }

  // ── Logical device ─────────────────────────────────────────────────────────
  float priority = 1.0f;
  std::set<uint32_t> uniqueFamilies = {graphicsFamily, presentFamily};
  std::vector<VkDeviceQueueCreateInfo> queueInfos;
  for (uint32_t fam : uniqueFamilies) {
    VkDeviceQueueCreateInfo qi{};
    qi.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qi.queueFamilyIndex = fam;
    qi.queueCount       = 1;
    qi.pQueuePriorities = &priority;
    queueInfos.push_back(qi);
  }
  const char* devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  VkDeviceCreateInfo devInfo{};
  devInfo.sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  devInfo.queueCreateInfoCount    = (uint32_t)queueInfos.size();
  devInfo.pQueueCreateInfos       = queueInfos.data();
  devInfo.enabledExtensionCount   = std::size(devExts);
  devInfo.ppEnabledExtensionNames = devExts;
  if (vkCreateDevice(physicalDevice, &devInfo, nullptr, &logicalDevice) != VK_SUCCESS) {
    LOGE("Failed to create logical device"); exit(1);
  }
  vkGetDeviceQueue(logicalDevice, graphicsFamily, 0, &graphicsQueue);
  vkGetDeviceQueue(logicalDevice, presentFamily,  0, &presentQueue);

  // ── Swapchain ──────────────────────────────────────────────────────────────
  VkSurfaceCapabilitiesKHR caps{};
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface, &caps);

  uint32_t fmtCount{};
  vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &fmtCount, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(fmtCount);
  vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface, &fmtCount, formats.data());

  uint32_t modeCount{};
  vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &modeCount, nullptr);
  std::vector<VkPresentModeKHR> modes(modeCount);
  vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface, &modeCount, modes.data());

  VkSurfaceFormatKHR chosenFmt = formats[0];
  for (auto& f : formats)
    if (f.format == VK_FORMAT_B8G8R8A8_SRGB &&
        f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosenFmt = f; break; }
  swapchainFormat = chosenFmt.format;

  VkPresentModeKHR chosenMode = VK_PRESENT_MODE_FIFO_KHR;
  for (auto m : modes) if (m == VK_PRESENT_MODE_MAILBOX_KHR) { chosenMode = m; break; }

  swapchainExtent = caps.currentExtent;

  uint32_t imgCount = caps.minImageCount + 1;
  if (caps.maxImageCount > 0 && imgCount > caps.maxImageCount) imgCount = caps.maxImageCount;

  VkSwapchainCreateInfoKHR swapInfo{};
  swapInfo.sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  swapInfo.surface          = surface;
  swapInfo.minImageCount    = imgCount;
  swapInfo.imageFormat      = chosenFmt.format;
  swapInfo.imageColorSpace  = chosenFmt.colorSpace;
  swapInfo.imageExtent      = swapchainExtent;
  swapInfo.imageArrayLayers = 1;
  swapInfo.imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  swapInfo.preTransform     = caps.currentTransform;
  swapInfo.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  swapInfo.presentMode      = chosenMode;
  swapInfo.clipped          = VK_TRUE;
  swapInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  if (vkCreateSwapchainKHR(logicalDevice, &swapInfo, nullptr, &swapchain) != VK_SUCCESS) {
    LOGE("Failed to create swapchain"); exit(1);
  }

  uint32_t imgCnt{};
  vkGetSwapchainImagesKHR(logicalDevice, swapchain, &imgCnt, nullptr);
  swapchainImages.resize(imgCnt);
  vkGetSwapchainImagesKHR(logicalDevice, swapchain, &imgCnt, swapchainImages.data());

  // ── Command pool ───────────────────────────────────────────────────────────
  VkCommandPoolCreateInfo cpInfo{};
  cpInfo.sType            = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpInfo.flags            = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpInfo.queueFamilyIndex = graphicsFamily;
  if (vkCreateCommandPool(logicalDevice, &cpInfo, nullptr, &commandPool) != VK_SUCCESS) {
    LOGE("Failed to create command pool"); exit(1);
  }

  // ── Renderer (compute pipelines) ──────────────────────────────────────────
  renderer.init(logicalDevice, physicalDevice, mgr,
                swapchainExtent.width, swapchainExtent.height);

  // Transition output image layout (one-shot command)
  {
    VkCommandBufferAllocateInfo transAlloc{};
    transAlloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    transAlloc.commandPool        = commandPool;
    transAlloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    transAlloc.commandBufferCount = 1;
    VkCommandBuffer transCmd;
    vkAllocateCommandBuffers(logicalDevice, &transAlloc, &transCmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(transCmd, &bi);
    renderer.transitionOutputImageInitial(transCmd);
    vkEndCommandBuffer(transCmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers    = &transCmd;
    vkQueueSubmit(graphicsQueue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphicsQueue);
    vkFreeCommandBuffers(logicalDevice, commandPool, 1, &transCmd);
  }

  // ── Swapchain image views ──────────────────────────────────────────────────
  swapchainImageViews.resize(swapchainImages.size());
  for (uint32_t i = 0; i < swapchainImages.size(); i++) {
    VkImageViewCreateInfo vi{};
    vi.sType                           = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image                           = swapchainImages[i];
    vi.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
    vi.format                          = swapchainFormat;
    vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel   = 0;
    vi.subresourceRange.levelCount     = 1;
    vi.subresourceRange.baseArrayLayer = 0;
    vi.subresourceRange.layerCount     = 1;
    if (vkCreateImageView(logicalDevice, &vi, nullptr, &swapchainImageViews[i]) != VK_SUCCESS) {
      LOGE("Failed to create image view"); exit(1);
    }
  }

  // ── Render pass ────────────────────────────────────────────────────────────
  VkAttachmentDescription colorAtt{};
  colorAtt.format         = swapchainFormat;
  colorAtt.samples        = VK_SAMPLE_COUNT_1_BIT;
  colorAtt.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
  colorAtt.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
  colorAtt.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
  colorAtt.finalLayout    = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

  VkAttachmentReference colorRef{};
  colorRef.attachment = 0;
  colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments    = &colorRef;

  VkRenderPassCreateInfo rpInfo{};
  rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpInfo.attachmentCount = 1;
  rpInfo.pAttachments    = &colorAtt;
  rpInfo.subpassCount    = 1;
  rpInfo.pSubpasses      = &subpass;
  if (vkCreateRenderPass(logicalDevice, &rpInfo, nullptr, &renderPass) != VK_SUCCESS) {
    LOGE("Failed to create render pass"); exit(1);
  }

  // ── Framebuffers ───────────────────────────────────────────────────────────
  framebuffers.resize(swapchainImageViews.size());
  for (uint32_t i = 0; i < swapchainImageViews.size(); i++) {
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass      = renderPass;
    fbInfo.attachmentCount = 1;
    fbInfo.pAttachments    = &swapchainImageViews[i];
    fbInfo.width           = swapchainExtent.width;
    fbInfo.height          = swapchainExtent.height;
    fbInfo.layers          = 1;
    if (vkCreateFramebuffer(logicalDevice, &fbInfo, nullptr, &framebuffers[i]) != VK_SUCCESS) {
      LOGE("Failed to create framebuffer"); exit(1);
    }
  }

  // ── Font ───────────────────────────────────────────────────────────────────
  {
    AAsset* a = AAssetManager_open(mgr, "fonts/font.otf", AASSET_MODE_BUFFER);
    if (a) {
      size_t sz = AAsset_getLength(a);
      std::vector<uint8_t> buf(sz);
      AAsset_read(a, buf.data(), sz);
      AAsset_close(a);
      if (!font.loadFromMemory(buf.data(), sz))
        LOGE("Font load failed — falling back to stroke glyphs");
      else
        LOGI("Font loaded OK (%zu bytes)", sz);
    } else {
      LOGE("fonts/font.otf not found — using stroke glyphs");
    }
  }

  // ── Demo scene init ────────────────────────────────────────────────────────
  demo.init(swapchainExtent.width, swapchainExtent.height);

  // ── Composite descriptor set ───────────────────────────────────────────────
  {
    VkDescriptorSetLayoutBinding binding{};
    binding.binding         = 0;
    binding.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags      = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo li{};
    li.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 1;
    li.pBindings    = &binding;
    if (vkCreateDescriptorSetLayout(logicalDevice, &li, nullptr, &compositeSetLayout) != VK_SUCCESS) {
      LOGE("Failed to create composite descriptor set layout"); exit(1);
    }

    VkDescriptorPoolSize ps{};
    ps.type            = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ps.descriptorCount = 1;
    VkDescriptorPoolCreateInfo pi{};
    pi.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pi.poolSizeCount = 1;
    pi.pPoolSizes    = &ps;
    pi.maxSets       = 1;
    if (vkCreateDescriptorPool(logicalDevice, &pi, nullptr, &compositePool) != VK_SUCCESS) {
      LOGE("Failed to create composite descriptor pool"); exit(1);
    }

    VkDescriptorSetAllocateInfo sa{};
    sa.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    sa.descriptorPool     = compositePool;
    sa.descriptorSetCount = 1;
    sa.pSetLayouts        = &compositeSetLayout;
    if (vkAllocateDescriptorSets(logicalDevice, &sa, &compositeSet) != VK_SUCCESS) {
      LOGE("Failed to allocate composite descriptor set"); exit(1);
    }

    VkDescriptorImageInfo imgInfo{};
    imgInfo.sampler     = renderer.outputSampler;
    imgInfo.imageView   = renderer.outputImageView;
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet wr{};
    wr.sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    wr.dstSet          = compositeSet;
    wr.dstBinding      = 0;
    wr.descriptorCount = 1;
    wr.descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo      = &imgInfo;
    vkUpdateDescriptorSets(logicalDevice, 1, &wr, 0, nullptr);
  }

  // ── Composite graphics pipeline ────────────────────────────────────────────
  {
    VkPipelineLayoutCreateInfo pli{};
    pli.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts    = &compositeSetLayout;
    if (vkCreatePipelineLayout(logicalDevice, &pli, nullptr, &pipelineLayout) != VK_SUCCESS) {
      LOGE("Failed to create composite pipeline layout"); exit(1);
    }

    auto loadModule = [&](const char* path) -> VkShaderModule {
      auto code = readAsset(mgr, path);
      VkShaderModuleCreateInfo si{};
      si.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
      si.codeSize = code.size();
      si.pCode    = reinterpret_cast<const uint32_t*>(code.data());
      VkShaderModule m = VK_NULL_HANDLE;
      if (vkCreateShaderModule(logicalDevice, &si, nullptr, &m) != VK_SUCCESS) {
        LOGE("Failed to create shader module: %s", path); exit(1);
      }
      return m;
    };

    VkShaderModule vertMod = loadModule("shaders/composite_vert.spv");
    VkShaderModule fragMod = loadModule("shaders/composite_frag.spv");

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage  = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertMod;
    stages[0].pName  = "main";
    stages[1].sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage  = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragMod;
    stages[1].pName  = "main";

    VkPipelineVertexInputStateCreateInfo   vi{};  vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{};  ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo      vp{};  vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{};  rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo   ms{};  ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState    ba{};
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|
                        VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo    cb{};  cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dynStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo       dy{};  dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dy.dynamicStateCount = 2; dy.pDynamicStates = dynStates;

    VkGraphicsPipelineCreateInfo gpci{};
    gpci.sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpci.stageCount          = 2;
    gpci.pStages             = stages;
    gpci.pVertexInputState   = &vi;
    gpci.pInputAssemblyState = &ia;
    gpci.pViewportState      = &vp;
    gpci.pRasterizationState = &rs;
    gpci.pMultisampleState   = &ms;
    gpci.pColorBlendState    = &cb;
    gpci.pDynamicState       = &dy;
    gpci.layout              = pipelineLayout;
    gpci.renderPass          = renderPass;
    gpci.subpass             = 0;

    if (vkCreateGraphicsPipelines(logicalDevice, VK_NULL_HANDLE, 1, &gpci,
                                  nullptr, &graphicsPipeline) != VK_SUCCESS) {
      LOGE("Failed to create composite graphics pipeline"); exit(1);
    }
    vkDestroyShaderModule(logicalDevice, vertMod, nullptr);
    vkDestroyShaderModule(logicalDevice, fragMod, nullptr);
  }

  // ── Command buffer + sync objects ──────────────────────────────────────────
  VkCommandBufferAllocateInfo cbAlloc{};
  cbAlloc.sType              = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbAlloc.commandPool        = commandPool;
  cbAlloc.level              = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbAlloc.commandBufferCount = 1;
  if (vkAllocateCommandBuffers(logicalDevice, &cbAlloc, &commandBuffer) != VK_SUCCESS) {
    LOGE("Failed to allocate command buffer"); exit(1);
  }

  VkSemaphoreCreateInfo semInfo{}; semInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  VkFenceCreateInfo     fenInfo{}; fenInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fenInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

  vkCreateSemaphore(logicalDevice, &semInfo, nullptr, &imageAvailableSemaphore);
  vkCreateSemaphore(logicalDevice, &semInfo, nullptr, &renderFinishedSemaphore);
  vkCreateFence    (logicalDevice, &fenInfo, nullptr, &inFlightFence);
  LOGI("App::init complete — swapchain %ux%u", swapchainExtent.width, swapchainExtent.height);
}

void App::drawFrame() {
  vkWaitForFences(logicalDevice, 1, &inFlightFence, VK_TRUE, UINT64_MAX);
  vkResetFences  (logicalDevice, 1, &inFlightFence);

  uint32_t imageIndex;
  vkAcquireNextImageKHR(logicalDevice, swapchain, UINT64_MAX,
                        imageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex);

  vkResetCommandBuffer(commandBuffer, 0);

  if (demo.dirty) {
    demo.rebuildCurves(scratchCurves, font.ftFace ? &font : nullptr);
    uint32_t curveCount = (uint32_t)(scratchCurves.size() / Renderer::CURVE_FLOATS);
    LOGI("rebuildCurves: %zu floats = %u curves",
         scratchCurves.size(), curveCount);
    renderer.uploadCurves(scratchCurves.data(), curveCount);
    demo.dirty   = false;
    computeDirty = true;   // curves changed → recompute the cached image once
  }

  VkCommandBufferBeginInfo bi{}; bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  vkBeginCommandBuffer(commandBuffer, &bi);

  // Compute only runs when the scene changed; otherwise we re-composite the
  // image computed on a previous frame (it persists in GENERAL/READ layout).
  if (computeDirty) {
    // If the image is currently sampled (READ), flip it back to GENERAL so the
    // compute shader can store into it again.
    if (outputLayout != VK_IMAGE_LAYOUT_GENERAL) {
      VkImageMemoryBarrier b{};
      b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      b.oldLayout           = outputLayout;
      b.newLayout           = VK_IMAGE_LAYOUT_GENERAL;
      b.srcAccessMask       = VK_ACCESS_SHADER_READ_BIT;
      b.dstAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
      b.image               = renderer.outputImage;
      b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      vkCmdPipelineBarrier(commandBuffer,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &b);
    }

    renderer.dispatch(commandBuffer);

    // GENERAL -> SHADER_READ_ONLY_OPTIMAL for the composite fragment shader.
    VkImageMemoryBarrier b{};
    b.sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout           = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout           = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask       = VK_ACCESS_SHADER_READ_BIT;
    b.image               = renderer.outputImage;
    b.subresourceRange    = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    vkCmdPipelineBarrier(commandBuffer,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);

    outputLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    computeDirty = false;
  }

  VkClearValue clearColor = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
  VkRenderPassBeginInfo rpbi{};
  rpbi.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rpbi.renderPass        = renderPass;
  rpbi.framebuffer       = framebuffers[imageIndex];
  rpbi.renderArea.offset = {0, 0};
  rpbi.renderArea.extent = swapchainExtent;
  rpbi.clearValueCount   = 1;
  rpbi.pClearValues      = &clearColor;
  vkCmdBeginRenderPass(commandBuffer, &rpbi, VK_SUBPASS_CONTENTS_INLINE);

  VkViewport vp{0.f, 0.f, (float)swapchainExtent.width, (float)swapchainExtent.height, 0.f, 1.f};
  VkRect2D   sc{{0,0}, swapchainExtent};
  vkCmdSetViewport(commandBuffer, 0, 1, &vp);
  vkCmdSetScissor (commandBuffer, 0, 1, &sc);
  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          pipelineLayout, 0, 1, &compositeSet, 0, nullptr);
  vkCmdDraw(commandBuffer, 3, 1, 0, 0);
  vkCmdEndRenderPass(commandBuffer);

  // No layout flip back to GENERAL here: the image stays in READ between frames
  // so subsequent repaint frames can re-sample it.  A future recompute pulls it
  // back to GENERAL via the pre-dispatch barrier above.

  vkEndCommandBuffer(commandBuffer);

  VkSemaphore waitSems[]   = {imageAvailableSemaphore};
  VkPipelineStageFlags wst = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSemaphore signalSems[] = {renderFinishedSemaphore};
  VkSubmitInfo si{};
  si.sType                = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.waitSemaphoreCount   = 1;
  si.pWaitSemaphores      = waitSems;
  si.pWaitDstStageMask    = &wst;
  si.commandBufferCount   = 1;
  si.pCommandBuffers      = &commandBuffer;
  si.signalSemaphoreCount = 1;
  si.pSignalSemaphores    = signalSems;
  vkQueueSubmit(graphicsQueue, 1, &si, inFlightFence);

  VkPresentInfoKHR pi{};
  pi.sType              = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  pi.waitSemaphoreCount = 1;
  pi.pWaitSemaphores    = signalSems;
  pi.swapchainCount     = 1;
  pi.pSwapchains        = &swapchain;
  pi.pImageIndices      = &imageIndex;
  vkQueuePresentKHR(presentQueue, &pi);

  if (pendingFrames > 0) pendingFrames--;
}

void App::cleanup() {
  vkDestroyPipeline           (logicalDevice, graphicsPipeline,     nullptr);
  vkDestroyPipelineLayout     (logicalDevice, pipelineLayout,       nullptr);
  vkDestroyDescriptorPool     (logicalDevice, compositePool,        nullptr);
  vkDestroyDescriptorSetLayout(logicalDevice, compositeSetLayout,   nullptr);
  for (auto& fb : framebuffers) vkDestroyFramebuffer(logicalDevice, fb, nullptr);
  vkDestroyRenderPass         (logicalDevice, renderPass,           nullptr);
  for (auto& iv : swapchainImageViews) vkDestroyImageView(logicalDevice, iv, nullptr);
  vkDestroySemaphore          (logicalDevice, imageAvailableSemaphore, nullptr);
  vkDestroySemaphore          (logicalDevice, renderFinishedSemaphore, nullptr);
  vkDestroyFence              (logicalDevice, inFlightFence,         nullptr);
  vkDestroyCommandPool        (logicalDevice, commandPool,           nullptr);
  vkDestroySwapchainKHR       (logicalDevice, swapchain,            nullptr);
  renderer.cleanup();
  font.destroy();
  vkDestroyDevice             (logicalDevice, nullptr);
  vkDestroySurfaceKHR         (instance, surface,                   nullptr);
  if (debugMessenger != VK_NULL_HANDLE) {
    auto destroyFn = (PFN_vkDestroyDebugUtilsMessengerEXT)
        vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (destroyFn) destroyFn(instance, debugMessenger, nullptr);
  }
  vkDestroyInstance           (instance, nullptr);
}

void App::setInsets(uint32_t top, uint32_t bottom, uint32_t left, uint32_t right) {
  demo.setInsets(top, bottom, left, right);
  dirty = true;
}

void App::requestRedraw() {
  // One frame per swapchain image (+1 for the image currently on screen) so a
  // single scene change repaints all of them before we idle.
  pendingFrames = (uint32_t)swapchainImages.size() + 1;
}
