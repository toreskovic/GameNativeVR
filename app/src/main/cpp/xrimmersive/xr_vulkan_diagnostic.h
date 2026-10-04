#pragma once
#include <vulkan/vulkan.h>
#include <algorithm>

namespace xrimmersive {
#ifdef GN_XR_SWAPCHAIN_PATTERN
inline constexpr bool kXrSwapchainPattern = true;
#else
inline constexpr bool kXrSwapchainPattern = false;
#endif

// Direct attachment clears: no shaders, descriptors, imported images or copies.
// Caller begins the same render pass and per-layer framebuffer as presentation.
inline void recordXrSwapchainPattern(PFN_vkCmdClearAttachments clear,
                                     VkCommandBuffer cmd, uint32_t w,
                                     uint32_t h, uint32_t markerBars) {
  auto rect = [&](uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1,
                  float r, float g, float b) {
    VkClearAttachment attachment{};
    attachment.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    attachment.colorAttachment = 0;
    attachment.clearValue.color = {{r, g, b, 1}};
    VkClearRect area{{{int32_t(x0), int32_t(y0)}, {x1 - x0, y1 - y0}}, 0, 1};
    clear(cmd, 1, &attachment, 1, &area);
  };
  rect(0, 0, w / 2, h / 2, 1, 0, 0);
  rect(w / 2, 0, w, h / 2, 0, 1, 0);
  rect(0, h / 2, w / 2, h, 0, 0, 1);
  rect(w / 2, h / 2, w, h, 1, 1, 0);
  // White perimeter exposes cropping and repeated/incorrectly pitched tiles.
  const uint32_t bw = std::max(1u, w / 50), bh = std::max(1u, h / 50);
  rect(0, 0, w, bh, 1, 1, 1);
  rect(0, h - bh, w, h, 1, 1, 1);
  rect(0, 0, bw, h, 1, 1, 1);
  rect(w - bw, 0, w, h, 1, 1, 1);
  // One white bar = left eye, two = right eye, three = launcher quad.
  rect(w * 3 / 8, h * 3 / 8, w * 5 / 8, h * 5 / 8, 0, 0, 0);
  for (uint32_t i = 0; i < markerBars; ++i) {
    uint32_t x = w * (13 + i * 2) / 32;
    rect(x, h * 13 / 32, x + w / 32, h * 19 / 32, 1, 1, 1);
  }
}
} // namespace xrimmersive
