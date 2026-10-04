#pragma once
#include "../lsfg/vk_dispatch.h"
namespace xrimmersive {
struct XrVulkanDispatch {
  PFN_vkAllocateCommandBuffers AllocateCommandBuffers =
      ::vkAllocateCommandBuffers;
  PFN_vkAllocateDescriptorSets AllocateDescriptorSets =
      ::vkAllocateDescriptorSets;
  PFN_vkAllocateMemory AllocateMemory = ::vkAllocateMemory;
  PFN_vkBeginCommandBuffer BeginCommandBuffer = ::vkBeginCommandBuffer;
  PFN_vkBindBufferMemory BindBufferMemory = ::vkBindBufferMemory;
  PFN_vkBindImageMemory BindImageMemory = ::vkBindImageMemory;
  PFN_vkCmdBeginRenderPass CmdBeginRenderPass = ::vkCmdBeginRenderPass;
  PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = ::vkCmdBindDescriptorSets;
  PFN_vkCmdBindPipeline CmdBindPipeline = ::vkCmdBindPipeline;
  PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = ::vkCmdCopyBufferToImage;
  PFN_vkCmdDispatch CmdDispatch = ::vkCmdDispatch;
  PFN_vkCmdDraw CmdDraw = ::vkCmdDraw;
  PFN_vkCmdEndRenderPass CmdEndRenderPass = ::vkCmdEndRenderPass;
  PFN_vkCmdPipelineBarrier CmdPipelineBarrier = ::vkCmdPipelineBarrier;
  PFN_vkCmdPushConstants CmdPushConstants = ::vkCmdPushConstants;
  PFN_vkCmdResetQueryPool CmdResetQueryPool = ::vkCmdResetQueryPool;
  PFN_vkCmdSetScissor CmdSetScissor = ::vkCmdSetScissor;
  PFN_vkCmdSetViewport CmdSetViewport = ::vkCmdSetViewport;
  PFN_vkCmdWriteTimestamp CmdWriteTimestamp = ::vkCmdWriteTimestamp;
  PFN_vkCreateBuffer CreateBuffer = ::vkCreateBuffer;
  PFN_vkCreateCommandPool CreateCommandPool = ::vkCreateCommandPool;
  PFN_vkCreateComputePipelines CreateComputePipelines =
      ::vkCreateComputePipelines;
  PFN_vkCreateDescriptorPool CreateDescriptorPool = ::vkCreateDescriptorPool;
  PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout =
      ::vkCreateDescriptorSetLayout;
  PFN_vkCreateDevice CreateDevice = ::vkCreateDevice;
  PFN_vkCreateFence CreateFence = ::vkCreateFence;
  PFN_vkCreateFramebuffer CreateFramebuffer = ::vkCreateFramebuffer;
  PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines =
      ::vkCreateGraphicsPipelines;
  PFN_vkCreateImage CreateImage = ::vkCreateImage;
  PFN_vkCreateImageView CreateImageView = ::vkCreateImageView;
  PFN_vkCreateInstance CreateInstance = ::vkCreateInstance;
  PFN_vkCreatePipelineLayout CreatePipelineLayout = ::vkCreatePipelineLayout;
  PFN_vkCreateQueryPool CreateQueryPool = ::vkCreateQueryPool;
  PFN_vkCreateRenderPass CreateRenderPass = ::vkCreateRenderPass;
  PFN_vkCreateSampler CreateSampler = ::vkCreateSampler;
  PFN_vkCreateSemaphore CreateSemaphore = ::vkCreateSemaphore;
  PFN_vkDestroyBuffer DestroyBuffer = ::vkDestroyBuffer;
  PFN_vkDestroyCommandPool DestroyCommandPool = ::vkDestroyCommandPool;
  PFN_vkDestroyDescriptorPool DestroyDescriptorPool = ::vkDestroyDescriptorPool;
  PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout =
      ::vkDestroyDescriptorSetLayout;
  PFN_vkDestroyDevice DestroyDevice = ::vkDestroyDevice;
  PFN_vkDestroyFence DestroyFence = ::vkDestroyFence;
  PFN_vkDestroyFramebuffer DestroyFramebuffer = ::vkDestroyFramebuffer;
  PFN_vkDestroyImage DestroyImage = ::vkDestroyImage;
  PFN_vkDestroyImageView DestroyImageView = ::vkDestroyImageView;
  PFN_vkDestroyInstance DestroyInstance = ::vkDestroyInstance;
  PFN_vkDestroyPipeline DestroyPipeline = ::vkDestroyPipeline;
  PFN_vkDestroyPipelineLayout DestroyPipelineLayout = ::vkDestroyPipelineLayout;
  PFN_vkDestroyQueryPool DestroyQueryPool = ::vkDestroyQueryPool;
  PFN_vkDestroyRenderPass DestroyRenderPass = ::vkDestroyRenderPass;
  PFN_vkDestroySampler DestroySampler = ::vkDestroySampler;
  PFN_vkDestroySemaphore DestroySemaphore = ::vkDestroySemaphore;
  PFN_vkDeviceWaitIdle DeviceWaitIdle = ::vkDeviceWaitIdle;
  PFN_vkEndCommandBuffer EndCommandBuffer = ::vkEndCommandBuffer;
  PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties =
      ::vkEnumerateDeviceExtensionProperties;
  PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices =
      ::vkEnumeratePhysicalDevices;
  PFN_vkFreeMemory FreeMemory = ::vkFreeMemory;
  PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements =
      ::vkGetBufferMemoryRequirements;
  PFN_vkGetDeviceQueue GetDeviceQueue = ::vkGetDeviceQueue;
  PFN_vkGetFenceStatus GetFenceStatus = ::vkGetFenceStatus;
  PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 =
      ::vkGetPhysicalDeviceFeatures2;
  PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties =
      ::vkGetPhysicalDeviceProperties;
  PFN_vkGetPhysicalDeviceQueueFamilyProperties
      GetPhysicalDeviceQueueFamilyProperties =
          ::vkGetPhysicalDeviceQueueFamilyProperties;
  PFN_vkGetQueryPoolResults GetQueryPoolResults = ::vkGetQueryPoolResults;
  PFN_vkMapMemory MapMemory = ::vkMapMemory;
  PFN_vkQueueSubmit QueueSubmit = ::vkQueueSubmit;
  PFN_vkQueueWaitIdle QueueWaitIdle = ::vkQueueWaitIdle;
  PFN_vkResetCommandBuffer ResetCommandBuffer = ::vkResetCommandBuffer;
  PFN_vkResetFences ResetFences = ::vkResetFences;
  PFN_vkUnmapMemory UnmapMemory = ::vkUnmapMemory;
  PFN_vkUpdateDescriptorSets UpdateDescriptorSets = ::vkUpdateDescriptorSets;
  PFN_vkWaitForFences WaitForFences = ::vkWaitForFences;
};
extern XrVulkanDispatch xrVk;
void loadXrVulkanDispatch(VkInstance, VkDevice, PFN_vkGetInstanceProcAddr);
void resetXrVulkanDispatch();
} // namespace xrimmersive
#define vkAllocateCommandBuffers xrimmersive::xrVk.AllocateCommandBuffers
#define vkAllocateDescriptorSets xrimmersive::xrVk.AllocateDescriptorSets
#define vkAllocateMemory xrimmersive::xrVk.AllocateMemory
#define vkBeginCommandBuffer xrimmersive::xrVk.BeginCommandBuffer
#define vkBindBufferMemory xrimmersive::xrVk.BindBufferMemory
#define vkBindImageMemory xrimmersive::xrVk.BindImageMemory
#define vkCmdBeginRenderPass xrimmersive::xrVk.CmdBeginRenderPass
#define vkCmdBindDescriptorSets xrimmersive::xrVk.CmdBindDescriptorSets
#define vkCmdBindPipeline xrimmersive::xrVk.CmdBindPipeline
#define vkCmdCopyBufferToImage xrimmersive::xrVk.CmdCopyBufferToImage
#define vkCmdDispatch xrimmersive::xrVk.CmdDispatch
#define vkCmdDraw xrimmersive::xrVk.CmdDraw
#define vkCmdEndRenderPass xrimmersive::xrVk.CmdEndRenderPass
#define vkCmdPipelineBarrier xrimmersive::xrVk.CmdPipelineBarrier
#define vkCmdPushConstants xrimmersive::xrVk.CmdPushConstants
#define vkCmdResetQueryPool xrimmersive::xrVk.CmdResetQueryPool
#define vkCmdSetScissor xrimmersive::xrVk.CmdSetScissor
#define vkCmdSetViewport xrimmersive::xrVk.CmdSetViewport
#define vkCmdWriteTimestamp xrimmersive::xrVk.CmdWriteTimestamp
#define vkCreateBuffer xrimmersive::xrVk.CreateBuffer
#define vkCreateCommandPool xrimmersive::xrVk.CreateCommandPool
#define vkCreateComputePipelines xrimmersive::xrVk.CreateComputePipelines
#define vkCreateDescriptorPool xrimmersive::xrVk.CreateDescriptorPool
#define vkCreateDescriptorSetLayout xrimmersive::xrVk.CreateDescriptorSetLayout
#define vkCreateDevice xrimmersive::xrVk.CreateDevice
#define vkCreateFence xrimmersive::xrVk.CreateFence
#define vkCreateFramebuffer xrimmersive::xrVk.CreateFramebuffer
#define vkCreateGraphicsPipelines xrimmersive::xrVk.CreateGraphicsPipelines
#define vkCreateImage xrimmersive::xrVk.CreateImage
#define vkCreateImageView xrimmersive::xrVk.CreateImageView
#define vkCreateInstance xrimmersive::xrVk.CreateInstance
#define vkCreatePipelineLayout xrimmersive::xrVk.CreatePipelineLayout
#define vkCreateQueryPool xrimmersive::xrVk.CreateQueryPool
#define vkCreateRenderPass xrimmersive::xrVk.CreateRenderPass
#define vkCreateSampler xrimmersive::xrVk.CreateSampler
#define vkCreateSemaphore xrimmersive::xrVk.CreateSemaphore
#define vkDestroyBuffer xrimmersive::xrVk.DestroyBuffer
#define vkDestroyCommandPool xrimmersive::xrVk.DestroyCommandPool
#define vkDestroyDescriptorPool xrimmersive::xrVk.DestroyDescriptorPool
#define vkDestroyDescriptorSetLayout                                           \
  xrimmersive::xrVk.DestroyDescriptorSetLayout
