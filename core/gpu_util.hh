#pragma once
// Small Vulkan helpers shared by the engine's GPU units (CurveRasterizer,
// MsdfTextRenderer). Free functions on purpose — the two renderers share no
// state, only these snippets of plumbing.

#include "asset_reader.hh"
#include <vulkan/vulkan.h>
#include <cstdint>

// Index of the first memory type compatible with typeBits that has all the
// required property flags, or UINT32_MAX if none exists.
uint32_t vfeFindMemoryType(VkPhysicalDevice physicalDevice, uint32_t typeBits,
                           VkMemoryPropertyFlags required);

// Loads a SPIR-V blob through the asset seam and wraps it in a shader module.
// Returns VK_NULL_HANDLE (after logging) if the asset is missing or invalid.
VkShaderModule vfeLoadShaderModule(VkDevice device, AssetReader& assets,
                                   const char* path);
