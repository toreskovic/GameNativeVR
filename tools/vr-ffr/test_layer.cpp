// Host-side dispatch regression tests. These do not emulate GPU rasterization.
#include "../../app/src/main/cpp/vrffr/layer.cpp"
#include <cassert>
#include <iostream>
namespace {
struct FakeHandle {
  void *dispatch;
};
int deviceTable, instanceTable;
FakeHandle ih{&instanceTable}, ph{&instanceTable}, dh{&deviceTable},
    ch{&deviceTable};
VkInstance instance = reinterpret_cast<VkInstance>(&ih);
VkPhysicalDevice physical = reinterpret_cast<VkPhysicalDevice>(&ph);
VkDevice device = reinterpret_cast<VkDevice>(&dh);
VkCommandBuffer command = reinterpret_cast<VkCommandBuffer>(&ch);
bool extension = true, regular = true, reject = false, attached = false,
     patchedPipeline = false;
unsigned creates = 0;
VkResult VKAPI_CALL fakeCreate(VkPhysicalDevice, const VkDeviceCreateInfo *ci,
                               const VkAllocationCallbacks *, VkDevice *out) {
  creates++;
  bool enabled = false;
  for (auto p = (const VkBaseInStructure *)ci->pNext; p; p = p->pNext)
    if (p->sType ==
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT)
      enabled = true;
  if (reject && enabled)
    return VK_ERROR_FEATURE_NOT_PRESENT;
  *out = device;
  return VK_SUCCESS;
}
void VKAPI_CALL fakeFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *f) {
  auto p = (VkPhysicalDeviceFragmentDensityMapFeaturesEXT *)f->pNext;
  p->fragmentDensityMap = extension;
  p->fragmentDensityMapNonSubsampledImages = regular;
}
void VKAPI_CALL fakeProperties(VkPhysicalDevice,
                               VkPhysicalDeviceProperties2 *p) {
  auto f = (VkPhysicalDeviceFragmentDensityMapPropertiesEXT *)p->pNext;
  f->maxFragmentDensityTexelSize = {32, 32};
  f->minFragmentDensityTexelSize = {8, 8};
}
VkResult VKAPI_CALL fakeExtensions(VkPhysicalDevice, const char *, uint32_t *n,
                                   VkExtensionProperties *p) {
  *n = extension ? 1 : 0;
  if (p && extension)
    std::strcpy(p->extensionName, VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME);
  return VK_SUCCESS;
}
void VKAPI_CALL fakeQueues(VkPhysicalDevice, uint32_t *n,
                           VkQueueFamilyProperties *p) {
  *n = 1;
  if (p) {
    p->queueFlags = VK_QUEUE_GRAPHICS_BIT;
    p->queueCount = 2;
  }
}
constexpr VkFormatFeatureFlags formatFlags = VK_FORMAT_FEATURE_FRAGMENT_DENSITY_MAP_BIT_EXT |
                                             VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
VkFormatFeatureFlags oldFlags = formatFlags, modernFlags = formatFlags;
VkFormatFeatureFlags2 wideFlags = formatFlags;
VkResult imageResult = VK_SUCCESS;
bool modernAvailable = true, khrOnly = false;
void VKAPI_CALL fakeFormat(VkPhysicalDevice, VkFormat, VkFormatProperties *p) {
  p->optimalTilingFeatures = oldFlags;
}
void VKAPI_CALL fakeFormat2(VkPhysicalDevice, VkFormat, VkFormatProperties2 *p) {
  p->formatProperties.optimalTilingFeatures = modernFlags;
  if (p->pNext) static_cast<VkFormatProperties3 *>(p->pNext)->optimalTilingFeatures = wideFlags;
}
void VKAPI_CALL fakeGpu(VkPhysicalDevice, VkPhysicalDeviceProperties *p) {
  p->apiVersion = VK_API_VERSION_1_3;
}
VkResult VKAPI_CALL fakeImageFormat(VkPhysicalDevice,
    const VkPhysicalDeviceImageFormatInfo2 *info, VkImageFormatProperties2 *p) {
  assert(info->format == VK_FORMAT_R8G8_UNORM && info->type == VK_IMAGE_TYPE_2D);
  assert(info->tiling == VK_IMAGE_TILING_OPTIMAL && info->flags == 0);
  assert(info->usage == (VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT | VK_IMAGE_USAGE_TRANSFER_DST_BIT));
  p->imageFormatProperties.maxExtent = {4096, 4096, 1};
  p->imageFormatProperties.maxArrayLayers = 2;
  p->imageFormatProperties.sampleCounts = VK_SAMPLE_COUNT_1_BIT;
  return imageResult;
}
void VKAPI_CALL fakeMemory(VkPhysicalDevice,
                           VkPhysicalDeviceMemoryProperties *p) {
  *p = {};
}
VkResult VKAPI_CALL fakeLoaderData(VkDevice, void *) { return VK_SUCCESS; }
void VKAPI_CALL fakeBegin(VkCommandBuffer, const VkRenderingInfo *ci) {
  attached = false;
  for (auto p = (const VkBaseInStructure *)ci->pNext; p; p = p->pNext)
    if (p->sType ==
        VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_INFO_EXT)
      attached = true;
}
void VKAPI_CALL fakeEnd(VkCommandBuffer) {}
void VKAPI_CALL fakeDraw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, int32_t,
                         uint32_t) {}
