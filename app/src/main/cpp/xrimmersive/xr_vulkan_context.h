#pragma once
// Graphics and platform types must precede OpenXR's platform declarations.
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include <jni.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <mutex>
namespace xrimmersive {
// One owner per XR session. Renderer and generator retain only borrowed
// handles.
class XrVulkanContext {
public:
  ~XrVulkanContext();
  bool initialize(XrInstance xr, XrSystemId system, bool subsampledSwapchains = false);
  bool supportsSubsampledSwapchain(VkFormat, VkExtent2D, uint32_t layers) const;
  const char *driverLabel() const { return "system Vulkan"; }
  void shutdown();
  VkResult submit(uint32_t count, const VkSubmitInfo *submits, VkFence fence);
  VkResult idle();
  VkInstance instance{};
  VkPhysicalDevice physical{};
  VkDevice device{};
  VkQueue queue{};
  uint32_t family = 0;
  VkPhysicalDeviceProperties properties{};
  bool presentationDensity = false;
  bool subsampledPresentation = false;
  VkExtent2D densityTexel{};
  PFN_vkGetInstanceProcAddr get{};
  PFN_vkGetAndroidHardwareBufferPropertiesANDROID ahbProperties{};
  std::mutex queueMutex;

private:
  void *library_{};
};
} // namespace xrimmersive
