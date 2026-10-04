#include "../../app/src/main/cpp/xrimmersive/xr_present_foveation.h"
#include <cassert>
using namespace xrimmersive;
int main() {
 assert(presentationDensity({0,1},{16,16},1).empty());
 assert(presentationDensity({1,1},{0,16},1).empty());
 assert(presentationDensity({1,1},{16,16},0).empty());
 for(auto size:{VkExtent2D{1,1},{65,49},{1964,2160}}) for(float scale:{.7f,1.f}) {
  const VkExtent2D cell{16,16};auto map=presentationDensity(size,cell,scale);
  const unsigned w=(size.width+15)/16,h=(size.height+15)/16;
  assert(map.size()==w*h*2);
  unsigned full=0,half=0,quarter=0;
  for(unsigned i=0;i<map.size();i+=2) {
   assert(map[i]==map[i+1]);
   assert(map[i]==255 || map[i]==127 || map[i]==63);
   full+=map[i]==255;half+=map[i]==127;quarter+=map[i]==63;
  }
  if(size.width>1000) {
   assert(full && half && quarter);
   auto at=[&](float x,float y){return map[(unsigned(y*size.height)/16*w+unsigned(x*size.width)/16)*2];};
   assert(at(.5f,.5f)==255);
   assert(at(.5f+.3f*scale,.5f)==127);
   assert(at(.5f+.45f*scale,.5f)==63);
   assert(at(.5f,.5f+.45f*scale)==127); // Game's wider vertical ellipse.
   assert(at(.01f,.01f)==63);
   if(scale==1) { // Approximate geometric areas, allowing conservative boundary cells.
    assert(float(full)/(w*h)>.18f && float(full)/(w*h)<.21f);
   }
  }
 }
}