unsigned indirectForwarded = 0;
void VKAPI_CALL fakeIndirect(VkCommandBuffer, VkBuffer buffer, VkDeviceSize offset,
                             uint32_t count, uint32_t stride) {
  assert(buffer == (VkBuffer)uintptr_t(99) && offset == 32 && stride == 20);
  assert(count <= 24); ++indirectForwarded;
}
void VKAPI_CALL fakeIndirectCount(VkCommandBuffer, VkBuffer buffer, VkDeviceSize offset,
                                  VkBuffer counts, VkDeviceSize countOffset, uint32_t maximum, uint32_t stride) {
  assert(buffer == (VkBuffer)uintptr_t(99) && offset == 32 && stride == 20);
  assert(counts == (VkBuffer)uintptr_t(98) && countOffset == 16 && maximum == 10000);
  ++indirectForwarded;
}
VkResult VKAPI_CALL fakeFence(VkDevice, VkFence) { return VK_SUCCESS; }
VkResult VKAPI_CALL fakePipelines(VkDevice, VkPipelineCache, uint32_t n,
                                  const VkGraphicsPipelineCreateInfo *ci,
                                  const VkAllocationCallbacks *, VkPipeline *) {
  patchedPipeline =
      n &&
      (ci[0].flags &
       VK_PIPELINE_CREATE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT);
  return VK_SUCCESS;
}
PFN_vkVoidFunction VKAPI_CALL fakeGipa(VkInstance, const char *n) {
#define MAP(name, f)                                                           \
  if (!strcmp(n, name))                                                        \
  return reinterpret_cast<PFN_vkVoidFunction>(f)
  MAP("vkCreateDevice", fakeCreate);
  MAP("vkGetPhysicalDeviceFeatures2", fakeFeatures);
  MAP("vkGetPhysicalDeviceProperties2", fakeProperties);
  MAP("vkEnumerateDeviceExtensionProperties", fakeExtensions);
  MAP("vkGetPhysicalDeviceQueueFamilyProperties", fakeQueues);
  MAP("vkGetPhysicalDeviceProperties", fakeGpu);
  if (modernAvailable) {
    if (!khrOnly) {
      MAP("vkGetPhysicalDeviceFormatProperties2", fakeFormat2);
      MAP("vkGetPhysicalDeviceImageFormatProperties2", fakeImageFormat);
    }
    MAP("vkGetPhysicalDeviceFormatProperties2KHR", fakeFormat2);
    MAP("vkGetPhysicalDeviceImageFormatProperties2KHR", fakeImageFormat);
  }
  MAP("vkGetPhysicalDeviceFormatProperties", fakeFormat);
  MAP("vkGetPhysicalDeviceMemoryProperties", fakeMemory);
  return nullptr;
}
unsigned legacyForwarded = 0;
void VKAPI_CALL fakeLegacy(VkCommandBuffer h, const VkRenderPassBeginInfo *ci,
                           VkSubpassContents contents) {
  assert(h == command && ci->renderArea.extent.width == 123 && contents == VK_SUBPASS_CONTENTS_INLINE);
  ++legacyForwarded;
}
void VKAPI_CALL fakeLegacy2(VkCommandBuffer h, const VkRenderPassBeginInfo *ci,
                            const VkSubpassBeginInfo *begin) {
  assert(h == command && ci->renderArea.extent.width == 123 && begin->contents == VK_SUBPASS_CONTENTS_INLINE);
  ++legacyForwarded;
}
unsigned executes = 0;
VkResult VKAPI_CALL fakeBeginCommand(VkCommandBuffer, const VkCommandBufferBeginInfo *) { return VK_SUCCESS; }
void VKAPI_CALL fakeExecute(VkCommandBuffer h, uint32_t count, const VkCommandBuffer *buffers) {
  assert(h == command && count == 1 && buffers[0]);
  ++executes;
}
void VKAPI_CALL fakeScissor(VkCommandBuffer, uint32_t, uint32_t, const VkRect2D *) {}
void VKAPI_CALL fakeViewport(VkCommandBuffer, uint32_t, uint32_t, const VkViewport *) {}
unsigned descriptorForwards = 0;
void VKAPI_CALL fakeBindDescriptors(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout,
    uint32_t, uint32_t, const VkDescriptorSet*, uint32_t, const uint32_t*) { ++descriptorForwards; }
void VKAPI_CALL fakeUpdateDescriptors(VkDevice, uint32_t, const VkWriteDescriptorSet*,
    uint32_t, const VkCopyDescriptorSet*) { ++descriptorForwards; }
