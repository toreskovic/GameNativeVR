#include <cassert>
#include <iostream>
#include <vector>
#include <vulkan/vulkan.h>
int main() {
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_3;
  const char *layer = "VK_LAYER_GN_vr_foveation";
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;
  ci.enabledLayerCount = 1;
  ci.ppEnabledLayerNames = &layer;
  VkInstance instance{};
  assert(vkCreateInstance(&ci, nullptr, &instance) == VK_SUCCESS);
  uint32_t count = 0;
  assert(vkEnumeratePhysicalDevices(instance, &count, nullptr) == VK_SUCCESS &&
         count);
  std::vector<VkPhysicalDevice> physical(count);
  vkEnumeratePhysicalDevices(instance, &count, physical.data());
  vkGetPhysicalDeviceQueueFamilyProperties(physical[0], &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical[0], &count,
                                           families.data());
  uint32_t family = 0;
  while (!(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
    ++family;
  VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  float priority = 1;
  qc.queueFamilyIndex = family;
  qc.queueCount = 1;
  qc.pQueuePriorities = &priority;
  VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dc.queueCreateInfoCount = 1;
  dc.pQueueCreateInfos = &qc;
  VkDevice device{};
  assert(vkCreateDevice(physical[0], &dc, nullptr, &device) == VK_SUCCESS);
  VkQueue queue{};
  vkGetDeviceQueue(device, family, 0, &queue);
  VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pc.queueFamilyIndex = family;
  VkCommandPool pool{};
  assert(vkCreateCommandPool(device, &pc, nullptr, &pool) == VK_SUCCESS);
  VkCommandBufferAllocateInfo ac{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ac.commandPool = pool;
  ac.commandBufferCount = 1;
  VkCommandBuffer cmd{};
  assert(vkAllocateCommandBuffers(device, &ac, &cmd) == VK_SUCCESS);
  VkCommandBufferBeginInfo bc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  assert(vkBeginCommandBuffer(cmd, &bc) == VK_SUCCESS);
  assert(vkEndCommandBuffer(cmd) == VK_SUCCESS);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  assert(vkQueueSubmit(queue, 1, &si, {}) == VK_SUCCESS);
  assert(vkQueueWaitIdle(queue) == VK_SUCCESS);
  vkDestroyCommandPool(device, pool, nullptr);
  vkDestroyDevice(device, nullptr);
  vkDestroyInstance(instance, nullptr);
  std::cout << "Real Vulkan loader/device/command-buffer/queue pass-through "
               "succeeded\n";
}
