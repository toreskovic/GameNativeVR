#include "xr_vulkan_dispatch.h"
namespace xrimmersive {
XrVulkanDispatch xrVk;
void resetXrVulkanDispatch() { xrVk = XrVulkanDispatch{}; }
void loadXrVulkanDispatch(VkInstance i, VkDevice d,
                          PFN_vkGetInstanceProcAddr get) {
  auto gd =
      reinterpret_cast<PFN_vkGetDeviceProcAddr>(get(i, "vkGetDeviceProcAddr"));
  xrVk.AllocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(
      gd(d, "vkAllocateCommandBuffers"));
  xrVk.AllocateDescriptorSets = reinterpret_cast<PFN_vkAllocateDescriptorSets>(
      gd(d, "vkAllocateDescriptorSets"));
  xrVk.AllocateMemory =
      reinterpret_cast<PFN_vkAllocateMemory>(gd(d, "vkAllocateMemory"));
  xrVk.BeginCommandBuffer =
      reinterpret_cast<PFN_vkBeginCommandBuffer>(gd(d, "vkBeginCommandBuffer"));
  xrVk.BindBufferMemory =
      reinterpret_cast<PFN_vkBindBufferMemory>(gd(d, "vkBindBufferMemory"));
  xrVk.BindImageMemory =
      reinterpret_cast<PFN_vkBindImageMemory>(gd(d, "vkBindImageMemory"));
  xrVk.CmdBeginRenderPass =
      reinterpret_cast<PFN_vkCmdBeginRenderPass>(gd(d, "vkCmdBeginRenderPass"));
  xrVk.CmdBindDescriptorSets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(
      gd(d, "vkCmdBindDescriptorSets"));
  xrVk.CmdBindPipeline =
      reinterpret_cast<PFN_vkCmdBindPipeline>(gd(d, "vkCmdBindPipeline"));
  xrVk.CmdCopyBufferToImage = reinterpret_cast<PFN_vkCmdCopyBufferToImage>(
      gd(d, "vkCmdCopyBufferToImage"));
  xrVk.CmdDispatch =
      reinterpret_cast<PFN_vkCmdDispatch>(gd(d, "vkCmdDispatch"));
  xrVk.CmdDraw = reinterpret_cast<PFN_vkCmdDraw>(gd(d, "vkCmdDraw"));
  xrVk.CmdEndRenderPass =
      reinterpret_cast<PFN_vkCmdEndRenderPass>(gd(d, "vkCmdEndRenderPass"));
  xrVk.CmdPipelineBarrier =
      reinterpret_cast<PFN_vkCmdPipelineBarrier>(gd(d, "vkCmdPipelineBarrier"));
  xrVk.CmdPushConstants =
      reinterpret_cast<PFN_vkCmdPushConstants>(gd(d, "vkCmdPushConstants"));
  xrVk.CmdResetQueryPool =
      reinterpret_cast<PFN_vkCmdResetQueryPool>(gd(d, "vkCmdResetQueryPool"));
  xrVk.CmdSetScissor =
      reinterpret_cast<PFN_vkCmdSetScissor>(gd(d, "vkCmdSetScissor"));
  xrVk.CmdSetViewport =
      reinterpret_cast<PFN_vkCmdSetViewport>(gd(d, "vkCmdSetViewport"));
  xrVk.CmdWriteTimestamp =
      reinterpret_cast<PFN_vkCmdWriteTimestamp>(gd(d, "vkCmdWriteTimestamp"));
  xrVk.CreateBuffer =
      reinterpret_cast<PFN_vkCreateBuffer>(gd(d, "vkCreateBuffer"));
  xrVk.CreateCommandPool =
      reinterpret_cast<PFN_vkCreateCommandPool>(gd(d, "vkCreateCommandPool"));
  xrVk.CreateComputePipelines = reinterpret_cast<PFN_vkCreateComputePipelines>(
      gd(d, "vkCreateComputePipelines"));
  xrVk.CreateDescriptorPool = reinterpret_cast<PFN_vkCreateDescriptorPool>(
      gd(d, "vkCreateDescriptorPool"));
  xrVk.CreateDescriptorSetLayout =
      reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(
          gd(d, "vkCreateDescriptorSetLayout"));
  xrVk.CreateDevice =
      reinterpret_cast<PFN_vkCreateDevice>(get(i, "vkCreateDevice"));
  xrVk.CreateFence =
      reinterpret_cast<PFN_vkCreateFence>(gd(d, "vkCreateFence"));
  xrVk.CreateFramebuffer =
      reinterpret_cast<PFN_vkCreateFramebuffer>(gd(d, "vkCreateFramebuffer"));
  xrVk.CreateGraphicsPipelines =
      reinterpret_cast<PFN_vkCreateGraphicsPipelines>(
          gd(d, "vkCreateGraphicsPipelines"));
  xrVk.CreateImage =
      reinterpret_cast<PFN_vkCreateImage>(gd(d, "vkCreateImage"));
  xrVk.CreateImageView =
      reinterpret_cast<PFN_vkCreateImageView>(gd(d, "vkCreateImageView"));
  xrVk.CreateInstance =
      reinterpret_cast<PFN_vkCreateInstance>(get(i, "vkCreateInstance"));
  xrVk.CreatePipelineLayout = reinterpret_cast<PFN_vkCreatePipelineLayout>(
      gd(d, "vkCreatePipelineLayout"));
  xrVk.CreateQueryPool =
      reinterpret_cast<PFN_vkCreateQueryPool>(gd(d, "vkCreateQueryPool"));
  xrVk.CreateRenderPass =
      reinterpret_cast<PFN_vkCreateRenderPass>(gd(d, "vkCreateRenderPass"));
  xrVk.CreateSampler =
      reinterpret_cast<PFN_vkCreateSampler>(gd(d, "vkCreateSampler"));
  xrVk.CreateSemaphore =
      reinterpret_cast<PFN_vkCreateSemaphore>(gd(d, "vkCreateSemaphore"));
  xrVk.DestroyBuffer =
      reinterpret_cast<PFN_vkDestroyBuffer>(gd(d, "vkDestroyBuffer"));
  xrVk.DestroyCommandPool =
      reinterpret_cast<PFN_vkDestroyCommandPool>(gd(d, "vkDestroyCommandPool"));
  xrVk.DestroyDescriptorPool = reinterpret_cast<PFN_vkDestroyDescriptorPool>(
      gd(d, "vkDestroyDescriptorPool"));
  xrVk.DestroyDescriptorSetLayout =
      reinterpret_cast<PFN_vkDestroyDescriptorSetLayout>(
          gd(d, "vkDestroyDescriptorSetLayout"));
  xrVk.DestroyDevice =
      reinterpret_cast<PFN_vkDestroyDevice>(gd(d, "vkDestroyDevice"));
  xrVk.DestroyFence =
      reinterpret_cast<PFN_vkDestroyFence>(gd(d, "vkDestroyFence"));
  xrVk.DestroyFramebuffer =
      reinterpret_cast<PFN_vkDestroyFramebuffer>(gd(d, "vkDestroyFramebuffer"));
  xrVk.DestroyImage =
      reinterpret_cast<PFN_vkDestroyImage>(gd(d, "vkDestroyImage"));
  xrVk.DestroyImageView =
      reinterpret_cast<PFN_vkDestroyImageView>(gd(d, "vkDestroyImageView"));
  xrVk.DestroyInstance =
      reinterpret_cast<PFN_vkDestroyInstance>(get(i, "vkDestroyInstance"));
  xrVk.DestroyPipeline =
      reinterpret_cast<PFN_vkDestroyPipeline>(gd(d, "vkDestroyPipeline"));
  xrVk.DestroyPipelineLayout = reinterpret_cast<PFN_vkDestroyPipelineLayout>(
      gd(d, "vkDestroyPipelineLayout"));
  xrVk.DestroyQueryPool =
      reinterpret_cast<PFN_vkDestroyQueryPool>(gd(d, "vkDestroyQueryPool"));
  xrVk.DestroyRenderPass =
      reinterpret_cast<PFN_vkDestroyRenderPass>(gd(d, "vkDestroyRenderPass"));
  xrVk.DestroySampler =
      reinterpret_cast<PFN_vkDestroySampler>(gd(d, "vkDestroySampler"));
  xrVk.DestroySemaphore =
      reinterpret_cast<PFN_vkDestroySemaphore>(gd(d, "vkDestroySemaphore"));
  xrVk.DeviceWaitIdle =
      reinterpret_cast<PFN_vkDeviceWaitIdle>(gd(d, "vkDeviceWaitIdle"));
  xrVk.EndCommandBuffer =
      reinterpret_cast<PFN_vkEndCommandBuffer>(gd(d, "vkEndCommandBuffer"));
  xrVk.EnumerateDeviceExtensionProperties =
      reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
          get(i, "vkEnumerateDeviceExtensionProperties"));
  xrVk.EnumeratePhysicalDevices =
      reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
          get(i, "vkEnumeratePhysicalDevices"));
  xrVk.FreeMemory = reinterpret_cast<PFN_vkFreeMemory>(gd(d, "vkFreeMemory"));
  xrVk.GetBufferMemoryRequirements =
      reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(
          gd(d, "vkGetBufferMemoryRequirements"));
  xrVk.GetDeviceQueue =
      reinterpret_cast<PFN_vkGetDeviceQueue>(gd(d, "vkGetDeviceQueue"));
  xrVk.GetFenceStatus =
      reinterpret_cast<PFN_vkGetFenceStatus>(gd(d, "vkGetFenceStatus"));
  xrVk.GetPhysicalDeviceFeatures2 =
      reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
          get(i, "vkGetPhysicalDeviceFeatures2"));
  xrVk.GetPhysicalDeviceProperties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
          get(i, "vkGetPhysicalDeviceProperties"));
  xrVk.GetPhysicalDeviceQueueFamilyProperties =
      reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
          get(i, "vkGetPhysicalDeviceQueueFamilyProperties"));
  xrVk.GetQueryPoolResults = reinterpret_cast<PFN_vkGetQueryPoolResults>(
      gd(d, "vkGetQueryPoolResults"));
  xrVk.MapMemory = reinterpret_cast<PFN_vkMapMemory>(gd(d, "vkMapMemory"));
  xrVk.QueueSubmit =
      reinterpret_cast<PFN_vkQueueSubmit>(gd(d, "vkQueueSubmit"));
  xrVk.QueueWaitIdle =
      reinterpret_cast<PFN_vkQueueWaitIdle>(gd(d, "vkQueueWaitIdle"));
  xrVk.ResetCommandBuffer =
      reinterpret_cast<PFN_vkResetCommandBuffer>(gd(d, "vkResetCommandBuffer"));
  xrVk.ResetFences =
      reinterpret_cast<PFN_vkResetFences>(gd(d, "vkResetFences"));
  xrVk.UnmapMemory =
      reinterpret_cast<PFN_vkUnmapMemory>(gd(d, "vkUnmapMemory"));
  xrVk.UpdateDescriptorSets = reinterpret_cast<PFN_vkUpdateDescriptorSets>(
      gd(d, "vkUpdateDescriptorSets"));
  xrVk.WaitForFences =
      reinterpret_cast<PFN_vkWaitForFences>(gd(d, "vkWaitForFences"));
}
} // namespace xrimmersive
