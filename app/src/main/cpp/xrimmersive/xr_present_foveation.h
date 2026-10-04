#pragma once
#include "../lsfg/lsfg_common.hpp"
#include "../vrffr/selection.h"
#include <algorithm>
#include <cmath>
#include <vector>
namespace xrimmersive {
// Use the game's density profile and the same corners/midpoints policy.
// FOV scaling maps the output back to the visible game's normalized eye space.
inline std::vector<uint8_t> presentationDensity(VkExtent2D extent, VkExtent2D texel,
                                               float fovScale) {
    if(!extent.width || !extent.height || !texel.width || !texel.height ||
       !std::isfinite(fovScale) || fovScale<=0) return {};
    const unsigned w=(extent.width+texel.width-1)/texel.width;
    const unsigned h=(extent.height+texel.height-1)/texel.height;
    const ffr::Rect eye{0,0,extent.width,extent.height};
    std::vector<uint8_t> data(w*h*2);
    for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
        uint8_t value=0;
        for(float dy : {0.f,.5f,1.f}) for(float dx : {0.f,.5f,1.f}) {
            float px=std::min(float(extent.width)-.5f,(x+dx)*texel.width);
            float py=std::min(float(extent.height)-.5f,(y+dy)*texel.height);
            px=(px-extent.width*.5f)/fovScale+extent.width*.5f;
            py=(py-extent.height*.5f)/fovScale+extent.height*.5f;
            value=std::max(value,ffr::density(px,py,&eye,1));
        }
        data[(y*w+x)*2]=data[(y*w+x)*2+1]=value;
    }
    return data;
}
class PresentationDensityMap {
public:
    ~PresentationDensityMap() { reset(); }
    // Caller serializes queue access. Upload happens once at initialization,
    // never in the frame loop; both eyes and every swapchain slot share it.
    bool initialize(const lsfg::Device&, VkQueue, uint32_t family,
                    VkExtent2D extent, VkExtent2D texel, float fovScale);
    void reset(); // Caller must retire GPU users first.
    VkImageView view() const { return view_; }
private:
    VkDevice device_{};
    VkImage image_{};
    VkImageView view_{};
    VkDeviceMemory memory_{};
};
}
