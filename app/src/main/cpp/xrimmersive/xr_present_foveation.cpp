#include "xr_present_foveation.h"
#include <cstring>
#include "xr_vulkan_dispatch.h"
namespace xrimmersive {
void PresentationDensityMap::reset() {
    if(view_) vkd.DestroyImageView(device_,view_,nullptr);
    if(image_) vkd.DestroyImage(device_,image_,nullptr);
    if(memory_) vkd.FreeMemory(device_,memory_,nullptr);
    view_={}; image_={}; memory_={}; device_={};
}
bool PresentationDensityMap::initialize(const lsfg::Device& device, VkQueue queue,
    uint32_t family, VkExtent2D extent, VkExtent2D texel, float fovScale) {
    reset(); device_=device.Handle();
    auto data=presentationDensity(extent,texel,fovScale);
    if(data.empty()) return false;
    VkBuffer buffer{}; VkDeviceMemory staging{}; VkCommandPool pool{}; VkFence fence{};
    auto cleanup=[&](bool success) {
        if(fence) xrVk.DestroyFence(device_,fence,nullptr);
        if(pool) xrVk.DestroyCommandPool(device_,pool,nullptr);
        if(buffer) vkd.DestroyBuffer(device_,buffer,nullptr);
        if(staging) vkd.FreeMemory(device_,staging,nullptr);
        if(!success) reset();
        return success;
    };
    auto allocate=[&](VkMemoryRequirements req,VkMemoryPropertyFlags flags,VkDeviceMemory& mem) {
        VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        a.allocationSize=req.size; a.memoryTypeIndex=device.FindMemoryType(req.memoryTypeBits,flags);
        return a.memoryTypeIndex!=UINT32_MAX && vkd.AllocateMemory(device_,&a,nullptr,&mem)==VK_SUCCESS;
    };
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType=VK_IMAGE_TYPE_2D;ci.format=VK_FORMAT_R8G8_UNORM;
    ci.extent={(extent.width+texel.width-1)/texel.width,(extent.height+texel.height-1)/texel.height,1};
    ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;
    ci.usage=VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if(vkd.CreateImage(device_,&ci,nullptr,&image_)!=VK_SUCCESS) return cleanup(false);
    VkMemoryRequirements req{};vkd.GetImageMemoryRequirements(device_,image_,&req);
    if(!allocate(req,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,memory_) ||
       vkd.BindImageMemory(device_,image_,memory_,0)!=VK_SUCCESS) return cleanup(false);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image=image_;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=ci.format;
    vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    if(vkd.CreateImageView(device_,&vi,nullptr,&view_)!=VK_SUCCESS) return cleanup(false);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};bi.size=data.size();bi.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if(vkd.CreateBuffer(device_,&bi,nullptr,&buffer)!=VK_SUCCESS) return cleanup(false);
    vkd.GetBufferMemoryRequirements(device_,buffer,&req);
    if(!allocate(req,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,staging) ||
       vkd.BindBufferMemory(device_,buffer,staging,0)!=VK_SUCCESS) return cleanup(false);
    void* mapped{};
    if(vkd.MapMemory(device_,staging,0,data.size(),0,&mapped)!=VK_SUCCESS) return cleanup(false);
    std::memcpy(mapped,data.data(),data.size());vkd.UnmapMemory(device_,staging);
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};pc.queueFamilyIndex=family;
    if(xrVk.CreateCommandPool(device_,&pc,nullptr,&pool)!=VK_SUCCESS) return cleanup(false);
    VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool=pool;ca.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ca.commandBufferCount=1;
    VkCommandBuffer cmd{};if(xrVk.AllocateCommandBuffers(device_,&ca,&cmd)!=VK_SUCCESS) return cleanup(false);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if(xrVk.BeginCommandBuffer(cmd,&begin)!=VK_SUCCESS) return cleanup(false);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=image_;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange=vi.subresourceRange;
    b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
    vkd.CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent=ci.extent;
    vkd.CmdCopyBufferToImage(cmd,buffer,image_,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    b.oldLayout=b.newLayout;b.newLayout=VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
    b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT;
    vkd.CmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT,0,0,nullptr,0,nullptr,1,&b);
    if(xrVk.EndCommandBuffer(cmd)!=VK_SUCCESS) return cleanup(false);
    VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if(xrVk.CreateFence(device_,&fc,nullptr,&fence)!=VK_SUCCESS) return cleanup(false);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&cmd;
    if(vkd.QueueSubmit(queue,1,&si,fence)!=VK_SUCCESS) return cleanup(false);
    // UINT64_MAX avoids freeing upload resources while a timeout leaves work live.
    const auto result=xrVk.WaitForFences(device_,1,&fence,VK_TRUE,UINT64_MAX);
    if(result!=VK_SUCCESS) vkd.DeviceWaitIdle(device_);
    return cleanup(result==VK_SUCCESS);
}
}
