// Execute the shipped SPIR-V on a host Vulkan device (lavapipe is sufficient).
#include "../../app/src/main/cpp/xrimmersive/xr_lsfg_capture_spv.h"
#include "../../app/src/main/cpp/xrimmersive/xr_frame_interpolation.h"
#include <vulkan/vulkan.h>
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
using namespace xrimmersive::windowsvr;
#define CHECK(call) do { auto r = (call); if(r != VK_SUCCESS) { std::cerr << #call << ": " << r << '\n'; std::abort(); } } while(0)
int main() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ii.pApplicationInfo=&app;
    VkInstance instance; CHECK(vkCreateInstance(&ii,nullptr,&instance));
    uint32_t count=0; CHECK(vkEnumeratePhysicalDevices(instance,&count,nullptr)); assert(count);
    std::vector<VkPhysicalDevice> physical(count); CHECK(vkEnumeratePhysicalDevices(instance,&count,physical.data()));
    auto gpu=physical[0]; uint32_t n=0; vkGetPhysicalDeviceQueueFamilyProperties(gpu,&n,nullptr);
    std::vector<VkQueueFamilyProperties> families(n); vkGetPhysicalDeviceQueueFamilyProperties(gpu,&n,families.data());
    uint32_t family=0; while(family<n && !(families[family].queueFlags & VK_QUEUE_COMPUTE_BIT)) ++family; assert(family<n);
    float priority=1; VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi;
    VkDevice device; CHECK(vkCreateDevice(gpu,&di,nullptr,&device)); VkQueue queue; vkGetDeviceQueue(device,family,0,&queue);
    VkPhysicalDeviceMemoryProperties props; vkGetPhysicalDeviceMemoryProperties(gpu,&props);
    auto memoryType=[&](uint32_t bits,VkMemoryPropertyFlags flags) {
        for(uint32_t i=0;i<props.memoryTypeCount;++i) {
            if((bits&(1u<<i)) && (props.memoryTypes[i].propertyFlags&flags)==flags) return i;
        }
        std::abort();
    };
    constexpr int w=32,h=32,bytes=w*h*4;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size=bytes*3;
    bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer; CHECK(vkCreateBuffer(device,&bi,nullptr,&buffer));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(device,buffer,&req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; alloc.allocationSize=req.size;
    alloc.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory host; CHECK(vkAllocateMemory(device,&alloc,nullptr,&host)); CHECK(vkBindBufferMemory(device,buffer,host,0));
    void* mapped; CHECK(vkMapMemory(device,host,0,VK_WHOLE_SIZE,0,&mapped)); auto pixels=static_cast<unsigned char*>(mapped);
    for(int y=0;y<h;++y) for(int x=0;x<w;++x) { int i=(y*w+x)*4; pixels[i]=x*8; pixels[i+1]=y*8; pixels[i+2]=77; pixels[i+3]=255; }
    VkImage images[3]; VkImageView views[3]; VkDeviceMemory memory[3];
    for(int i=0;i<3;++i) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.imageType=VK_IMAGE_TYPE_2D;
        ci.format=VK_FORMAT_R8G8B8A8_UNORM; ci.extent=VkExtent3D{w,h,1}; ci.mipLevels=ci.arrayLayers=1; ci.samples=VK_SAMPLE_COUNT_1_BIT;
        ci.usage=i ? VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT : VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        CHECK(vkCreateImage(device,&ci,nullptr,&images[i])); vkGetImageMemoryRequirements(device,images[i],&req);
        alloc.allocationSize=req.size; alloc.memoryTypeIndex=memoryType(req.memoryTypeBits,0);
        CHECK(vkAllocateMemory(device,&alloc,nullptr,&memory[i])); CHECK(vkBindImageMemory(device,images[i],memory[i],0));
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=images[i]; vi.viewType=VK_IMAGE_VIEW_TYPE_2D;
        vi.format=ci.format; vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1}; CHECK(vkCreateImageView(device,&vi,nullptr,&views[i]));
    }
    VkSamplerCreateInfo sm{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO}; sm.minFilter=sm.magFilter=VK_FILTER_LINEAR;
    sm.addressModeU=sm.addressModeV=sm.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler; CHECK(vkCreateSampler(device,&sm,nullptr,&sampler));
    VkDescriptorSetLayoutBinding bindings[]={{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},{2,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}; si.bindingCount=3; si.pBindings=bindings;
    VkDescriptorSetLayout setLayout; CHECK(vkCreateDescriptorSetLayout(device,&si,nullptr,&setLayout));
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,128};
    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; li.setLayoutCount=1; li.pSetLayouts=&setLayout;
    li.pushConstantRangeCount=1; li.pPushConstantRanges=&range; VkPipelineLayout layout; CHECK(vkCreatePipelineLayout(device,&li,nullptr,&layout));
    VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO}; dpi.maxSets=1; dpi.poolSizeCount=2; dpi.pPoolSizes=sizes;
    VkDescriptorPool pool; CHECK(vkCreateDescriptorPool(device,&dpi,nullptr,&pool));
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO}; dai.descriptorPool=pool; dai.descriptorSetCount=1; dai.pSetLayouts=&setLayout;
    VkDescriptorSet set; CHECK(vkAllocateDescriptorSets(device,&dai,&set));
    VkDescriptorImageInfo infos[]={{sampler,views[0],VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,views[1],VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,views[2],VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[3]{};
    for(int i=0;i<3;++i) { writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[i].dstSet=set; writes[i].dstBinding=i;
        writes[i].descriptorCount=1; writes[i].descriptorType=bindings[i].descriptorType; writes[i].pImageInfo=&infos[i]; }
    vkUpdateDescriptorSets(device,3,writes,0,nullptr);
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; mi.codeSize=sizeof(kCaptureSpv); mi.pCode=kCaptureSpv;
    VkShaderModule module; CHECK(vkCreateShaderModule(device,&mi,nullptr,&module));
    VkComputePipelineCreateInfo pci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; pci.layout=layout;
    pci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};
    VkPipeline pipeline; CHECK(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pci,nullptr,&pipeline));
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; cpi.queueFamilyIndex=family; cpi.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool commands; CHECK(vkCreateCommandPool(device,&cpi,nullptr,&commands));
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool=commands; cai.commandBufferCount=1;
    VkCommandBuffer cmd; CHECK(vkAllocateCommandBuffers(device,&cai,&cmd));
    struct Params { float crop[4], rotation[3][4], sourceFov[4], targetFov[4]; int flags[4]; float packing[4]; } push{};
    static_assert(sizeof(push)==128);
    for(int variant=0;variant<8;++variant) {
        // Single-snapshot mode binds both output descriptors to the same image,
        // but the shader must only execute one imageStore.
        infos[2].imageView = variant >= 6 ? views[1] : views[2];
        vkUpdateDescriptorSets(device,3,writes,0,nullptr);
        CHECK(vkResetCommandPool(device,commands,0)); VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd,&begin));
        VkImageMemoryBarrier barriers[3]{};
        for(int i=0;i<3;++i) { auto& b=barriers[i]; b={VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
            b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED; b.image=images[i]; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            b.dstAccessMask=i?VK_ACCESS_SHADER_WRITE_BIT:VK_ACCESS_TRANSFER_WRITE_BIT; }
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,3,barriers);
        VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={w,h,1};
        vkCmdCopyBufferToImage(cmd,buffer,images[0],VK_IMAGE_LAYOUT_GENERAL,1,&copy);
        auto b=barriers[0]; b.oldLayout=VK_IMAGE_LAYOUT_GENERAL; b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
        float id[]={0,0,0,1}, yaw[]={0,std::sin(.05f),0,std::cos(.05f)};
        auto rot=targetToSourceRotation(id,variant>=2?yaw:id);
        for(int c=0;c<3;++c) for(int r=0;r<3;++r) push.rotation[c][r]=rot[c*3+r];
        float fov[]={-.6f,.9f,1.f,-.7f}; std::copy_n(fov,4,push.sourceFov); std::copy_n(fov,4,push.targetFov);
        push.crop[0]=push.crop[1]=0; push.crop[2]=push.crop[3]=1;
        if(variant) { push.crop[0]=.25f; push.crop[1]=.75f; push.crop[2]=.5f; push.crop[3]=-.5f; }
        push.flags[0]=variant<4; push.flags[1]=variant<6; push.flags[2]=variant==3 || variant==5; push.flags[3]=0;
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
        vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);
        vkCmdDispatch(cmd,4,4,1);
        for(int i=1;i<(variant>=6?2:3);++i) { barriers[i].oldLayout=VK_IMAGE_LAYOUT_GENERAL; barriers[i].srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; barriers[i].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT; }
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,variant>=6?1:2,barriers+1);
        for(int i=1;i<(variant>=6?2:3);++i) {
            copy.bufferOffset=bytes*i; copy.imageExtent=VkExtent3D{w,h,1};
            vkCmdCopyImageToBuffer(cmd,images[i],VK_IMAGE_LAYOUT_GENERAL,buffer,1,&copy);
        }
        VkMemoryBarrier hostRead{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; hostRead.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; hostRead.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&hostRead,0,nullptr,0,nullptr);
        CHECK(vkEndCommandBuffer(cmd)); VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&cmd;
        CHECK(vkQueueSubmit(queue,1,&submit,VK_NULL_HANDLE)); CHECK(vkQueueWaitIdle(queue));
        for(int eye=1;eye<(variant>=6?2:3);++eye) {
          const int side=w;
          for(int y=0;y<side;++y) for(int x=0;x<side;++x) {
            float u=(x+.5f)/w,v=(y+.5f)/h;
            if(eye==1 && push.flags[0]) {
                float ray[]={fov[0]+u*(fov[1]-fov[0]), fov[2]+v*(fov[3]-fov[2]),-1}, r[3]{};
                for(int a=0;a<3;++a) for(int c=0;c<3;++c) r[a]+=rot[c*3+a]*ray[c];
                u=(r[0]/-r[2]-fov[0])/(fov[1]-fov[0]); v=(r[1]/-r[2]-fov[2])/(fov[3]-fov[2]);
            }
            float lo=variant?.25f:0,hi=variant?.75f:1;
            u=std::clamp(push.crop[0]+u*push.crop[2],lo+.5f/w,hi-.5f/w);
            v=std::clamp(push.crop[1]+v*push.crop[3],lo+.5f/h,hi-.5f/h);
            int expected[]={int(std::lround(u*256-4)),int(std::lround(v*256-4)),77,255};
            if(variant==3 || variant==5) std::swap(expected[0],expected[2]);
            for(int c=0;c<4;++c) assert(std::abs(int(pixels[eye*bytes+(y*side+x)*4+c])-expected[c])<=2);
          }
        }
    }
    vkDestroyCommandPool(device,commands,nullptr); vkDestroyPipeline(device,pipeline,nullptr); vkDestroyShaderModule(device,module,nullptr);
    vkDestroyDescriptorPool(device,pool,nullptr); vkDestroyPipelineLayout(device,layout,nullptr); vkDestroyDescriptorSetLayout(device,setLayout,nullptr);
    vkDestroySampler(device,sampler,nullptr);
    for(int i=0;i<3;++i) { vkDestroyImageView(device,views[i],nullptr); vkDestroyImage(device,images[i],nullptr); vkFreeMemory(device,memory[i],nullptr); }
    vkUnmapMemory(device,host); vkDestroyBuffer(device,buffer,nullptr); vkFreeMemory(device,host,nullptr);
    vkDestroyDevice(device,nullptr); vkDestroyInstance(instance,nullptr);
    std::cout << "Vulkan capture SPIR-V: identity, asymmetric FOV, crop/flip, rotation, real history, swizzle, single immutable snapshot passed\n";
}
