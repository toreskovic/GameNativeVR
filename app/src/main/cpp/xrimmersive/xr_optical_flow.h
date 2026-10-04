#pragma once
#include "../lsfg/lsfg_common.hpp"
#include <memory>
#include "xr_frame_interpolation.h"
namespace xrimmersive::windowsvr {
// One eye, one job in flight. Caller owns input/output and external synchronization.
class OpticalFlowBackend {
public:
    OpticalFlowBackend(const lsfg::Device& device, VkExtent2D extent, float flowScale, bool presentationSynthesis = false);
    ~OpticalFlowBackend();
    bool valid() const;
    void record(VkCommandBuffer cmd, VkImageView oldColor, VkImageView newColor,
                VkImageView output, int currentSlot, bool reuseHistory,
                const std::array<ColorTransform, 2>& transforms = {});
private:
    struct Impl;
    std::unique_ptr<Impl> p_;
};
}