void VKAPI_CALL fakeTemplateUpdate(VkDevice, VkDescriptorSet, VkDescriptorUpdateTemplate, const void*) { ++descriptorForwards; }
PFN_vkVoidFunction VKAPI_CALL fakeGdpa(VkDevice, const char *n) {
  MAP("vkCmdBindDescriptorSets", fakeBindDescriptors);
  MAP("vkUpdateDescriptorSets", fakeUpdateDescriptors);
  MAP("vkUpdateDescriptorSetWithTemplateKHR", fakeTemplateUpdate);

  MAP("vkBeginCommandBuffer", fakeBeginCommand);
  MAP("vkCmdExecuteCommands", fakeExecute);
  MAP("vkCmdSetScissor", fakeScissor);
  MAP("vkCmdSetViewport", fakeViewport);
  MAP("vkCmdBeginRenderPass", fakeLegacy);
  MAP("vkCmdBeginRenderPass2KHR", fakeLegacy2);
  MAP("vkCmdBeginRendering", fakeBegin);
  MAP("vkCmdEndRendering", fakeEnd);
  MAP("vkCmdDrawIndexed", fakeDraw);
  MAP("vkCmdDrawIndirect", fakeIndirect);
  MAP("vkCmdDrawIndexedIndirect", fakeIndirect);
  MAP("vkCmdDrawIndirectCount", fakeIndirectCount);
  MAP("vkCmdDrawIndirectCountKHR", fakeIndirectCount);
  MAP("vkCmdDrawIndirectCountAMD", fakeIndirectCount);
  MAP("vkCmdDrawIndexedIndirectCount", fakeIndirectCount);
  MAP("vkCmdDrawIndexedIndirectCountKHR", fakeIndirectCount);
  MAP("vkCmdDrawIndexedIndirectCountAMD", fakeIndirectCount);

  MAP("vkGetFenceStatus", fakeFence);
  MAP("vkCreateGraphicsPipelines", fakePipelines);
#undef MAP
  return nullptr;
}
Device *setup(unsigned queueCount = 1) {
  devices.clear();
  instances.clear();
  creates = 0;
  instances[key(instance)] = {instance, fakeGipa, nullptr};
  VkLayerDeviceLink link{};
  link.pfnNextGetInstanceProcAddr = fakeGipa;
  link.pfnNextGetDeviceProcAddr = fakeGdpa;
  VkLayerDeviceCreateInfo chain{VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO};
  chain.function = VK_LAYER_LINK_INFO;
  chain.u.pLayerInfo = &link;
  VkLayerDeviceCreateInfo loader{VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO};
  loader.function = VK_LOADER_DATA_CALLBACK;
  loader.u.pfnSetDeviceLoaderData = fakeLoaderData;
  chain.pNext = &loader;
  float priority[2] = {1, 1};
  VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  queue.queueCount = queueCount;
  queue.pQueuePriorities = priority;
  VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  ci.pNext = &chain;
  ci.queueCreateInfoCount = 1;
  ci.pQueueCreateInfos = &queue;
  VkDevice out{};
  assert(vkCreateDevice(physical, &ci, nullptr, &out) == VK_SUCCESS);
  assert(ci.pNext == &chain);
  return dev(out);
}
} // namespace
// Older dispatch/selection fixtures explicitly seed historical evidence. Keep
// those tests focused on dispatch gates; recurring-history integration below
// exercises the real recording path without this fixture adapter.
void beginWithHistoryFixture(VkCommandBuffer command, const VkRenderingInfo *ri) {
  auto d = dev(command);
  if (ri->colorAttachmentCount == 1 && ri->pColorAttachments) {
    auto v = d->views.find(ri->pColorAttachments[0].imageView);
    if (v != d->views.end()) {
      auto &im = d->images[v->second.image];
      unsigned signature = 0;
      if (ri->pDepthAttachment && ri->pDepthAttachment->imageView) {
        auto layout = ri->pDepthAttachment->imageLayout;
        bool readOnly = layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL ||
            layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL || layout == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL ||
            layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
        signature = (unsigned(ri->pColorAttachments[0].loadOp) & 3u) |
            ((unsigned(ri->pDepthAttachment->loadOp) & 3u) << 2) | (readOnly ? 16u : 0u) |
            ((ri->flags & VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT) ? 32u : 0u);
      }
      const auto &h = im.history[signature];
      im.recurring = {};
      auto &r = im.recurring;
      r.frame = ++d->recordingFrame;
      ffr::PassHistory::Slot slot{passKey(d, *ri)};
      slot.observations = h.passes;
      slot.recent.fill({h.passes, h.draws, h.indexed, h.indexedVertices});
      r.slots.push_back(slot);
    }
  }
  vkCmdBeginRendering(command, ri);
}
void testRecurringPasses(Device *d, VkImage image, const VkRenderingInfo &ri) {
  auto &im = d->images[image];
  im.recurring = {};
  for (unsigned frame = 0; frame < 12; ++frame) {
    ++d->recordingFrame;
    if (frame >= 4 && frame % 2 == 0) {
      // Insert an unrelated scope before the known scene. Absolute writer
      // positions shift, but the actual scene must remain foveated.
      auto extra = ri;
      auto extraDepth = *ri.pDepthAttachment;
      extraDepth.imageView = (VkImageView)uintptr_t(99102);
      extra.pDepthAttachment = &extraDepth;
      vkCmdBeginRendering(command, &extra);
      assert(!attached);
      vkCmdEndRendering(command);
    }
    vkCmdBeginRendering(command, &ri);
    assert(attached == (frame >= 3));
    for (unsigned i = 0; i < 24; ++i) vkCmdDrawIndexed(command, 360, 1, 0, 0, 0);
    vkCmdEndRendering(command);
    // Temporarily missing cheap writer must not invalidate the heavy writer.
    if (frame == 6) continue;
    // Same image, identical attachments/load operations, different writer.
    vkCmdBeginRendering(command, &ri);
    assert(!attached);
    vkCmdDrawIndexed(command, 6, 1, 0, 0, 0);
    vkCmdEndRendering(command);
  }
  assert(im.recurring.slots.size() == 3);
  assert(im.recurring.slots[0].observations >= 8);
  assert(im.recurring.slots[1].recent[0].draws == 0);
  // A changed depth attachment gets its own untrained identity.
  auto changed = ri;
  auto depth = *ri.pDepthAttachment;
  depth.imageView = (VkImageView)uintptr_t(99101);
  changed.pDepthAttachment = &depth;
  ++d->recordingFrame;
  vkCmdBeginRendering(command, &changed);
  assert(!attached && d->commands[command].passTicket.prediction.passes == 0);
  vkCmdEndRendering(command);
  im.recurring = {};
  im.history = {};
}
#define vkCmdBeginRendering beginWithHistoryFixture
int main() {
  // RGBA16 normalized scene/resolve targets must remain eligible alongside
  // float color buffers. Depth and integer data textures are still excluded.
  assert(colorFormat(VK_FORMAT_R16G16B16A16_UNORM));
  assert(colorFormat(VK_FORMAT_R16G16B16A16_SFLOAT));
  assert(!colorFormat(VK_FORMAT_D32_SFLOAT));
  assert(!colorFormat(VK_FORMAT_R16G16B16A16_UINT));
  setenv("GN_VR_FFR", "1", 1);
  setenv("GN_VR_FFR_ENGINE", "unity", 1);
  auto d = setup();
  assert(d && d->enabled);
  VkRenderPassBeginInfo legacyInfo{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  legacyInfo.renderArea.extent.width = 123;
  VkSubpassBeginInfo subpass{VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO};
  subpass.contents = VK_SUBPASS_CONTENTS_INLINE;
  vkCmdBeginRenderPass(command, &legacyInfo, VK_SUBPASS_CONTENTS_INLINE);
  auto legacy2 = reinterpret_cast<PFN_vkCmdBeginRenderPass2>(vkGetDeviceProcAddr(device, "vkCmdBeginRenderPass2KHR"));
  assert(legacy2);
  legacy2(command, &legacyInfo, &subpass);
  assert(legacyForwarded == 2 && d->legacyCalls == 0 && d->maps.empty());

  oldFlags = 0;
  assert(setup()->enabled); // modern query recovers a missing legacy flag
  modernFlags = 0;
  assert(setup()->enabled); // properties3 reports the full pair
  wideFlags = 0;
  assert(!setup()->enabled); // no unsupported-format override
  oldFlags = VK_FORMAT_FEATURE_FRAGMENT_DENSITY_MAP_BIT_EXT;
  modernFlags = VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  assert(!setup()->enabled); // never combine incomplete answers
  oldFlags = modernFlags = 0;
  wideFlags = formatFlags;
  assert(!ffr::queryFormat(fakeGipa, instance, physical, false).supported());
  assert(!strcmp(ffr::queryFormat(fakeGipa, instance, physical, true).source, "properties3"));
  wideFlags = 0;
  modernFlags = formatFlags;
  assert(!strcmp(ffr::queryFormat(fakeGipa, instance, physical, false).source, "properties2"));
  modernFlags = 0;
  oldFlags = formatFlags;
  assert(!strcmp(ffr::queryFormat(fakeGipa, instance, physical, false).source, "legacy"));
  oldFlags = modernFlags = formatFlags;
  wideFlags = formatFlags;
  imageResult = VK_ERROR_FORMAT_NOT_SUPPORTED;
  assert(!setup()->enabled); // usage query vetoes positive feature flags
  imageResult = VK_ERROR_OUT_OF_HOST_MEMORY;
  assert(!setup()->enabled); // errors are not support
  imageResult = VK_SUCCESS;
  khrOnly = true;
  assert(setup()->enabled); // extension aliases work
  khrOnly = false;
  modernAvailable = false;
  assert(!setup()->enabled); // cannot confirm the exact image usage
  modernAvailable = true;
  setenv("GN_VR_FFR_ALLOW_MISSING_FORMAT_BIT", "1", 1);
  oldFlags = modernFlags = VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  wideFlags = VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
  assert(setup()->enabled); // experimental bypass of only the missing FDM bit
  imageResult = VK_ERROR_FORMAT_NOT_SUPPORTED;
  assert(!setup()->enabled);
  imageResult = VK_SUCCESS;
  oldFlags = modernFlags = 0;
  wideFlags = 0;
  assert(!setup()->enabled); // override cannot bypass transfer support
  oldFlags = modernFlags = VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  wideFlags = VK_FORMAT_FEATURE_2_TRANSFER_DST_BIT;
  regular = false;
  assert(!setup()->enabled);
  regular = true;
  extension = false;
  assert(!setup()->enabled);
  extension = true;
  assert(!setup(2)->enabled);
  reject = true;
  assert(!setup()->enabled && creates == 2);
  reject = false;
  setenv("GN_VR_FFR_ALLOW_MISSING_FORMAT_BIT", "0", 1);
  assert(!setup()->enabled);
  unsetenv("GN_VR_FFR_ALLOW_MISSING_FORMAT_BIT");
  oldFlags = modernFlags = formatFlags;
  wideFlags = formatFlags;
  regular = false;
  assert(!setup()->enabled);
  regular = true;
  extension = false;
  assert(!setup()->enabled);
  extension = true;
  assert(!setup(2)->enabled);
  reject = true;
  assert(!setup()->enabled && creates == 2);
  reject = false;
  d = setup();
  auto image = (VkImage)uintptr_t(100), eye = (VkImage)uintptr_t(101);
  auto view = (VkImageView)uintptr_t(102), depth = (VkImageView)uintptr_t(103);
  Image im;
  im.w = 1000;
  im.h = 1000;
  im.layers = im.mips = 1;
  im.eligible = true;
  d->images[image] = im;
  d->images[eye] = im;
  edge(d, image, eye, 500, 1000, {}, {}, 0, 0);
  assert(d->edges.empty()); // partial copy is not evidence
  edge(d, image, eye, 1000, 1000, {}, {}, 0, 0);
  assert(d->edges.size() == 1);
  gnFfrRegisterEye(device, eye, 0, 0, 0, 1000, 1000);
  assert(d->images[image].eyeCount == 1);
  d->images[eye].registered = false;
  propagate(d);
  assert(d->images[image].eyeCount == 0); // roots can disappear
  gnFfrRegisterEye(device, eye, 0, 0, 0, 1000, 1000);
  d->views[view] = {image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  Commands c;
  c.family = d->family;
  c.viewportCount = 1;
  c.viewports[0] = {0, 0, 1000, 1000, 0, 1};
  d->commands[command] = c;
  auto ready = std::make_unique<Map>();
  ready->w = ready->h = 1000;
  ready->eyeCount = 1;
  ready->eyes[0] = {0, 0, 1000, 1000};
  ready->submitted = true;
  ready->view = (VkImageView)uintptr_t(200);
  ready->fence = (VkFence)uintptr_t(201);
  d->maps.push_back(std::move(ready));
  VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  color.imageView = view;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  VkRenderingAttachmentInfo z{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  z.imageView = depth;
  z.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  z.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea.extent = {1000, 1000};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &color;
  ri.pDepthAttachment = &z;
  testRecurringPasses(d, image, ri);
  for (int pass = 0; pass < 4; pass++) {
    vkCmdBeginRendering(command, &ri);
    assert(attached == (pass >= 3));
    assert(ri.pNext == nullptr);
    for (int i = 0; i < 24; i++)
      vkCmdDrawIndexed(command, 36, 1, 0, 0, 0);
    vkCmdEndRendering(command);
  }
  // Reproduce DXVK's secondary-command-buffer scene path with read-only depth.
  FakeHandle secondaryHandle{&deviceTable};
  auto secondary = reinterpret_cast<VkCommandBuffer>(&secondaryHandle);
  Commands child;
  child.blocked = true;
  child.family = d->family;
  d->commands[secondary] = child;
  VkCommandBufferInheritanceRenderingInfo inherited{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO};
  inherited.colorAttachmentCount = 1;
  VkCommandBufferInheritanceInfo inheritance{VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
  inheritance.pNext = &inherited;
  VkCommandBufferBeginInfo beginSecondary{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  beginSecondary.flags = VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
  beginSecondary.pInheritanceInfo = &inheritance;
  assert(vkBeginCommandBuffer(secondary, &beginSecondary) == VK_SUCCESS);
  VkViewport sceneViewportTest{0, 0, 1000, 1000, 0, 1};
  vkCmdSetViewport(secondary, 0, 1, &sceneViewportTest);
  for (int i = 0; i < 24; ++i) vkCmdDrawIndexed(secondary, 36, 1, 0, 0, 0);
  assert(d->commands[secondary].batches.size() == 1);
  ri.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
  z.imageLayout = VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
  for (int pass = 0; pass < 4; ++pass) {
    vkCmdBeginRendering(command, &ri);
    assert(attached == (pass >= 3));
    vkCmdExecuteCommands(command, 1, &secondary);
    assert(d->commands[command].draws == 24 && d->commands[command].indexed == 24);
    vkCmdEndRendering(command);
  }
  assert(executes == 4 && ri.pNext == nullptr && inheritance.pNext == &inherited);
  // Indirect-only secondary scene: preserve arguments, learn geometry and attach
  // the map after warmup without reading the GPU argument buffer.
  for (auto &history : d->images[image].history) history = {};
  vkBeginCommandBuffer(secondary, &beginSecondary);
  vkCmdSetViewport(secondary, 0, 1, &sceneViewportTest);
  VkBuffer argumentBuffer = (VkBuffer)uintptr_t(99);
  vkCmdDrawIndexedIndirect(secondary, argumentBuffer, 32, 24, 20);
  assert(d->commands[secondary].batches[0].draws == 24);
  for (int pass = 0; pass < 4; ++pass) {
    vkCmdBeginRendering(command, &ri);
    assert(attached == (pass >= 3));
    vkCmdExecuteCommands(command, 1, &secondary);
    assert(d->commands[command].indexed == 24);
    vkCmdEndRendering(command);
  }
  // Empty indirect submissions do not train; GPU counts are not maxDrawCount.
  vkBeginCommandBuffer(secondary, &beginSecondary);
  vkCmdSetViewport(secondary, 0, 1, &sceneViewportTest);
  vkCmdDrawIndexedIndirect(secondary, argumentBuffer, 32, 0, 20);
  assert(d->commands[secondary].batches.empty());
  const char *countNames[] = {"vkCmdDrawIndirectCount", "vkCmdDrawIndirectCountKHR",
    "vkCmdDrawIndirectCountAMD", "vkCmdDrawIndexedIndirectCount",
    "vkCmdDrawIndexedIndirectCountKHR", "vkCmdDrawIndexedIndirectCountAMD"};
  for (auto name : countNames) {
    auto call = reinterpret_cast<PFN_vkCmdDrawIndirectCount>(vkGetDeviceProcAddr(device, name));
    assert(call);
    call(secondary, argumentBuffer, 32, (VkBuffer)uintptr_t(98), 16, 10000, 20);
  }
  assert(d->commands[secondary].batches[0].draws == 6);
  assert(d->commands[secondary].batches[0].indexed == 3);
  assert(d->commands[secondary].batches[0].indexedVertices == 0);
  assert(indirectForwarded == 8);
  // Non-indexed indirect work must never satisfy the indexed geometry gate.
  vkBeginCommandBuffer(secondary, &beginSecondary);
  vkCmdSetViewport(secondary, 0, 1, &sceneViewportTest);
  vkCmdDrawIndirect(secondary, argumentBuffer, 32, 24, 20);
  assert(d->commands[secondary].batches[0].indexed == 0);
  // Two large direct batches carry real geometry evidence through the child.
  vkBeginCommandBuffer(secondary, &beginSecondary);
  vkCmdSetViewport(secondary, 0, 1, &sceneViewportTest);
  vkCmdDrawIndexed(secondary, 384, 1, 0, 0, 0);
  vkCmdDrawIndexed(secondary, 96, 4, 0, 0, 0);
  vkCmdBeginRendering(command, &ri);
  vkCmdExecuteCommands(command, 1, &secondary);
  assert(d->commands[command].indexed == 2 && d->commands[command].indexedVertices == 768);
  vkCmdEndRendering(command);
  // Re-recording clears previous draws; nonmatching child viewports don't count.
  vkBeginCommandBuffer(secondary, &beginSecondary);
  assert(d->commands[secondary].batches.empty());
  sceneViewportTest.width = 100;
  vkCmdSetViewport(secondary, 0, 1, &sceneViewportTest);
  vkCmdDrawIndexed(secondary, 36, 1, 0, 0, 0);
  vkCmdBeginRendering(command, &ri);
  vkCmdExecuteCommands(command, 1, &secondary);
  assert(d->commands[command].draws == 0);
  vkCmdEndRendering(command);
  // Legacy inheritance must not train the dynamic-rendering heuristic.
  inheritance.renderPass = reinterpret_cast<VkRenderPass>(uintptr_t(9));
  vkBeginCommandBuffer(secondary, &beginSecondary);
  assert(!d->commands[secondary].inherited && d->commands[secondary].batches.empty());
  inheritance.renderPass = VK_NULL_HANDLE;
  ri.flags = VK_RENDERING_SUSPENDING_BIT;
  vkCmdBeginRendering(command, &ri);
  assert(!attached);
  vkCmdEndRendering(command);
  ri.flags = 0;
  z.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  ri.pDepthAttachment = nullptr;
  vkCmdBeginRendering(command, &ri);
  assert(!attached);
  vkCmdEndRendering(command);
  assert(d->rejections.empty() && d->rawDraws == 0 && d->dynamicCalls == 0);
  d->debug = true;
  vkCmdBeginRendering(command, &ri);
  assert(!attached && d->rejections["missing_or_unsupported_depth"] == 1);
  vkCmdEndRendering(command);
  d->debug = false;

  ri.pDepthAttachment = &z;
  ri.viewMask = 4;
  vkCmdBeginRendering(command, &ri);
  assert(!attached);
  vkCmdEndRendering(command);
  ri.viewMask = 0;
  ri.layerCount = 3;
  vkCmdBeginRendering(command, &ri);
  assert(!attached);
  vkCmdEndRendering(command);
  ri.layerCount = 1;
  ri.renderArea.extent.width = 500;
  vkCmdBeginRendering(command, &ri);
  assert(!attached);
  vkCmdEndRendering(command);
  ri.renderArea.extent.width = 1000;
  VkGraphicsPipelineCreateInfo pi{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  VkPipeline output{};
  vkCreateGraphicsPipelines(device, {}, 1, &pi, nullptr, &output);
  assert(patchedPipeline && pi.flags == 0);
  pi.renderPass = (VkRenderPass)uintptr_t(1);
  vkCreateGraphicsPipelines(device, {}, 1, &pi, nullptr, &output);
  assert(!patchedPipeline);
  assert(vkGetDeviceProcAddr(device, "vkUnsupportedNotReal") == nullptr);
  // flags2 overrides flags; preserve an application's chained rendering info.
  VkPipelineCreateFlags2CreateInfoKHR flags2{
      VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR};
  VkPipelineRenderingCreateInfo rendering{
      VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  rendering.pNext = &flags2;
  VkGraphicsPipelineCreateInfo chained{
      VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  chained.pNext = &rendering;
  std::vector<std::unique_ptr<uint8_t[]>> storage;
  assert(patchFlags2(chained, storage));
  auto patchedRendering =
      static_cast<const VkPipelineRenderingCreateInfo *>(chained.pNext);
  auto patchedFlags = static_cast<const VkPipelineCreateFlags2CreateInfoKHR *>(
      patchedRendering->pNext);
  assert(patchedFlags->flags &
         VK_PIPELINE_CREATE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT);
  assert(flags2.flags == 0 && rendering.pNext == &flags2);

  // An unconnected but repeatedly scene-like target is aggressive-only.
  auto guessed = (VkImage)uintptr_t(301);
  auto scene = d->images[image];
  scene.registered = false;
  scene.eyeCount = 0;
  scene.eyes = {};
  scene.history[5] = {8, 24, 8};
  d->images[guessed] = scene;
  d->views[view].image = guessed;
  d->mode = 1;
  vkCmdBeginRendering(command, &ri);
  assert(!attached);
  for (int i = 0; i < 24; i++)
    vkCmdDrawIndexed(command, 36, 1, 0, 0, 0);
  vkCmdEndRendering(command);
  d->mode = 2;
  vkCmdBeginRendering(command, &ri);
  assert(attached);
  vkCmdEndRendering(command);
  z.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
  vkCmdBeginRendering(command, &ri);
  assert(!attached);
  vkCmdEndRendering(command);
  z.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  // Ordinary layered stereo shares one density map; multiview needs two layers.
  d->views[view].image = image;
  d->images[image].layers = 2;
  d->images[image].history[5] = {8, 24, 8};
  ri.layerCount = 2;
  vkCmdBeginRendering(command, &ri);
  assert(attached);
  for (int i = 0; i < 24; i++)
    vkCmdDrawIndexed(command, 36, 1, 0, 0, 0);
  vkCmdEndRendering(command);
  auto stereo = std::make_unique<Map>(*d->maps.front());
  stereo->layers = 2;
  d->maps.push_back(std::move(stereo));
  ri.layerCount = 1;
  ri.viewMask = 3;
  vkCmdBeginRendering(command, &ri);
  assert(attached);
  vkCmdEndRendering(command);

  ffr::Evidence e{true, true, false, true, 8, 24, 8, false};
  assert(!ffr::select(e, 1) && ffr::select(e, 2));
  e.draws = 16;
  assert(!ffr::select(e, 2));
  e.knownEngine = true;
  assert(ffr::select(e, 2));
  e.depth = false;
  assert(!ffr::select(e, 2));
  e.depth = true;
  e.matchesEye = false;
  assert(!ffr::select(e, 2));
  e.connected = true;
  assert(ffr::select(e, 1));
  e.observations = 2;
  assert(!ffr::select(e, 1));
  ffr::Evidence batched{true, true, true, false, 8, 2, 2, true, 768};
  assert(ffr::select(batched, 1) && ffr::select(batched, 2));
  batched.connected = false; batched.matchesEye = true;
  assert(!ffr::select(batched, 2)); // Size alone cannot relax the threshold.
  batched.connected = true; batched.indexedVertices = 72;
  assert(!ffr::select(batched, 2)); // Two small meshes are not enough.
  batched.indexedVertices = 768; batched.observations = 7;
  assert(!ffr::select(batched, 2));
  batched.observations = 8; batched.depth = false;
  assert(!ffr::select(batched, 2));
  ffr::Rect eyes[2] = {{0, 0, 1000, 1000}, {1000, 0, 1000, 1000}};
  assert(ffr::density(500, 500, eyes, 2) == 255 &&
         ffr::density(1500, 500, eyes, 2) == 255);
  assert(ffr::density(800, 500, eyes, 2) == 128 &&
         ffr::density(1, 1, eyes, 2) == 64);
  // MRT diagnostics must observe geometry without attaching a density map or
  // training the single-color selection history.
  d->debug = true;
  const auto mrtImage = (VkImage)uintptr_t(810);
  const auto mrtView = (VkImageView)uintptr_t(811);
  const auto depthImage = (VkImage)uintptr_t(812);
  d->images[mrtImage] = im;
  d->images[depthImage] = im;
  d->views[mrtView] = {mrtImage, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}, VK_FORMAT_R16G16B16A16_SFLOAT};
  d->views[depth] = {depthImage, {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1}, VK_FORMAT_D32_SFLOAT};
  VkRenderingAttachmentInfo colors[] = {color, color};
  colors[1].imageView = mrtView;
  auto mrt = ri;
  mrt.flags = 0;
  mrt.colorAttachmentCount = 2;
  mrt.pColorAttachments = colors;
  auto candidates = d->gbufferCandidates;
  auto beforeHistory = d->images[image].history;
  vkCmdBeginRendering(command, &mrt);
  assert(!attached && !d->commands[command].color);
  assert(d->commands[command].mrtActive == 2 && d->commands[command].mrtAligned);
  assert(d->commands[command].mrtDescription.find("format=97,size=1000x1000") != std::string::npos);
  vkCmdDrawIndexed(command, 6, 2, 0, 0, 0); // Even small draws count diagnostically.
  assert(d->commands[command].diagnosticDraws == 1);
  assert(d->commands[command].diagnosticVertices == 12);
  vkCmdEndRendering(command);
  assert(d->gbufferCandidates == ++candidates);
  assert(!d->commands[command].mrtSlots && !d->commands[command].diagnosticDraws);
  for (unsigned j = 0; j < beforeHistory.size(); ++j)
    assert(d->images[image].history[j].passes == beforeHistory[j].passes);

  inherited.colorAttachmentCount = 2;
  inherited.viewMask = mrt.viewMask;
  vkBeginCommandBuffer(secondary, &beginSecondary);
  assert(d->commands[secondary].diagnosticInheritance && !d->commands[secondary].inherited);
  vkCmdDrawIndexed(secondary, 36, 3, 0, 0, 0);
  vkCmdDrawIndexedIndirect(secondary, argumentBuffer, 32, 24, 20);
  mrt.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
  vkCmdBeginRendering(command, &mrt);
  vkCmdExecuteCommands(command, 1, &secondary);
  assert(!attached && d->commands[command].draws == 0);
  assert(d->commands[command].diagnosticDraws == 2);
  assert(d->commands[command].diagnosticIndexed == 2);
  assert(d->commands[command].diagnosticIndirect == 1);
  assert(d->commands[command].diagnosticVertices == 108);
  vkCmdEndRendering(command);
  assert(d->gbufferCandidates == ++candidates);
  mrt.flags = 0;
  // Missing depth, mismatched dimensions, null slots and no draws are not candidates.
  for (int variant = 0; variant < 4; ++variant) {
    mrt.pDepthAttachment = variant == 0 ? nullptr : &z;
    d->images[mrtImage].w = variant == 1 ? 500 : 1000;
    colors[1].imageView = variant == 2 ? VK_NULL_HANDLE : mrtView;
    vkCmdBeginRendering(command, &mrt);
    if (variant != 3) vkCmdDrawIndexed(command, 36, 1, 0, 0, 0);
    vkCmdEndRendering(command);
    assert(d->gbufferCandidates == candidates && !attached);
  }
  d->debug = false;
  auto mrtPasses = d->mrtPasses;
  vkCmdBeginRendering(command, &mrt);
  vkCmdDrawIndexed(command, 36, 1, 0, 0, 0);
  assert(d->commands[command].mrtDescription.empty());
  vkCmdEndRendering(command);
  assert(d->mrtPasses == mrtPasses);
  // Eye tracing sees raw secondary draws even when production inheritance and
  // small-primitive filters reject them; it must not train normal selection.
  d->debug = true;
  inheritance.renderPass = (VkRenderPass)uintptr_t(9);
  vkBeginCommandBuffer(secondary, &beginSecondary);
  vkCmdDrawIndexed(secondary, 6, 4, 0, 0, 0);
  assert(!d->commands[secondary].inherited);
  assert(d->commands[secondary].diagnosticSmall == 1);
  assert(d->commands[secondary].traceSamples.find("count=6 instances=4") != std::string::npos);
  auto traced = ri;
  traced.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
  vkCmdBeginRendering(command, &traced);
  vkCmdExecuteCommands(command, 1, &secondary);
  assert(d->commands[command].diagnosticDraws == 1);
  assert(d->commands[command].diagnosticVertices == 24);
  assert(d->commands[command].diagnosticSmall == 1);
  assert(d->commands[command].draws == 0);
  assert(d->commands[command].traceChildren.find("acceptedInheritance=0") != std::string::npos);
  vkCmdEndRendering(command);
  assert(!d->eyeTracePatterns.empty());
  assert(d->commands[command].traceChildren.empty());
  VkImageSubresourceLayers sub{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  auto edgesBefore = d->edges.size();
  traceTransfer(d, "test-partial-copy", image, eye, 100, 100, {3,4,0}, {}, sub, sub);
  assert(!d->transferTracePatterns.empty() && d->edges.size() == edgesBefore);
  std::unordered_map<std::string, uint64_t> bounded;
  assert(traceOccurrence(bounded, "one", 1));
  assert(!traceOccurrence(bounded, "two", 1));
  for (int j = 2; j < 64; ++j) assert(!traceOccurrence(bounded, "one", 1));
  assert(traceOccurrence(bounded, "one", 1));
  assert(bounded.size() == 1);
  d->debug = false;
  vkBeginCommandBuffer(secondary, &beginSecondary);
  vkCmdDrawIndexed(secondary, 6, 4, 0, 0, 0);
  assert(d->commands[secondary].traceSamples.empty());
  // Descriptor updates, array spill, copies and strided templates.
  using namespace descriptor_trace;
  auto state = std::make_shared<Set>();
  state->layout = {{0, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT}},
                   {1, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2, VK_SHADER_STAGE_FRAGMENT_BIT}},
                   {2, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_VERTEX_BIT}}};
  VkDescriptorImageInfo inputs[3]{};
  inputs[0].imageView = view; inputs[1].imageView = mrtView; inputs[2].imageView = depth;
  write(*state, 0, 0, 3, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, inputs, sizeof(inputs[0]));
  assert(state->images.at({0,0}) == view && state->images.at({1,0}) == mrtView && state->images.at({1,1}) == depth);
  write(*state, 2, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, inputs, sizeof(inputs[0]));
  assert(!state->images.count({2,0})); // Fragment-only candidates.
  VkCopyDescriptorSet copyInfo{VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET};
  copyInfo.srcBinding = 0; copyInfo.dstBinding = 1; copyInfo.descriptorCount = 2;
  copy(*state, *state, copyInfo); // Snapshot overlapping ranges.
  assert(state->images.at({1,0}) == view && state->images.at({1,1}) == mrtView);
  auto set = (VkDescriptorSet)uintptr_t(990);
  auto layout = (VkPipelineLayout)uintptr_t(991);
  auto templ = (VkDescriptorUpdateTemplate)uintptr_t(992);
  state->pool = (VkDescriptorPool)uintptr_t(993);
  d->descriptorSets[set] = state;
  d->sampledTrace = true;
  Template t;
  t.entries.push_back({0, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, sizeof(VkDescriptorImageInfo), 2*sizeof(VkDescriptorImageInfo)});
  t.entries.push_back({1, 0, 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 0, 2*sizeof(VkDescriptorImageInfo)});
  d->descriptorTemplates[templ] = t;
  vkUpdateDescriptorSetWithTemplate(device, set, templ, inputs);
  assert(state->images.at({0,0}) == mrtView);
  assert(state->images.at({1,0}) == view && state->images.at({1,1}) == depth);
  VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w.dstSet = set; w.dstBinding = 1; w.descriptorCount = 2;
  w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
  VkDescriptorImageInfo empty[2]{}; w.pImageInfo = empty;
  vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
  assert(state->images.size() == 1);
  vkBeginCommandBuffer(secondary, &beginSecondary);
  vkCmdBindDescriptorSets(secondary, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
  vkCmdDrawIndexed(secondary, 6, 2, 0, 0, 0);
  assert(d->commands[secondary].sampledDraws.size() == 1);
  assert(d->commands[secondary].sampledDraws[0].views == std::vector<VkImageView>{mrtView});
  // A later descriptor update cannot rewrite the draw-recording snapshot.
  w.dstBinding = 0; w.descriptorCount = 1;
  vkUpdateDescriptorSets(device, 1, &w, 0, nullptr);
  assert(d->commands[secondary].sampledDraws[0].views[0] == mrtView);
  auto edgesSaved = d->edges.size();
  vkCmdBeginRendering(command, &traced);
  vkCmdExecuteCommands(command, 1, &secondary);
  vkCmdEndRendering(command);
  assert(!d->sampledReports.empty() && d->edges.size() == edgesSaved);
  auto reports = d->sampledReports.size();
  vkCmdBeginRendering(command, &traced);
  vkCmdExecuteCommands(command, 1, &secondary);
  vkCmdEndRendering(command);
  assert(d->sampledReports.size() == reports); // No repeat spam.
  forgetDescriptorPool(d, state->pool);
  assert(!state->valid && !d->descriptorSets.count(set));
  vkBeginCommandBuffer(secondary, &beginSecondary);
  assert(d->commands[secondary].sampledDraws.empty() && !d->commands[secondary].descriptorSets[0]);
  assert(descriptorForwards == 4);
  d->sampledTrace = false;
  d->mode = 1; // Conservative does not collect inferred sampled ancestry.
  vkCmdDrawIndexed(secondary, 6, 2, 0, 0, 0);
  assert(d->commands[secondary].sampledDraws.empty());
  // Generic sampled-input ancestry, independent of debug, engine and resolution.
  d->mode = 2; d->knownEngine = false;
  VkImage sceneImage = (VkImage)uintptr_t(2001), resolvedImage = (VkImage)uintptr_t(2002);
  VkImage postImage = (VkImage)uintptr_t(2003), outputImage = (VkImage)uintptr_t(2004);
  VkImageView resolvedView = (VkImageView)uintptr_t(2005), postView = (VkImageView)uintptr_t(2006);
  VkImageView outputView = (VkImageView)uintptr_t(2007);
  Image generic;
  generic.w = 864; generic.h = 960; generic.layers = 2; generic.mips = 1;
  generic.format = VK_FORMAT_R16G16B16A16_UNORM;
  generic.eligible = colorFormat(generic.format); generic.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  for (auto id : {sceneImage, resolvedImage, postImage, outputImage}) d->images[id] = generic;
  d->images[outputImage].registered = true; d->images[outputImage].eyeCount = 1;
  d->images[outputImage].eyes[0] = {0,0,864,960};
  VkImageSubresourceRange whole{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,2};
  d->views[resolvedView] = {resolvedImage, whole};
  d->views[postView] = {postImage, whole};
  d->views[outputView] = {outputImage, whole};
  edge(d, sceneImage, resolvedImage, 864, 960, {}, {}, 0, 0);
  auto fullPass = [&](VkImage target, VkImageView input) {
    Commands c; c.rendering = true; c.sampledPassAllowed = true; c.sampledTarget = target;
    c.sampledArea = {{0,0},{864,960}}; c.sampledRange = whole; c.sampledTotalDraws = 1;
    SampledDraw draw; draw.views = {input}; draw.viewportCount = draw.scissorCount = 1;
    draw.viewports[0] = {0,960,864,-960,0,1}; draw.scissors[0] = c.sampledArea;
    c.sampledDraws.push_back(draw); return c;
  };
  auto postPass = fullPass(postImage, resolvedView);
  auto outputPass = fullPass(outputImage, postView);
  std::array<ffr::Rect,2> inferredEyes{}; unsigned inferredCount = 0;
  d->sampledLinks.clear();
  for (unsigned j = 1; j <= 8; ++j) {
    ++d->sampledEpoch; learnSampled(d, postPass); learnSampled(d, outputPass);
    assert(sampledAncestry(d, sceneImage, inferredEyes, inferredCount) == (j == 8));
  }
  assert(inferredCount == 1 && inferredEyes[0].w == 864);
  assert(!d->images[sceneImage].eyeCount && !d->images[resolvedImage].eyeCount);
  auto mature = d->sampledLinks;
  ffr::Evidence inferred{true,true,false,false,8,2,2,false,4096,true};
  assert(ffr::select(inferred,2) && !ffr::select(inferred,1));
  inferred.depth = false; assert(!ffr::select(inferred,2)); inferred.depth = true;
  inferred.indexedVertices = 12; assert(!ffr::select(inferred,2));
  // Same recording epoch cannot prematurely warm up a link.
  d->sampledLinks.clear();
  for (unsigned j = 0; j < 20; ++j) learnSampled(d, outputPass);
  assert(d->sampledLinks[0].observations == 1);
  // Reject partial coverage, ambiguous scene inputs, mixed geometry, invalid
  // snapshots and non-full subresources. Auxiliary small textures are allowed.
  for (unsigned variant = 0; variant < 8; ++variant) {
    auto bad = outputPass; d->sampledLinks = mature;
    if (variant == 0) bad.sampledDraws[0].scissors[0].extent.width = 400;
    if (variant == 1) bad.sampledDraws[0].viewports[0].width = 400;
    if (variant == 2) bad.sampledDraws[0].views.push_back(resolvedView);
    if (variant == 3) bad.sampledTotalDraws = 2;
    if (variant == 4) bad.sampledRange.baseMipLevel = 1;
    if (variant == 5) bad.sampledDraws[0].complete = false;
    if (variant == 6) bad.sampledOverflow = true;
    if (variant == 7) bad.sampledPassAllowed = false;
    learnSampled(d,bad);
    assert(d->sampledLinks.size() == 1 && d->sampledLinks[0].destination == postImage);
  }
  d->sampledLinks = mature;
  auto changedSource = fullPass(outputImage, resolvedView);
  learnSampled(d, changedSource);
  assert(d->sampledLinks.back().source == resolvedImage && d->sampledLinks.back().observations == 1);
  d->sampledLinks = mature; d->sampledEpoch += 241;
  assert(!sampledAncestry(d, sceneImage, inferredEyes, inferredCount));
  // Exercise real bind/draw/secondary execution/end hooks with debug disabled.
  state = std::make_shared<Set>(); state->layout[0] = {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_FRAGMENT_BIT};
  state->images[{0,0}] = resolvedView; d->descriptorSets[set] = state;
  inheritance.renderPass = VK_NULL_HANDLE; inherited.colorAttachmentCount = 1;
  inherited.viewMask = 0;
  vkBeginCommandBuffer(secondary, &beginSecondary);
  VkViewport fullViewport{0,960,864,-960,0,1}; VkRect2D fullScissor{{0,0},{864,960}};
  vkCmdSetViewport(secondary, 0, 1, &fullViewport);
  vkCmdSetScissor(secondary, 0, 1, &fullScissor);
  vkCmdBindDescriptorSets(secondary, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
  vkCmdDrawIndexed(secondary, 6, 2, 0, 0, 0);
  VkRenderingAttachmentInfo finalColor{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  finalColor.imageView = outputView; finalColor.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo finalPass{VK_STRUCTURE_TYPE_RENDERING_INFO};
  finalPass.renderArea = fullScissor; finalPass.layerCount = 2;
  finalPass.flags = VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
  finalPass.colorAttachmentCount = 1; finalPass.pColorAttachments = &finalColor;
  d->sampledLinks.clear();
  for (unsigned j = 0; j < 8; ++j) {
    ++d->sampledEpoch;
    vkCmdBeginRendering(command, &finalPass);
    vkCmdExecuteCommands(command, 1, &secondary);
    vkCmdEndRendering(command);
  }
  assert(sampledAncestry(d, sceneImage, inferredEyes, inferredCount));
  auto goodDraw = d->commands[secondary].sampledDraws[0];
  assert(sampledCoverageRejection(goodDraw,generic) == nullptr);
  ++state->revision;
  assert(std::string(sampledCoverageRejection(goodDraw,generic)) == "descriptor-updated-after-draw");
  d->mode = 1;
  assert(!sampledAncestry(d, sceneImage, inferredEyes, inferredCount));
  d->maps.clear();
  devices.clear();
  std::cout << "FFR dispatch, selection, stereo mask, unsupported-device and "
               "fallback tests passed\n";
}
