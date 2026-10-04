#pragma once
#include "../lsfg/vk_dispatch.h"
#include "xr_windows_transport.h"
#include <memory>

namespace xrimmersive::windowsvr {
// Serialized by the generator mutex, with at most one job outstanding. Imported source
// images remain alive until the job completes; descriptors are not edited in flight.
class VulkanEyeCapture {
public:
    VulkanEyeCapture(VkDevice device, PFN_vkGetAndroidHardwareBufferPropertiesANDROID properties, bool capturePipeline = true);
    ~VulkanEyeCapture();
    bool valid() const;
    static bool probe(VkDevice device, PFN_vkGetAndroidHardwareBufferPropertiesANDROID properties, const EyeFrame &frame);
    bool import(const std::array<EyeFrame, 2>& frames);
    VkImage sourceImage(int eye) const;
    VkImageView sourceView(int eye) const;
    void record(VkCommandBuffer cmd, unsigned slot, VkImageView source, VkImageView aligned,
                VkImageView real, const EyeFrame& frame, const EyeFrame& target,
                int width, int height, bool captureReal, bool align = true);
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
}
