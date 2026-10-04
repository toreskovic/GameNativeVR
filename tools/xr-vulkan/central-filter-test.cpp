#include "../../app/src/main/cpp/xrimmersive/xr_filter_sampling.h"
#include <cassert>
using xrimmersive::centralFilterEligible;
int main() {
 assert(centralFilterEligible(1768,1944,0,0,1768,1944));
 assert(centralFilterEligible(513,517,0,0,513,517));
 assert(centralFilterEligible(512,512,128,128,256,256));
 assert(!centralFilterEligible(512,512,0,0,256,256));
 assert(!centralFilterEligible(128,128,0,0,128,128));
 assert(!centralFilterEligible(512,512,511,0,2,512));
 assert(!centralFilterEligible(512,512,-1,0,512,512));
 assert(!centralFilterEligible(0,512,0,0,512,512));
 // Tiny centered crops must still leave the filter footprint inside the eye.
 assert(!centralFilterEligible(512,512,255,255,2,2));
}