#define vkDestroyDevice xrimmersive::xrVk.DestroyDevice
#define vkDestroyFence xrimmersive::xrVk.DestroyFence
#define vkDestroyFramebuffer xrimmersive::xrVk.DestroyFramebuffer
#define vkDestroyImage xrimmersive::xrVk.DestroyImage
#define vkDestroyImageView xrimmersive::xrVk.DestroyImageView
#define vkDestroyInstance xrimmersive::xrVk.DestroyInstance
#define vkDestroyPipeline xrimmersive::xrVk.DestroyPipeline
#define vkDestroyPipelineLayout xrimmersive::xrVk.DestroyPipelineLayout
#define vkDestroyQueryPool xrimmersive::xrVk.DestroyQueryPool
#define vkDestroyRenderPass xrimmersive::xrVk.DestroyRenderPass
#define vkDestroySampler xrimmersive::xrVk.DestroySampler
#define vkDestroySemaphore xrimmersive::xrVk.DestroySemaphore
#define vkDeviceWaitIdle xrimmersive::xrVk.DeviceWaitIdle
#define vkEndCommandBuffer xrimmersive::xrVk.EndCommandBuffer
#define vkEnumerateDeviceExtensionProperties                                   \
  xrimmersive::xrVk.EnumerateDeviceExtensionProperties
