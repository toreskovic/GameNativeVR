// Execute the production backend on Vulkan, including both optical-flow directions.
#include "../../app/src/main/cpp/xrimmersive/xr_optical_flow.h"
#include <vulkan/vulkan.h>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>
#include "presentation_test.h"
using namespace xrimmersive::windowsvr;
#define CHECK(x) do { auto r=(x); if(r!=VK_SUCCESS) { std::cerr<<#x<<": "<<r<<'\n';std::abort(); } } while(0)
// Mask queried capabilities to exercise every independent fallback without
// adding runtime switches to the production backend.
static PFN_vkGetPhysicalDeviceFormatProperties actualFormats;
static PFN_vkGetPhysicalDeviceProperties2 actualProperties;
static unsigned fallbackMask;
static VKAPI_ATTR void VKAPI_CALL testFormats(VkPhysicalDevice gpu,VkFormat f,VkFormatProperties* p) {
    actualFormats(gpu,f,p);
    if(((fallbackMask&1)&&f==VK_FORMAT_R8_UINT)||
       ((fallbackMask&2)&&f==VK_FORMAT_R16G16_SFLOAT)) p->optimalTilingFeatures=0;
}
static VKAPI_ATTR void VKAPI_CALL testProperties(VkPhysicalDevice gpu,VkPhysicalDeviceProperties2* p) {
    actualProperties(gpu,p);
    if(fallbackMask&4) {
        auto* next=static_cast<VkBaseOutStructure*>(p->pNext);
        for(;next;next=next->pNext) if(next->sType==VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES)
            reinterpret_cast<VkPhysicalDeviceSubgroupProperties*>(next)->supportedOperations=0;
    }
}
int main(int argc,char** argv) {
    const int presentMode=argc>1?std::atoi(argv[1]):0;
    assert(presentMode==0 || presentMode==1);
    std::unique_ptr<PresentationTest> presentation;
    if(presentMode) presentation=std::make_unique<PresentationTest>(presentMode);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};ii.pApplicationInfo=&app;
    VkInstance instance;CHECK(vkCreateInstance(&ii,nullptr,&instance));
    uint32_t count=0;CHECK(vkEnumeratePhysicalDevices(instance,&count,nullptr));assert(count);
    std::vector<VkPhysicalDevice> devices(count);CHECK(vkEnumeratePhysicalDevices(instance,&count,devices.data()));auto gpu=devices[0];
    uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(gpu,&n,nullptr);std::vector<VkQueueFamilyProperties> qs(n);vkGetPhysicalDeviceQueueFamilyProperties(gpu,&n,qs.data());
    uint32_t family=0;while(family<n && !(qs[family].queueFlags&VK_QUEUE_COMPUTE_BIT)) ++family;assert(family<n);
    float priority=1;VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};qi.queueFamilyIndex=family;qi.queueCount=1;qi.pQueuePriorities=&priority;
    VkPhysicalDeviceFeatures features{};features.shaderStorageImageExtendedFormats=VK_TRUE;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};di.queueCreateInfoCount=1;di.pQueueCreateInfos=&qi;di.pEnabledFeatures=&features;
    VkDevice device;CHECK(vkCreateDevice(gpu,&di,nullptr,&device));VkQueue queue;vkGetDeviceQueue(device,family,0,&queue);
    assert(vkd_load(instance,device,vkGetInstanceProcAddr));
    actualFormats=vkd.GetPhysicalDeviceFormatProperties;
    actualProperties=vkd.GetPhysicalDeviceProperties2;
    vkd.GetPhysicalDeviceFormatProperties=testFormats;
    vkd.GetPhysicalDeviceProperties2=testProperties;
    lsfg::Device alloc(device,gpu);
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.queueFamilyIndex=family;pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool;CHECK(vkCreateCommandPool(device,&pc,nullptr,&pool));
    VkCommandBufferAllocateInfo ac{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};ac.commandPool=pool;ac.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ac.commandBufferCount=2;
    VkCommandBuffer commands[2];CHECK(vkAllocateCommandBuffers(device,&ac,commands));
    VkCommandBuffer cmd = commands[0];
    for(fallbackMask=0;fallbackMask<8;++fallbackMask) {
    std::cout<<"Fallback mask="<<fallbackMask<<std::endl;
    for(auto extent: {VkExtent2D{128,128},VkExtent2D{129,97},VkExtent2D{384,320}}) {
      const int w=extent.width,h=extent.height,bytes=w*h*4;
      lsfg::LsfgImage color[2]={lsfg::LsfgImage(alloc,extent,VK_FORMAT_R8G8B8A8_UNORM),lsfg::LsfgImage(alloc,extent,VK_FORMAT_R8G8B8A8_UNORM)};
      lsfg::LsfgImage output(alloc,extent,VK_FORMAT_R8G8B8A8_UNORM);
      VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bc.size=bytes*3;bc.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
      VkBuffer staging;CHECK(vkCreateBuffer(device,&bc,nullptr,&staging));VkMemoryRequirements req;vkGetBufferMemoryRequirements(device,staging,&req);
      VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ma.allocationSize=req.size;ma.memoryTypeIndex=alloc.FindMemoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      VkDeviceMemory memory;CHECK(vkAllocateMemory(device,&ma,nullptr,&memory));CHECK(vkBindBufferMemory(device,staging,memory,0));void* mapped;CHECK(vkMapMemory(device,memory,0,VK_WHOLE_SIZE,0,&mapped));auto pixels=(unsigned char*)mapped;
      auto pattern=[](float x,float y,int c) -> unsigned char {
        // Smooth, nonperiodic, textured scene with a known rigid translation.
        float v=115+38*std::sin(x*.31f+y*.13f)+30*std::cos(y*.27f-x*.07f)+20*std::sin(x*.09f+y*.41f+c);
        return c==3?255:static_cast<unsigned char>(v);
      };
      for(float scale:{1.f,.5f,.1f}) {
        const int fw=std::max(32,int(w*scale)),fh=std::max(32,int(h*scale));
        lsfg::LsfgImage packed(alloc,{uint32_t((fw+7)/8),uint32_t((fh+7)/8)},VK_FORMAT_R16G16B16A16_SFLOAT);
        auto& destination=presentMode?packed:output;
        OpticalFlowBackend backend(alloc,extent,scale,presentMode!=0);assert(backend.valid());
        for(int variant=0;variant<(presentMode?8:11);++variant) {
          int current=(variant==2||variant==9)?0:1,old=1-current;
          std::array<ColorTransform,2> transforms{};
          const float identity[]={0,0,0,1},fov[]={-.8f,.75f,.7f,-.8f};
          float orientations[2][4]={{0,std::sin(.025f),0,std::cos(.025f)},
                                    {0,std::sin(-.02f),0,std::cos(-.02f)}};
          float sourceFov[2][4];
          for(auto& f:sourceFov) std::copy_n(fov,4,f);
          if(variant==10) { sourceFov[current][0]-=.025f; sourceFov[current][2]+=.03f; }
          if(variant>=8) {
            transforms[0]=colorTransform(orientations[old],sourceFov[old],identity,fov);
            transforms[1]=colorTransform(orientations[current],sourceFov[current],identity,fov);
          }
          for(int slot=0;slot<2;++slot) for(int y=0;y<h;++y) for(int x=0;x<w;++x) for(int c=0;c<4;++c) {
            int shift=variant==0?0:(variant==2?8:0)+(slot==current?8:0);
            if(variant==4) shift=slot==current?-8:0;
            if(variant==5) shift=0;
            if(variant>=6) shift=slot==current?(variant==6?3:13):0;
            int shiftY=variant==5 && slot==current?8:0;
            if(variant>=8) {
                const auto inverse=colorTransform(identity,fov,orientations[slot],sourceFov[slot]);
                const auto& m=inverse.columns;
                const float u=(x+.5f)/w,v=(y+.5f)/h;
                const float z=m[2]*u+m[6]*v+m[10];
                pixels[slot*bytes+(y*w+x)*4+c]=pattern((m[0]*u+m[4]*v+m[8])/z*w-.5f,
                    (m[1]*u+m[5]*v+m[9])/z*h-.5f,c);
                continue;
            }
            pixels[slot*bytes+(y*w+x)*4+c]=variant==3?(c==3?255:(c==(slot==current?1:0)?180:0)):pattern(x-shift,y-shiftY,c);
          }
          CHECK(vkResetCommandPool(device,pool,0));cmd=commands[0];VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};CHECK(vkBeginCommandBuffer(cmd,&begin));
          VkImageMemoryBarrier barriers[3]{};
          for(int i=0;i<3;++i) { auto& b=barriers[i];b={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=i<2?color[i].Handle():destination.Handle();b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};b.dstAccessMask=i<2?VK_ACCESS_TRANSFER_WRITE_BIT:VK_ACCESS_SHADER_WRITE_BIT; }
          vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,3,barriers);
          VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={extent.width,extent.height,1};
          for(int i=0;i<2;++i) {copy.bufferOffset=i*bytes;vkCmdCopyBufferToImage(cmd,staging,color[i].Handle(),VK_IMAGE_LAYOUT_GENERAL,1,&copy);}
          // Producer and flow are submitted as adjacent batches, with no host
          // wait between them. The following GPU barrier makes writes visible.
          CHECK(vkEndCommandBuffer(cmd));cmd=commands[1];
          CHECK(vkBeginCommandBuffer(cmd,&begin));
          VkMemoryBarrier ready{VK_STRUCTURE_TYPE_MEMORY_BARRIER};ready.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
          vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&ready,0,nullptr,0,nullptr);
          backend.record(cmd,color[old].View(),color[current].View(),destination.View(),current,variant==2||variant==9,transforms);
          ready.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
          vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,1,&ready,0,nullptr,0,nullptr);
          copy.bufferOffset=bytes*2;copy.imageExtent={destination.Extent().width,destination.Extent().height,1};vkCmdCopyImageToBuffer(cmd,destination.Handle(),VK_IMAGE_LAYOUT_GENERAL,staging,1,&copy);
          ready.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;ready.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
          vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&ready,0,nullptr,0,nullptr);
          CHECK(vkEndCommandBuffer(cmd));VkSubmitInfo submits[2]={{VK_STRUCTURE_TYPE_SUBMIT_INFO},{VK_STRUCTURE_TYPE_SUBMIT_INFO}};for(int i=0;i<2;++i){submits[i].commandBufferCount=1;submits[i].pCommandBuffers=&commands[i];}CHECK(vkQueueSubmit(queue,2,submits,VK_NULL_HANDLE));CHECK(vkQueueWaitIdle(queue));
          std::vector<unsigned char> synthesized;
          if(presentMode) synthesized=presentation->render(w,h,fw,fh,pixels+old*bytes,pixels+current*bytes,pixels+bytes*2);
          const auto* result=presentMode?synthesized.data():pixels+bytes*2;
          double midpointError=0,newError=0;int samples=0,maxError=0;
          int margin=(variant==0||variant==3)?0:24;
          for(int y=margin;y<h-margin;++y) for(int x=margin;x<w-margin;++x) for(int c=0;c<3;++c) {
            int actual=result[(y*w+x)*4+c];
            int expected=variant==3?(c==1?180:0):pattern(x-(variant==0||variant==5?0:variant==2?12:variant==4?-4:variant==6?1.5f:variant==7?6.5f:4),y-(variant==5?4:0),c);
            if(variant>=8) expected=pattern(x,y,c);
            maxError=std::max(maxError,std::abs(actual-expected));midpointError+=std::abs(actual-expected);
            newError+=std::abs(int(pixels[current*bytes+(y*w+x)*4+c])-expected);++samples;
          }
          std::cout<<w<<'x'<<h<<" scale="<<scale<<" case="<<variant<<" midpoint MAE="<<midpointError/samples<<" real MAE="<<newError/samples<<" max="<<maxError<<'\n';
          if(variant==0||variant==3) assert(maxError<=2);
          if(variant>=8) assert(midpointError/samples<2.5);
          if((variant==1||variant==2||(variant>=4&&variant<8))&&(scale==1.f||(scale==.5f&&variant<6))) assert(midpointError<newError*.65);
        }
      }
      vkUnmapMemory(device,memory);vkDestroyBuffer(device,staging,nullptr);vkFreeMemory(device,memory,nullptr);
    }
    }
    vkDestroyCommandPool(device,pool,nullptr);vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);
    std::cout<<"Optical flow Vulkan tests passed\n";
}
