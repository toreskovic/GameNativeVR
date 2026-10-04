#include "xr_vulkan_context.h"
#include "xr_vulkan_dispatch.h"
#include <algorithm>
#include <android/log.h>
#include <cstring>
#include <dlfcn.h>
#include <vector>
#define VLOG(...) __android_log_print(ANDROID_LOG_INFO, "VrVulkan", __VA_ARGS__)
namespace xrimmersive {
XrVulkanContext::~XrVulkanContext() { shutdown(); }
bool XrVulkanContext::initialize(XrInstance xr, XrSystemId system, bool subsampledSwapchains) {
  // The XR compositor may share driver-private opaque memory or resolve Vulkan
  // entry points independently. Use the platform loader for the entire Android
  // capture/generation/presentation device, regardless of the game's driver.
  library_ = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
  if (!library_) {
    VLOG("System Vulkan loader could not be loaded: %s", dlerror());
    return false;
  }
  get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
      dlsym(library_, "vkGetInstanceProcAddr"));
  if (!get)
    return false;
  PFN_xrGetVulkanGraphicsRequirements2KHR requirements{};
  PFN_xrCreateVulkanInstanceKHR createInstance{};
  PFN_xrGetVulkanGraphicsDevice2KHR graphicsDevice{};
  PFN_xrCreateVulkanDeviceKHR createDevice{};
#define XRLOAD(name, variable)                                                 \
  if (XR_FAILED(xrGetInstanceProcAddr(                                         \
          xr, #name, reinterpret_cast<PFN_xrVoidFunction *>(&variable))) ||    \
      !variable)                                                               \
  return false
  XRLOAD(xrGetVulkanGraphicsRequirements2KHR, requirements);
  XRLOAD(xrCreateVulkanInstanceKHR, createInstance);
  XRLOAD(xrGetVulkanGraphicsDevice2KHR, graphicsDevice);
  XRLOAD(xrCreateVulkanDeviceKHR, createDevice);
#undef XRLOAD
  XrGraphicsRequirementsVulkan2KHR req{
      XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
  if (XR_FAILED(requirements(xr, system, &req)))
    return false;
  auto version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
      get(nullptr, "vkEnumerateInstanceVersion"));
  uint32_t supported = VK_API_VERSION_1_0;
  if (version)
    version(&supported);
  const auto xrVersion = XR_MAKE_VERSION(VK_VERSION_MAJOR(supported),
                                         VK_VERSION_MINOR(supported), 0);
  if (xrVersion < req.minApiVersionSupported ||
      req.maxApiVersionSupported < XR_MAKE_VERSION(1, 1, 0))
    return false;
  uint32_t api = std::min(
      supported,
      VK_MAKE_VERSION(XR_VERSION_MAJOR(req.maxApiVersionSupported),
                      XR_VERSION_MINOR(req.maxApiVersionSupported), 0));
  api = std::min(api, uint32_t(VK_API_VERSION_1_2));
  if (api < VK_API_VERSION_1_1)
    return false;
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "GameNative VR";
  app.apiVersion = api;
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;
  XrVulkanInstanceCreateInfoKHR xi{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
  xi.systemId = system;
  xi.pfnGetInstanceProcAddr = get;
  xi.vulkanCreateInfo = &ci;
  VkResult result = VK_ERROR_INITIALIZATION_FAILED;
  auto xrResult = createInstance(xr, &xi, &instance, &result);
  VLOG("%s OpenXR instance: xr=%d vk=%d", driverLabel(), int(xrResult),
       int(result));
  if (XR_FAILED(xrResult) || result != VK_SUCCESS)
    return false;
  XrVulkanGraphicsDeviceGetInfoKHR gi{
      XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
  gi.systemId = system;
  gi.vulkanInstance = instance;
  if (XR_FAILED(graphicsDevice(xr, &gi, &physical)))
    return false;
  auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
      get(instance, "vkGetPhysicalDeviceProperties"));
  props(physical, &properties);
  auto enumerate = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
      get(instance, "vkEnumerateDeviceExtensionProperties"));
  uint32_t n = 0;
  enumerate(physical, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> exts(n);
  enumerate(physical, nullptr, &n, exts.data());
  auto has = [&](const char *name) {
    return std::any_of(exts.begin(), exts.end(),
                       [&](auto &e) { return !strcmp(name, e.extensionName); });
  };
  std::vector<const char *> enabled = {
      VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
      VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
      VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME};
  for (auto e : enabled)
    if (!has(e)) {
      VLOG("Missing required extension %s", e);
      return false;
    }
  for (auto e : {"VK_KHR_calibrated_timestamps",
                 VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME})
    if (has(e))
      enabled.push_back(e);
  auto queues = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
      get(instance, "vkGetPhysicalDeviceQueueFamilyProperties"));
  queues(physical, &n, nullptr);
  std::vector<VkQueueFamilyProperties> families(n);
  queues(physical, &n, families.data());
  for (family = 0; family < n; ++family)
    if ((families[family].queueFlags & 3) == 3)
      break;
  if (family == n)
    return false;
  float priority = 1;
  VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qi.queueFamilyIndex = family;
  qi.queueCount = 1;
  qi.pQueuePriorities = &priority;
  VkPhysicalDeviceFeatures available{};
  reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures>(
      get(instance, "vkGetPhysicalDeviceFeatures"))(physical, &available);
  VkPhysicalDeviceFeatures features{};
  features.shaderStorageImageExtendedFormats =
      available.shaderStorageImageExtendedFormats;
  features.shaderStorageImageWriteWithoutFormat =
      available.shaderStorageImageWriteWithoutFormat;
  if (!features.shaderStorageImageExtendedFormats ||
      !features.shaderStorageImageWriteWithoutFormat) {
    VLOG("%s lacks required storage-image features: extended=%d "
         "writeWithoutFormat=%d",
         driverLabel(), features.shaderStorageImageExtendedFormats,
         features.shaderStorageImageWriteWithoutFormat);
    return false;
  }
  VkPhysicalDeviceFragmentDensityMapFeaturesEXT density{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT};
  presentationDensity=false;
  subsampledPresentation=false;
  if(has(VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME)) {
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; f.pNext=&density;
    reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(get(instance,"vkGetPhysicalDeviceFeatures2"))(physical,&f);
    VkPhysicalDeviceFragmentDensityMapPropertiesEXT dp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};p.pNext=&dp;
    reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(get(instance,"vkGetPhysicalDeviceProperties2"))(physical,&p);
    VkFormatProperties fmt{};
    reinterpret_cast<PFN_vkGetPhysicalDeviceFormatProperties>(get(instance,"vkGetPhysicalDeviceFormatProperties"))(physical,VK_FORMAT_R8G8_UNORM,&fmt);
    VkImageFormatProperties image{};
    auto query=reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(get(instance,"vkGetPhysicalDeviceImageFormatProperties"));
    const auto result=query(physical,VK_FORMAT_R8G8_UNORM,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,0,&image);
    presentationDensity=density.fragmentDensityMap && density.fragmentDensityMapNonSubsampledImages &&
        (fmt.optimalTilingFeatures&VK_FORMAT_FEATURE_FRAGMENT_DENSITY_MAP_BIT_EXT) && result==VK_SUCCESS &&
        dp.minFragmentDensityTexelSize.width && dp.minFragmentDensityTexelSize.height;
    densityTexel=dp.minFragmentDensityTexelSize;
    density.fragmentDensityMapDynamic=VK_FALSE;
    if(presentationDensity) enabled.push_back(VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME);
    // The runtime samples the final images. Enable map2 for its subsampled
    // reconstruction path, but not deferred/dynamic maps: our map is immutable.
    subsampledPresentation=presentationDensity && subsampledSwapchains &&
        has(VK_EXT_FRAGMENT_DENSITY_MAP_2_EXTENSION_NAME);
    if(subsampledPresentation) enabled.push_back(VK_EXT_FRAGMENT_DENSITY_MAP_2_EXTENSION_NAME);
    VLOG("Presentation density: supported=%d nonSubsampled=%u format=%x query=%d texel=%ux%u",
         presentationDensity,density.fragmentDensityMapNonSubsampledImages,fmt.optimalTilingFeatures,result,
         densityTexel.width,densityTexel.height);
  }
  VLOG("Presentation subsampled capability: OpenXR=%d enabled=%d",
       subsampledSwapchains,subsampledPresentation);
  VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  di.queueCreateInfoCount = 1;
  di.pQueueCreateInfos = &qi;
  di.pEnabledFeatures = &features;
  di.pNext = presentationDensity ? &density : nullptr;
  di.enabledExtensionCount = enabled.size();
  di.ppEnabledExtensionNames = enabled.data();
  XrVulkanDeviceCreateInfoKHR xd{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
  xd.systemId = system;
  xd.pfnGetInstanceProcAddr = get;
  xd.vulkanPhysicalDevice = physical;
  xd.vulkanCreateInfo = &di;
  xrResult = createDevice(xr, &xd, &device, &result);
  VLOG("%s OpenXR device: xr=%d vk=%d GPU=%s", driverLabel(), int(xrResult),
       int(result), properties.deviceName);
  if (XR_FAILED(xrResult) || result != VK_SUCCESS)
    return false;
  if (!vkd_load(instance, device, get))
    return false;
  loadXrVulkanDispatch(instance, device, get);
  vkGetDeviceQueue(device, family, 0, &queue);
  ahbProperties =
      reinterpret_cast<PFN_vkGetAndroidHardwareBufferPropertiesANDROID>(
          vkd.GetDeviceProcAddr(device,
                                "vkGetAndroidHardwareBufferPropertiesANDROID"));
  return ahbProperties && queue;
}
VkResult XrVulkanContext::submit(uint32_t n, const VkSubmitInfo *s, VkFence f) {
  std::lock_guard<std::mutex> lock(queueMutex);
  return vkQueueSubmit(queue, n, s, f);
}
VkResult XrVulkanContext::idle() {
  std::lock_guard<std::mutex> lock(queueMutex);
  return device ? vkDeviceWaitIdle(device) : VK_SUCCESS;
}
bool XrVulkanContext::supportsSubsampledSwapchain(VkFormat format, VkExtent2D extent,
                                                uint32_t layers) const {
  if(!subsampledPresentation || !physical || !get) return false;
  auto query=reinterpret_cast<PFN_vkGetPhysicalDeviceImageFormatProperties>(
      get(instance,"vkGetPhysicalDeviceImageFormatProperties"));
  VkImageFormatProperties p{};
  const auto result=query(physical,format,VK_IMAGE_TYPE_2D,VK_IMAGE_TILING_OPTIMAL,
      VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT,
      VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT,&p);
  return result==VK_SUCCESS && extent.width<=p.maxExtent.width &&
      extent.height<=p.maxExtent.height && layers<=p.maxArrayLayers &&
      (p.sampleCounts&VK_SAMPLE_COUNT_1_BIT);
}

void XrVulkanContext::shutdown() {
  if (device && get) {
    auto gd = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        get(instance, "vkGetDeviceProcAddr"));
    reinterpret_cast<PFN_vkDeviceWaitIdle>(gd(device, "vkDeviceWaitIdle"))(
        device);
    reinterpret_cast<PFN_vkDestroyDevice>(gd(device, "vkDestroyDevice"))(
        device, nullptr);
    device = {};
  }
  if (instance && get) {
    reinterpret_cast<PFN_vkDestroyInstance>(get(instance, "vkDestroyInstance"))(
        instance, nullptr);
    instance = {};
  }
  physical = {};
  queue = {};
  ahbProperties = nullptr;
  presentationDensity = subsampledPresentation = false;
  resetXrVulkanDispatch();
  if (library_) {
    dlclose(library_);
    library_ = nullptr;
  }
  get = nullptr;
}
} // namespace xrimmersive