#define vkEnumeratePhysicalDevices xrimmersive::xrVk.EnumeratePhysicalDevices
#define vkFreeMemory xrimmersive::xrVk.FreeMemory
#define vkGetBufferMemoryRequirements                                          \
  xrimmersive::xrVk.GetBufferMemoryRequirements
#define vkGetDeviceQueue xrimmersive::xrVk.GetDeviceQueue
#define vkGetFenceStatus xrimmersive::xrVk.GetFenceStatus
#define vkGetPhysicalDeviceFeatures2                                           \
  xrimmersive::xrVk.GetPhysicalDeviceFeatures2
#define vkGetPhysicalDeviceProperties                                          \
  xrimmersive::xrVk.GetPhysicalDeviceProperties
#define vkGetPhysicalDeviceQueueFamilyProperties                               \
  xrimmersive::xrVk.GetPhysicalDeviceQueueFamilyProperties
#define vkGetQueryPoolResults xrimmersive::xrVk.GetQueryPoolResults
#define vkMapMemory xrimmersive::xrVk.MapMemory
#define vkQueueSubmit xrimmersive::xrVk.QueueSubmit
#define vkQueueWaitIdle xrimmersive::xrVk.QueueWaitIdle
#define vkResetCommandBuffer xrimmersive::xrVk.ResetCommandBuffer
#define vkResetFences xrimmersive::xrVk.ResetFences
#define vkUnmapMemory xrimmersive::xrVk.UnmapMemory
#define vkUpdateDescriptorSets xrimmersive::xrVk.UpdateDescriptorSets
#define vkWaitForFences xrimmersive::xrVk.WaitForFences
