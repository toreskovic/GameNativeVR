#pragma once
#include <cstdint>
namespace xrimmersive {
// Must cover the 0.47 filter radius plus SGSR gathers and bilinear AA support.
// Integer packing cuts match gamenative_foveated_packing.h, including odd sizes.
inline bool centralFilterEligible(int width, int height, int x, int y,
                                  int eyeWidth, int eyeHeight) {
    if(width<=0 || height<=0 || x<0 || y<0 || eyeWidth<=0 || eyeHeight<=0 ||
       int64_t(x)+eyeWidth>width || int64_t(y)+eyeHeight>height) return false;
    auto fits=[](int extent,int start,int size) {
        const double cut=(extent/8)*2;
        const double center=start+size*.5;
        const double reach=size*.235+3.;
        // Also stay inside the submitted eye, not just the packing rectangle.
        return center-reach>=cut && center+reach<=extent-cut &&
               reach<=size*.5;
    };
    return fits(width,x,eyeWidth) && fits(height,y,eyeHeight);
}
}
