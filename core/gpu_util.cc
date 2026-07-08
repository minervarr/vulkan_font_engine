#include "gpu_util.hh"
#include "log.hh"
#include <cstring>
#include <vector>

#define LOGE(...) VFE_LOGE("GpuUtil", __VA_ARGS__)

uint32_t vfeFindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits,
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

VkShaderModule vfeLoadShaderModule(VkDevice device, AssetReader& assets,
                                   const char* path) {
  std::vector<uint8_t> bytes;
  if (!assets.read(path, bytes) || bytes.empty()) {
    LOGE("Could not open shader asset: %s", path);
    return VK_NULL_HANDLE;
  }
  size_t size = bytes.size();
  std::vector<uint32_t> code(size / sizeof(uint32_t));
  std::memcpy(code.data(), bytes.data(), size);

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
