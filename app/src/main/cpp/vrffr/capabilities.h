#pragma once
#include <vulkan/vulkan.h>
#include <cstring>

namespace ffr {
struct FormatSupport {
  VkFormatProperties legacy{};
  VkFormatProperties2 modern{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
  VkFormatProperties3 wide{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3};
  VkImageFormatProperties2 image{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
  bool hasModern = false, hasWide = false, hasImage = false;
  VkResult imageResult = VK_ERROR_FORMAT_NOT_SUPPORTED;
  const char *source = "none";
  bool imageSupported() const {
    return hasImage && imageResult == VK_SUCCESS &&
        (image.imageFormatProperties.sampleCounts & VK_SAMPLE_COUNT_1_BIT) &&
        image.imageFormatProperties.maxExtent.width &&
        image.imageFormatProperties.maxExtent.height &&
        image.imageFormatProperties.maxArrayLayers;
  }
  bool experimentalFormatOverride() const {
    // Bypass only the FDM bit. Transfer destination and the exact image usage
    // still have to be advertised; feature/extension gates live in the layer.
    const auto transfer = VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
    return imageSupported() &&
        ((legacy.optimalTilingFeatures & transfer) ||
         (hasModern && (modern.formatProperties.optimalTilingFeatures & transfer)) ||
         (hasWide && (wide.optimalTilingFeatures & transfer)));
  }
  bool supported() const { return std::strcmp(source, "none") != 0; }
};
// Only accept a complete pair of format flags from one query, and require the
// exact image usage check to agree. Never synthesize support by OR-ing results.
inline FormatSupport queryFormat(PFN_vkGetInstanceProcAddr get, VkInstance instance,
                                 VkPhysicalDevice physical, bool wideSupported) {
  FormatSupport q;
  auto old = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(
      get(instance, "vkGetPhysicalDeviceFormatProperties"));
  if (old) old(physical, VK_FORMAT_R8G8_UNORM, &q.legacy);
  auto modern = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties2>(
      get(instance, "vkGetPhysicalDeviceFormatProperties2"));
  if (!modern) modern = reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties2>(
      get(instance, "vkGetPhysicalDeviceFormatProperties2KHR"));
  q.hasModern = modern != nullptr;
  q.hasWide = modern && wideSupported;
  if (modern) {
    q.modern.pNext = q.hasWide ? &q.wide : nullptr;
    modern(physical, VK_FORMAT_R8G8_UNORM, &q.modern);
    q.modern.pNext = nullptr;
  }
  auto image = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
      get(instance, "vkGetPhysicalDeviceImageFormatProperties2"));
  if (!image) image = reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties2>(
      get(instance, "vkGetPhysicalDeviceImageFormatProperties2KHR"));
  q.hasImage = image != nullptr;
  if (image) {
    VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
    info.format = VK_FORMAT_R8G8_UNORM;
    info.type = VK_IMAGE_TYPE_2D;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    q.imageResult = image(physical, &info, &q.image);
  }
  constexpr VkFormatFeatureFlags2 required = VK_FORMAT_FEATURE_2_FRAGMENT_DENSITY_MAP_BIT_EXT |
                                            VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
  if (q.hasImage && q.imageResult == VK_SUCCESS &&
      (q.image.imageFormatProperties.sampleCounts & VK_SAMPLE_COUNT_1_BIT) &&
      q.image.imageFormatProperties.maxExtent.width &&
      q.image.imageFormatProperties.maxExtent.height &&
      q.image.imageFormatProperties.maxArrayLayers) {
    if (q.hasWide && (q.wide.optimalTilingFeatures & required) == required)
      q.source = "properties3";
    else if (q.hasModern && (q.modern.formatProperties.optimalTilingFeatures & required) == required)
      q.source = "properties2";
    else if ((q.legacy.optimalTilingFeatures & required) == required)
      q.source = "legacy";
  }
  return q;
}
} // namespace ffr
