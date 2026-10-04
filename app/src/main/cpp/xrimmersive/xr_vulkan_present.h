#pragma once
#include "xr_visibility_mask.h"
#include "xr_vulkan_context.h"
#include "xr_windows_transport.h"
#include <array>
#include <memory>
#include <vector>
namespace xrimmersive {
class XrVulkanPresenter {
public:
  struct Source {
    VkImage image{};
    VkImageView view{};
    windowsvr::EyeFrame frame{};
    bool external = false;
    uint32_t owner = VK_QUEUE_FAMILY_EXTERNAL;
  };
  void setVisibilityMasks(const std::array<VisibilityMesh,2>&, const std::array<XrFovf,2>&, uint64_t revision);
  XrVulkanPresenter();
  ~XrVulkanPresenter();
  bool initialize(XrVulkanContext *, XrSwapchain, VkFormat, uint32_t width,
                  uint32_t height, uint32_t layers, int upscaler,
                  float sharpness, float fov, int border, bool fxaa = false, int ffrDebug = 0,
                  bool subsampled = false);
  // Acquires/waits the OpenXR image, submits asynchronously, and releases it.
  // Returned fence covers only this submission and is owned by the caller.
  bool render(std::array<Source, 2> sources, int &releaseFence);
  bool renderGuest(const std::array<windowsvr::EyeFrame, 2> &frames,
                   int &releaseFence);
  bool renderQuad(AHardwareBuffer *game, const std::vector<uint8_t> &rgba,
                  int width, int height, uint64_t version, float scaleX,
                  float scaleY);
  void shutdown();

private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};
} // namespace xrimmersive
