// Experimental app-scoped Vulkan FDM layer. No shader rewriting or timing
// queries.
#include "selection.h"
#include "pass_history.h"
#include "capabilities.h"
#include <algorithm>
#include <android/log.h>
#include <array>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <sstream>
#include <unordered_map>
#include <vector>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>
#include "descriptor_trace.h"
#define EXPORT extern "C" __attribute__((visibility("default")))
#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "GN-VR-FFR", __VA_ARGS__)
#define DEBUG_LOG(d, ...) do { if ((d)->debug) LOG(__VA_ARGS__); } while (0)
namespace {
std::recursive_mutex lock;
void *key(const void *h) {
  return h ? *reinterpret_cast<void *const *>(h) : nullptr;
}
struct Instance {
  VkInstance handle;
  PFN_vkGetInstanceProcAddr gipa;
  PFN_vkGetDeviceProcAddr gdpa;
};
struct History {
  uint32_t passes = 0, draws = 0, indexed = 0, indexedVertices = 0;
};
struct Image {
  uint32_t w = 0, h = 0, layers = 0, mips = 0;
  VkImageUsageFlags usage = 0;
  VkFormat format{};
  bool eligible = false, registered = false;
  std::array<History, 64> history{}; // Legacy diagnostic fingerprint only.
  ffr::PassHistory recurring;
  uint64_t identity = 0;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  std::array<uint64_t, 64> reports{};
  // Registered projection subrectangles, or a proven full-image copy ancestor.
  std::array<ffr::Rect, 2> eyes{};
  unsigned eyeCount = 0;
};
struct View {
  VkImage image{};
  VkImageSubresourceRange range{};
  VkFormat format = VK_FORMAT_UNDEFINED;
};
struct DrawBatch {
  std::array<VkViewport, 2> viewports{};
  uint32_t count = 0, draws = 0, indexed = 0, indexedVertices = 0;
};
struct SampledDraw {
  std::vector<VkImageView> views;
  bool complete = true;
  std::array<VkViewport, 2> viewports{};
  std::array<VkRect2D, 2> scissors{};
  uint32_t viewportCount = 0, scissorCount = 0;
  std::vector<std::pair<std::shared_ptr<descriptor_trace::Set>, uint64_t>> revisions;
};
struct Commands {
  std::array<std::shared_ptr<descriptor_trace::Set>, 8> descriptorSets;
  VkPipelineLayout descriptorLayout{};
  std::vector<SampledDraw> sampledDraws;
  bool sampledOverflow = false, descriptorOverflow = false, sampledPassAllowed = false;
  uint32_t sampledTotalDraws = 0;
  std::array<VkRect2D, 2> scissors{};
  uint32_t scissorCount = 0;
  VkImage sampledTarget{};
  VkRect2D sampledArea{};
  VkImageSubresourceRange sampledRange{};
  VkCommandPool pool{};
  uint32_t family = ~0u;
  VkImage color{};
  ffr::PassHistory::Ticket passTicket{};
  bool fdmAttached = false;

  bool depth = false;
  uint32_t draws = 0, indexed = 0, signature = 0, indexedVertices = 0;
  bool rendering = false, blocked = false;
  bool inherited = false, secondaryContents = false;
  uint32_t viewMask = 0;
  bool diagnosticInheritance = false;
  uint32_t mrtSlots = 0, mrtActive = 0;
  bool mrtAligned = false, mrtDepth = false, mrtEyeMatch = false;
  std::string mrtDescription;
  VkImage diagnosticTarget{};
  std::string tracePass, traceInheritance, traceSamples, traceChildren;
  unsigned traceSampleCount = 0, traceChildCount = 0;
  uint64_t diagnosticSmall = 0;
  uint64_t diagnosticDraws = 0, diagnosticIndexed = 0, diagnosticIndirect = 0, diagnosticVertices = 0;
  std::vector<DrawBatch> batches;
  std::array<VkViewport, 2> viewports{};
  uint32_t viewportCount = 0;
};
struct Map {
  uint32_t w = 0, h = 0, layers = 1;
  std::array<ffr::Rect, 2> eyes{};
  unsigned eyeCount = 0;
  VkImage image{};
  VkImageView view{};
  VkDeviceMemory memory{}, stagingMemory{};
  VkBuffer staging{};
  VkCommandPool pool{};
  VkCommandBuffer cmd{};
  VkFence fence{};
  bool submitted = false, failed = false;
};
struct Device {
  VkDevice handle{};
  VkPhysicalDevice physical{};
  Instance instance{};
  PFN_vkGetDeviceProcAddr next{};
  PFN_vkSetDeviceLoaderData setLoaderData{};
  bool enabled = false, knownEngine = false, debug = false;
  bool sampledTrace = false;
  std::unordered_map<VkDescriptorSetLayout, descriptor_trace::Layout> descriptorLayouts;
  std::unordered_map<VkPipelineLayout, std::vector<descriptor_trace::Layout>> pipelineLayouts;
  std::unordered_map<VkDescriptorSet, std::shared_ptr<descriptor_trace::Set>> descriptorSets;
  std::unordered_map<VkDescriptorUpdateTemplate, descriptor_trace::Template> descriptorTemplates;
  std::unordered_map<std::string, uint64_t> sampledReports;
  struct SampledLink { VkImage source{}, destination{}; uint32_t observations = 0; uint64_t epoch = 0; };
  std::vector<SampledLink> sampledLinks;
  uint64_t sampledEpoch = 0;
  uint64_t recordingFrame = 0, imageIdentity = 0;
  unsigned sampledSelections = 0;
  std::unordered_map<std::string, unsigned> sampledGates;
  int mode = 0;
  uint32_t family = ~0u;
  VkPhysicalDeviceMemoryProperties memory{};
  VkPhysicalDeviceFragmentDensityMapPropertiesEXT props{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_PROPERTIES_EXT};
  std::unordered_map<VkImage, Image> images;
  std::unordered_map<VkImageView, View> views;
  std::unordered_map<VkCommandPool, uint32_t> pools;
  std::unordered_map<VkCommandBuffer, Commands> commands;

  struct Edge {
    VkImage source{}, destination{};
  };
  std::vector<Edge> edges;
  std::vector<std::unique_ptr<Map>> maps;
  uint64_t mrtPasses = 0, gbufferCandidates = 0;
  unsigned mrtReports = 0;
  std::unordered_map<std::string, uint64_t> eyeTracePatterns, transferTracePatterns;
  std::unordered_map<std::string, uint64_t> mrtPatterns;
  unsigned messages = 0;
  uint64_t dynamicCalls = 0, legacyCalls = 0, eyeCalls = 0;
  uint64_t rawDraws = 0, indirectCalls = 0, qualifiedDraws = 0, viewportRejects = 0;
  unsigned diagnosticLines = 0, secondaryMessages = 0;
  std::unordered_map<std::string, uint64_t> rejections;

  PFN_vkAllocateCommandBuffers vkAllocateCommandBuffers = nullptr;
  PFN_vkAllocateMemory vkAllocateMemory = nullptr;
  PFN_vkBeginCommandBuffer vkBeginCommandBuffer = nullptr;
  PFN_vkBindBufferMemory vkBindBufferMemory = nullptr;
  PFN_vkBindImageMemory vkBindImageMemory = nullptr;
  PFN_vkCmdBeginRendering vkCmdBeginRendering = nullptr;
  PFN_vkCmdCopyBufferToImage vkCmdCopyBufferToImage = nullptr;
  PFN_vkCmdCopyImage vkCmdCopyImage = nullptr;
  PFN_vkCmdCopyImage2 vkCmdCopyImage2 = nullptr;
  PFN_vkCmdDraw vkCmdDraw = nullptr;
  PFN_vkCmdDrawIndexed vkCmdDrawIndexed = nullptr;
  PFN_vkCmdDrawIndirect vkCmdDrawIndirect = nullptr;
  PFN_vkCmdDrawIndexedIndirect vkCmdDrawIndexedIndirect = nullptr;
  PFN_vkCmdDrawIndirectCount vkCmdDrawIndirectCount = nullptr;
  PFN_vkCmdDrawIndirectCount vkCmdDrawIndirectCountKHR = nullptr;
  PFN_vkCmdDrawIndirectCount vkCmdDrawIndirectCountAMD = nullptr;
  PFN_vkCmdDrawIndexedIndirectCount vkCmdDrawIndexedIndirectCount = nullptr;
  PFN_vkCmdDrawIndexedIndirectCount vkCmdDrawIndexedIndirectCountKHR = nullptr;
  PFN_vkCmdDrawIndexedIndirectCount vkCmdDrawIndexedIndirectCountAMD = nullptr;

  PFN_vkCmdEndRendering vkCmdEndRendering = nullptr;
  PFN_vkCmdPipelineBarrier vkCmdPipelineBarrier = nullptr;
  PFN_vkCmdResolveImage vkCmdResolveImage = nullptr;
  PFN_vkCmdResolveImage2 vkCmdResolveImage2 = nullptr;
  PFN_vkCreateBuffer vkCreateBuffer = nullptr;
  PFN_vkCreateCommandPool vkCreateCommandPool = nullptr;
  PFN_vkCreateFence vkCreateFence = nullptr;
  PFN_vkCreateGraphicsPipelines vkCreateGraphicsPipelines = nullptr;
  PFN_vkCreateImage vkCreateImage = nullptr;
  PFN_vkCreateImageView vkCreateImageView = nullptr;
  PFN_vkDestroyBuffer vkDestroyBuffer = nullptr;
  PFN_vkDestroyCommandPool vkDestroyCommandPool = nullptr;
  PFN_vkDestroyDevice vkDestroyDevice = nullptr;
  PFN_vkDestroyFence vkDestroyFence = nullptr;
  PFN_vkDestroyImage vkDestroyImage = nullptr;
  PFN_vkDestroyImageView vkDestroyImageView = nullptr;
  PFN_vkEndCommandBuffer vkEndCommandBuffer = nullptr;
  PFN_vkFreeCommandBuffers vkFreeCommandBuffers = nullptr;
  PFN_vkFreeMemory vkFreeMemory = nullptr;
  PFN_vkGetBufferMemoryRequirements vkGetBufferMemoryRequirements = nullptr;
  PFN_vkGetDeviceQueue vkGetDeviceQueue = nullptr;
  PFN_vkGetDeviceQueue2 vkGetDeviceQueue2 = nullptr;
  PFN_vkGetFenceStatus vkGetFenceStatus = nullptr;
  PFN_vkGetImageMemoryRequirements vkGetImageMemoryRequirements = nullptr;
  PFN_vkMapMemory vkMapMemory = nullptr;
  PFN_vkQueueSubmit vkQueueSubmit = nullptr;
  PFN_vkQueueSubmit2 vkQueueSubmit2 = nullptr;
  PFN_vkUnmapMemory vkUnmapMemory = nullptr;
  PFN_vkCmdSetViewport vkCmdSetViewport = nullptr;
  PFN_vkCmdSetViewportWithCount vkCmdSetViewportWithCount = nullptr;
  void loadFunctions() {
    vkCmdSetViewport = (PFN_vkCmdSetViewport)next(handle, "vkCmdSetViewport");
    vkCmdSetViewportWithCount = (PFN_vkCmdSetViewportWithCount)next(
        handle, "vkCmdSetViewportWithCount");
    if (!vkCmdSetViewportWithCount)
      vkCmdSetViewportWithCount = (PFN_vkCmdSetViewportWithCount)next(
          handle, "vkCmdSetViewportWithCountEXT");
    vkAllocateCommandBuffers = reinterpret_cast<PFN_vkAllocateCommandBuffers>(
        next(handle, "vkAllocateCommandBuffers"));
    vkAllocateMemory = reinterpret_cast<PFN_vkAllocateMemory>(
        next(handle, "vkAllocateMemory"));
    vkBeginCommandBuffer = reinterpret_cast<PFN_vkBeginCommandBuffer>(
        next(handle, "vkBeginCommandBuffer"));
    vkBindBufferMemory = reinterpret_cast<PFN_vkBindBufferMemory>(
        next(handle, "vkBindBufferMemory"));
    vkBindImageMemory = reinterpret_cast<PFN_vkBindImageMemory>(
        next(handle, "vkBindImageMemory"));
    vkCmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(
        next(handle, "vkCmdBeginRendering"));
    vkCmdCopyBufferToImage = reinterpret_cast<PFN_vkCmdCopyBufferToImage>(
        next(handle, "vkCmdCopyBufferToImage"));
    vkCmdCopyImage =
        reinterpret_cast<PFN_vkCmdCopyImage>(next(handle, "vkCmdCopyImage"));
    vkCmdCopyImage2 =
        reinterpret_cast<PFN_vkCmdCopyImage2>(next(handle, "vkCmdCopyImage2"));
    vkCmdDraw = reinterpret_cast<PFN_vkCmdDraw>(next(handle, "vkCmdDraw"));
    vkCmdDrawIndexed = reinterpret_cast<PFN_vkCmdDrawIndexed>(
        next(handle, "vkCmdDrawIndexed"));
    vkCmdDrawIndirect = reinterpret_cast<PFN_vkCmdDrawIndirect>(next(handle, "vkCmdDrawIndirect"));
    vkCmdDrawIndexedIndirect = reinterpret_cast<PFN_vkCmdDrawIndexedIndirect>(next(handle, "vkCmdDrawIndexedIndirect"));
    vkCmdDrawIndirectCount = reinterpret_cast<PFN_vkCmdDrawIndirectCount>(next(handle, "vkCmdDrawIndirectCount"));
    vkCmdDrawIndirectCountKHR = reinterpret_cast<PFN_vkCmdDrawIndirectCount>(next(handle, "vkCmdDrawIndirectCountKHR"));
    vkCmdDrawIndirectCountAMD = reinterpret_cast<PFN_vkCmdDrawIndirectCount>(next(handle, "vkCmdDrawIndirectCountAMD"));
    vkCmdDrawIndexedIndirectCount = reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCount>(next(handle, "vkCmdDrawIndexedIndirectCount"));
    vkCmdDrawIndexedIndirectCountKHR = reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCount>(next(handle, "vkCmdDrawIndexedIndirectCountKHR"));
    vkCmdDrawIndexedIndirectCountAMD = reinterpret_cast<PFN_vkCmdDrawIndexedIndirectCount>(next(handle, "vkCmdDrawIndexedIndirectCountAMD"));
    vkCmdEndRendering = reinterpret_cast<PFN_vkCmdEndRendering>(
        next(handle, "vkCmdEndRendering"));
    vkCmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(
        next(handle, "vkCmdPipelineBarrier"));
    vkCmdResolveImage = reinterpret_cast<PFN_vkCmdResolveImage>(
        next(handle, "vkCmdResolveImage"));
    vkCmdResolveImage2 = reinterpret_cast<PFN_vkCmdResolveImage2>(
        next(handle, "vkCmdResolveImage2"));
    vkCreateBuffer =
        reinterpret_cast<PFN_vkCreateBuffer>(next(handle, "vkCreateBuffer"));
    vkCreateCommandPool = reinterpret_cast<PFN_vkCreateCommandPool>(
        next(handle, "vkCreateCommandPool"));
    vkCreateFence =
        reinterpret_cast<PFN_vkCreateFence>(next(handle, "vkCreateFence"));
    vkCreateGraphicsPipelines = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(
        next(handle, "vkCreateGraphicsPipelines"));
    vkCreateImage =
        reinterpret_cast<PFN_vkCreateImage>(next(handle, "vkCreateImage"));
    vkCreateImageView = reinterpret_cast<PFN_vkCreateImageView>(
        next(handle, "vkCreateImageView"));
    vkDestroyBuffer =
        reinterpret_cast<PFN_vkDestroyBuffer>(next(handle, "vkDestroyBuffer"));
    vkDestroyCommandPool = reinterpret_cast<PFN_vkDestroyCommandPool>(
        next(handle, "vkDestroyCommandPool"));
    vkDestroyDevice =
        reinterpret_cast<PFN_vkDestroyDevice>(next(handle, "vkDestroyDevice"));
    vkDestroyFence =
        reinterpret_cast<PFN_vkDestroyFence>(next(handle, "vkDestroyFence"));
    vkDestroyImage =
        reinterpret_cast<PFN_vkDestroyImage>(next(handle, "vkDestroyImage"));
    vkDestroyImageView = reinterpret_cast<PFN_vkDestroyImageView>(
        next(handle, "vkDestroyImageView"));
    vkEndCommandBuffer = reinterpret_cast<PFN_vkEndCommandBuffer>(
        next(handle, "vkEndCommandBuffer"));
    vkFreeCommandBuffers = reinterpret_cast<PFN_vkFreeCommandBuffers>(
        next(handle, "vkFreeCommandBuffers"));
    vkFreeMemory =
        reinterpret_cast<PFN_vkFreeMemory>(next(handle, "vkFreeMemory"));
    vkGetBufferMemoryRequirements =
        reinterpret_cast<PFN_vkGetBufferMemoryRequirements>(
            next(handle, "vkGetBufferMemoryRequirements"));
    vkGetDeviceQueue = reinterpret_cast<PFN_vkGetDeviceQueue>(
        next(handle, "vkGetDeviceQueue"));
    vkGetDeviceQueue2 = reinterpret_cast<PFN_vkGetDeviceQueue2>(
        next(handle, "vkGetDeviceQueue2"));
    vkGetFenceStatus = reinterpret_cast<PFN_vkGetFenceStatus>(
        next(handle, "vkGetFenceStatus"));
    vkGetImageMemoryRequirements =
        reinterpret_cast<PFN_vkGetImageMemoryRequirements>(
            next(handle, "vkGetImageMemoryRequirements"));
    vkMapMemory =
        reinterpret_cast<PFN_vkMapMemory>(next(handle, "vkMapMemory"));
    vkQueueSubmit =
        reinterpret_cast<PFN_vkQueueSubmit>(next(handle, "vkQueueSubmit"));
    vkQueueSubmit2 =
        reinterpret_cast<PFN_vkQueueSubmit2>(next(handle, "vkQueueSubmit2"));
    vkUnmapMemory =
        reinterpret_cast<PFN_vkUnmapMemory>(next(handle, "vkUnmapMemory"));
    if (!vkCmdBeginRendering)
      vkCmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRendering>(
          next(handle, "vkCmdBeginRenderingKHR"));
    if (!vkCmdEndRendering)
      vkCmdEndRendering = reinterpret_cast<PFN_vkCmdEndRendering>(
          next(handle, "vkCmdEndRenderingKHR"));
    if (!vkCmdCopyImage2)
      vkCmdCopyImage2 = reinterpret_cast<PFN_vkCmdCopyImage2>(
          next(handle, "vkCmdCopyImage2KHR"));
    if (!vkCmdResolveImage2)
      vkCmdResolveImage2 = reinterpret_cast<PFN_vkCmdResolveImage2>(
          next(handle, "vkCmdResolveImage2KHR"));
    if (!vkQueueSubmit2)
      vkQueueSubmit2 = reinterpret_cast<PFN_vkQueueSubmit2>(
          next(handle, "vkQueueSubmit2KHR"));
  }
};
std::unordered_map<void *, Instance> instances;
std::unordered_map<void *, std::unique_ptr<Device>> devices;
Device *dev(const void *h) {
  auto i = devices.find(key(h));
  return i == devices.end() ? nullptr : i->second.get();
}
#define FN(d, n) (d)->n
bool colorFormat(VkFormat f) {
  switch (f) {
  case VK_FORMAT_R8G8B8A8_UNORM:
  case VK_FORMAT_R8G8B8A8_SRGB:
  case VK_FORMAT_B8G8R8A8_UNORM:
  case VK_FORMAT_B8G8R8A8_SRGB:
  case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
  case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
  case VK_FORMAT_R16G16B16A16_UNORM:
  case VK_FORMAT_R16G16B16A16_SFLOAT:
  case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
    return true;
  default:
    return false;
  }
}
// Bounded recording diagnostics; never read GPU buffers or change selection.
bool traceOccurrence(std::unordered_map<std::string, uint64_t> &patterns,
                     const std::string &key, size_t limit) {
  auto it = patterns.find(key);
  if (it == patterns.end()) {
    if (patterns.size() >= limit) return false;
    it = patterns.emplace(key, 0).first;
  }
  auto n = ++it->second;
  return n == 1 || n == 64 || n == 1024;
}
void traceTransfer(Device *d, const char *operation, VkImage src, VkImage dst,
                   uint32_t w, uint32_t h, VkOffset3D a, VkOffset3D b,
                   VkImageSubresourceLayers sl, VkImageSubresourceLayers dl) {
  if (!d->debug) return;
  auto x = d->images.find(src), y = d->images.find(dst);
  if (x == d->images.end() || y == d->images.end()) return;
  if (!x->second.eyeCount && !y->second.eyeCount && x->second.layers != 2 && y->second.layers != 2) return;
  std::ostringstream out;
  out << operation << " src=" << (void*)src << ' ' << x->second.w << 'x' << x->second.h
      << " layers=" << x->second.layers << " eye=" << x->second.eyeCount
      << " dst=" << (void*)dst << ' ' << y->second.w << 'x' << y->second.h
      << " layers=" << y->second.layers << " eye=" << y->second.eyeCount
      << " extent=" << w << 'x' << h << " srcOffset=" << a.x << ',' << a.y << ',' << a.z
      << " dstOffset=" << b.x << ',' << b.y << ',' << b.z
      << " srcMip/layer/count=" << sl.mipLevel << '/' << sl.baseArrayLayer << '/' << sl.layerCount
      << " dstMip/layer/count=" << dl.mipLevel << '/' << dl.baseArrayLayer << '/' << dl.layerCount;
  auto key = out.str();
  if (traceOccurrence(d->transferTracePatterns, key, 128)) LOG("eye transfer: %s", key.c_str());
}
void traceDraw(Device *d, Commands &c, const char *kind, uint32_t count, uint32_t instances) {
  if (!d->debug || c.traceSampleCount >= 6) return;
  ++c.traceSampleCount;
  std::ostringstream out;
  out << " [" << kind << " count=" << count << " instances=" << instances
      << " viewports=" << c.viewportCount;
  for (unsigned i = 0; i < std::min(c.viewportCount, 2u); ++i) {
    const auto &v = c.viewports[i];
    out << ' ' << v.x << ',' << v.y << ',' << v.width << ',' << v.height;
  }
  out << ']';
  c.traceSamples += out.str();
}
void beginEyeTrace(Device *d, Commands &c, const VkRenderingInfo *ci) {
  if (!d->debug || ci->colorAttachmentCount != 1 || !ci->pColorAttachments) return;
  const auto &a = ci->pColorAttachments[0];
  auto v = d->views.find(a.imageView);
  if (v == d->views.end()) return;
  auto i = d->images.find(v->second.image);
  if (i == d->images.end()) return;
  c.diagnosticTarget = i->first;
  std::ostringstream out;
  out << "target=" << (void*)i->first << ' ' << i->second.w << 'x' << i->second.h
      << " layers=" << i->second.layers << " mip=" << v->second.range.baseMipLevel
      << " baseLayer=" << v->second.range.baseArrayLayer << " viewLayers=" << v->second.range.layerCount
      << " area=" << ci->renderArea.offset.x << ',' << ci->renderArea.offset.y << ','
      << ci->renderArea.extent.width << ',' << ci->renderArea.extent.height
      << " flags=" << ci->flags << " mask=" << ci->viewMask
      << " load/store=" << a.loadOp << '/' << a.storeOp
      << " depth=" << bool(ci->pDepthAttachment && ci->pDepthAttachment->imageView)
      << " resolveMode=" << a.resolveMode << " resolveView=" << (void*)a.resolveImageView;
  c.tracePass = out.str();
  if (a.resolveImageView) {
    auto r = d->views.find(a.resolveImageView);
    if (r != d->views.end()) {
      VkImageSubresourceLayers src{v->second.range.aspectMask, v->second.range.baseMipLevel,
          v->second.range.baseArrayLayer, v->second.range.layerCount};
      VkImageSubresourceLayers dst{r->second.range.aspectMask, r->second.range.baseMipLevel,
          r->second.range.baseArrayLayer, r->second.range.layerCount};
      traceTransfer(d, "attachment-resolve", i->first, r->second.image,
          ci->renderArea.extent.width, ci->renderArea.extent.height,
          {ci->renderArea.offset.x, ci->renderArea.offset.y, 0},
          {ci->renderArea.offset.x, ci->renderArea.offset.y, 0}, src, dst);
    }
  }
}
void finishEyeTrace(Device *d, const Commands &c) {
  if (!d->debug || !c.diagnosticTarget) return;
  auto i = d->images.find(c.diagnosticTarget);
  if (i == d->images.end() || (!i->second.eyeCount && i->second.layers != 2)) return;
  std::ostringstream out;
  out << c.tracePass << " eyeConnected=" << bool(i->second.eyeCount)
      << " raw=" << c.diagnosticDraws << " indexed=" << c.diagnosticIndexed
      << " indirect=" << c.diagnosticIndirect << " small=" << c.diagnosticSmall
      << " directInvocations=" << c.diagnosticVertices
      << " qualified=" << c.draws << " children=" << c.traceChildCount
      << c.traceSamples << c.traceChildren;
  auto key = out.str();
  if (traceOccurrence(d->eyeTracePatterns, key, 128)) LOG("eye pass: %s", key.c_str());
}
bool sampledTracking(Device *d) { return d->mode == 2 || d->sampledTrace; }
void sampledDraw(Device *d, Commands &c, bool candidate) {
  if (!sampledTracking(d) || !candidate) return;
  if (c.sampledDraws.size() >= 8) { c.sampledOverflow = true; return; }
  SampledDraw draw;
  draw.complete = !c.descriptorOverflow;
  draw.viewports = c.viewports; draw.viewportCount = c.viewportCount;
  draw.scissors = c.scissors; draw.scissorCount = c.scissorCount;
  for (const auto &set : c.descriptorSets) {
    if (!set) continue;
    if (!set->valid) { draw.complete = false; continue; }
    draw.revisions.push_back({set, set->revision});
    for (const auto &entry : set->images) {
      if (draw.views.size() >= 32) { draw.complete = false; break; }
      if (std::find(draw.views.begin(), draw.views.end(), entry.second) == draw.views.end())
        draw.views.push_back(entry.second);
    }
  }
  c.sampledDraws.push_back(std::move(draw));
}
bool fullSampledView(const VkImageSubresourceRange &r, const Image &im) {
  auto layers = r.layerCount == VK_REMAINING_ARRAY_LAYERS ? im.layers - std::min(r.baseArrayLayer, im.layers) : r.layerCount;
  return r.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && r.baseMipLevel == 0 &&
      r.baseArrayLayer == 0 && layers == im.layers && (im.layers == 1 || im.layers == 2);
}
const char *sampledCoverageRejection(const SampledDraw &draw, const Image &im) {
  if (!draw.complete) return "incomplete-descriptors";
  if (!draw.viewportCount || draw.viewportCount > 2) return "viewport-count";
  if (draw.scissorCount != draw.viewportCount) return "scissor-count";
  for (uint32_t j = 0; j < draw.viewportCount; ++j) {
    auto v = draw.viewports[j]; auto r = draw.scissors[j];
    if (std::abs(v.x) > 1 || std::abs(std::min(v.y, v.y + v.height)) > 1 ||
        std::abs(v.width - im.w) > 1 || std::abs(std::abs(v.height) - im.h) > 1) return "partial-viewport";
    if (r.offset.x > 0 || r.offset.y > 0 ||
        int64_t(r.offset.x) + r.extent.width < im.w || int64_t(r.offset.y) + r.extent.height < im.h) return "partial-scissor";
  }
  for (const auto &r : draw.revisions) {
    if (!r.first->valid) return "invalidated-descriptor-set";
    if (r.first->revision != r.second) return "descriptor-updated-after-draw";
  }
  return nullptr;
}
void sampledGate(Device *d, const Commands &c, const char *reason) {
  if (!d->sampledTrace) return;
  auto im = d->images.find(c.sampledTarget);
  if (im == d->images.end() || !im->second.eyeCount) return;
  std::string key = std::to_string(im->second.w) + "x" + std::to_string(im->second.h) + ":" + reason;
  auto old = d->sampledGates.find(key);
  if (old != d->sampledGates.end()) { ++old->second; return; }
  if (d->sampledGates.size() >= 16) return;
  d->sampledGates.emplace(key,1);
  VkViewport viewport{}; VkRect2D scissor{}; unsigned vc = 0, sc = 0;
  if (!c.sampledDraws.empty()) {
    const auto &draw = c.sampledDraws[0]; vc = draw.viewportCount; sc = draw.scissorCount;
    viewport = draw.viewports[0]; scissor = draw.scissors[0];
  }
  LOG("sampled FFR gate: target=%ux%u reason=%s allowed=%d overflow=%d draws=%u snapshots=%zu eligible=%d format=%d "
      "viewportCount=%u viewport=%.1f,%.1f,%.1f,%.1f scissorCount=%u scissor=%d,%d,%u,%u links=%zu epoch=%llu",
      im->second.w, im->second.h, reason, c.sampledPassAllowed, c.sampledOverflow,
      c.sampledTotalDraws, c.sampledDraws.size(), im->second.eligible, int(im->second.format),
      vc, viewport.x, viewport.y, viewport.width, viewport.height, sc,
      scissor.offset.x, scissor.offset.y, scissor.extent.width, scissor.extent.height,
      d->sampledLinks.size(), (unsigned long long)d->sampledEpoch);
}
void learnSampled(Device *d, const Commands &c) {
  if (d->mode != 2 || !c.sampledTarget) return;
  // An observed incompatible writer revokes old inference immediately instead
  // of keeping it alive until the age limit. Changing the unique source resets warm-up.
  struct Revoke {
    Device *d; VkImage destination; bool keep = false;
    ~Revoke() { if (!keep) d->sampledLinks.erase(std::remove_if(d->sampledLinks.begin(), d->sampledLinks.end(),
        [&](const auto &l) { return l.destination == destination; }), d->sampledLinks.end()); }
  } revoke{d, c.sampledTarget};
  auto reject = [&](const char *reason) { sampledGate(d, c, reason); };
  if (!c.sampledPassAllowed) { reject("rendering-scope"); return; }
  if (c.sampledOverflow) { reject("unsupported-secondary-or-overflow"); return; }
  if (!c.sampledTotalDraws) { reject("empty-writer"); return; }
  if (c.sampledTotalDraws > 2 || c.sampledDraws.size() != c.sampledTotalDraws) { reject("mixed-or-nonquad-draws"); return; }
  auto dst = d->images.find(c.sampledTarget);
  if (dst == d->images.end()) return;
  if (!dst->second.eligible) { reject("output-ineligible"); return; }
  if (!fullSampledView(c.sampledRange, dst->second)) { reject("output-subresource"); return; }
  if (c.sampledArea.offset.x || c.sampledArea.offset.y || c.sampledArea.extent.width != dst->second.w ||
      c.sampledArea.extent.height != dst->second.h) { reject("output-render-area"); return; }
  VkImage source{};
  for (const auto &draw : c.sampledDraws) {
    if (auto reason = sampledCoverageRejection(draw, dst->second)) { reject(reason); return; }
    VkImage unique{};
    for (auto view : draw.views) {
      auto v = d->views.find(view); if (v == d->views.end()) { reject("expired-input-view"); return; }
      auto im = d->images.find(v->second.image); if (im == d->images.end()) { reject("expired-input-image"); return; }
      if (im->first == dst->first) { reject("feedback-input"); return; } // Feedback is not a one-way compositor.
      const auto &a = im->second; const auto &b = dst->second;
      if (!a.eligible || !(a.usage & VK_IMAGE_USAGE_SAMPLED_BIT) ||
          a.w != b.w || a.h != b.h || a.layers != b.layers || !fullSampledView(v->second.range, a)) continue;
      if (unique && unique != im->first) { reject("ambiguous-inputs"); return; } // History/motion/multiple scene inputs are ambiguous.
      unique = im->first;
    }
    if (!unique) { reject("no-matching-scene-input"); return; }
    if (source && source != unique) { reject("draws-use-different-inputs"); return; }
    source = unique;
  }
  revoke.keep = true;
  // Age and bound inferred links; only distinct eye-registration epochs count.
  d->sampledLinks.erase(std::remove_if(d->sampledLinks.begin(), d->sampledLinks.end(),
      [&](const auto &l) { return d->sampledEpoch - l.epoch > 240 || (l.destination == dst->first && l.source != source); }), d->sampledLinks.end());
  for (auto &l : d->sampledLinks) if (l.source == source && l.destination == dst->first) {
    if (l.epoch != d->sampledEpoch) l.observations = std::min(l.observations + 1, 8u);
    l.epoch = d->sampledEpoch;
    sampledGate(d, c, l.observations >= 8 ? "link-mature" : "link-warming"); return;
  }
  if (d->sampledLinks.size() < 256) { d->sampledLinks.push_back({source, dst->first, 1, d->sampledEpoch}); sampledGate(d,c,"link-created"); }
  else sampledGate(d,c,"link-capacity");
}
// Walk forward from a scene through proven transfers and mature inferred inputs.
// Keep this evidence out of the proven eyeCount graph used by Conservative.
bool sampledAncestry(Device *d, VkImage start, std::array<ffr::Rect, 2> &eyes, unsigned &count) {
  if (d->mode != 2) return false;
  struct Node { VkImage image; unsigned depth; bool sampled; };
  std::vector<Node> nodes{{start, 0, false}};
  for (size_t j = 0; j < nodes.size() && j < 64; ++j) {
    auto n = nodes[j]; auto im = d->images.find(n.image);
    if (im == d->images.end()) continue;
    if (n.sampled && im->second.eyeCount) { eyes = im->second.eyes; count = im->second.eyeCount; return true; }
    if (n.depth >= 6) continue;
    auto add = [&](VkImage dst, bool sampled) {
      auto to = d->images.find(dst);
      if (to == d->images.end() || to->second.w != im->second.w || to->second.h != im->second.h || to->second.layers != im->second.layers) return;
      for (auto old : nodes) if (old.image == dst && old.sampled == sampled) return;
      if (nodes.size() < 64) nodes.push_back({dst, n.depth + 1, sampled});
    };
    for (const auto &e : d->edges) if (e.source == n.image) add(e.destination, n.sampled);
    for (const auto &e : d->sampledLinks) if (e.source == n.image && e.observations >= 8 &&
        d->sampledEpoch - e.epoch <= 240) add(e.destination, true);
  }
  return false;
}
void reportSampled(Device *d, const Commands &parent, const SampledDraw &draw) {
  auto dst = d->images.find(parent.sampledTarget);
  if (dst == d->images.end() || !dst->second.eyeCount) return;
  // Bound descriptors are possible shader inputs, not proof that a shader reads
  // them. Never feed this relationship into production eye ancestry yet.
  std::ostringstream identity;
  identity << (void*)dst->first;
  std::ostringstream out;
  out << "eye=" << (void*)dst->first << ' ' << dst->second.w << 'x' << dst->second.h
      << " baseLayer=" << parent.sampledRange.baseArrayLayer << " viewLayers=" << parent.sampledRange.layerCount
      << " area=" << parent.sampledArea.offset.x << ',' << parent.sampledArea.offset.y << ','
      << parent.sampledArea.extent.width << ',' << parent.sampledArea.extent.height
      << " limited=" << !draw.complete << " boundSampledViews=" << draw.views.size();
  for (auto view : draw.views) {
    auto v = d->views.find(view);
    if (v == d->views.end()) { out << " {expired-view}"; continue; }
    auto im = d->images.find(v->second.image);
    if (im == d->images.end()) { out << " {expired-image}"; continue; }
    identity << ":" << (void*)im->first;
    out << " {input=" << (void*)im->first << ' ' << im->second.w << 'x' << im->second.h
        << " layers=" << im->second.layers << " eligible=" << im->second.eligible << " format=" << int(im->second.format) << " usage=" << im->second.usage << " mip=" << v->second.range.baseMipLevel
        << " layer=" << v->second.range.baseArrayLayer << " count=" << v->second.range.layerCount;
    unsigned ancestors = 0;
    for (const auto &edge : d->edges) {
      if (edge.destination != im->first || ancestors >= 4) continue;
      auto source = d->images.find(edge.source);
      if (source == d->images.end()) continue;
      identity << "/" << (void*)source->first;
      uint32_t draws = 0, vertices = 0;
      for (const auto &h : source->second.history) { draws = std::max(draws, h.draws); vertices = std::max(vertices, h.indexedVertices); }
      out << " upstream=" << (void*)source->first << ':' << source->second.w << 'x' << source->second.h
          << ":eligible=" << source->second.eligible << ":format=" << int(source->second.format) << ":draws=" << draws << ":indexInvocations=" << vertices;
      ++ancestors;
    }
    out << '}';
  }
  auto key = identity.str();
  // One report per distinct binding/scene summary, bounded for the whole device.
  if (!d->sampledReports.count(key) && d->sampledReports.size() < 24) {
    d->sampledReports.emplace(key, 1);
    LOG("sampled eye input (candidate, not proven shader use): %s", out.str().c_str());
  }
}
void propagate(Device *d) {
  for (auto &pair : d->images)
    if (!pair.second.registered) {
      pair.second.eyes = {};
      pair.second.eyeCount = 0;
    }
  // Bounded backward traversal, only exact full-size copy/resolve
  // relationships.
  for (unsigned depth = 0; depth < 4; depth++) {
    bool changed = false;
    for (auto e : d->edges) {
      auto a = d->images.find(e.source), b = d->images.find(e.destination);
      if (a == d->images.end() || b == d->images.end())
        continue;
      if (!a->second.eyeCount && b->second.eyeCount &&
          a->second.w == b->second.w && a->second.h == b->second.h &&
          a->second.layers == b->second.layers) {
        a->second.eyes = b->second.eyes;
        a->second.eyeCount = b->second.eyeCount;
        changed = true;
      }
    }
    if (!changed)
      break;
  }
}
void edge(Device *d, VkImage src, VkImage dst, uint32_t w, uint32_t h,
          VkOffset3D a, VkOffset3D b, uint32_t srcMip, uint32_t dstMip) {
  auto x = d->images.find(src), y = d->images.find(dst);
  if (x == d->images.end() || y == d->images.end() || a.x || a.y || a.z ||
      b.x || b.y || b.z || srcMip || dstMip)
    return;
  if (w != x->second.w || h != x->second.h || w != y->second.w ||
      h != y->second.h)
    return;
  for (auto e : d->edges)
    if (e.source == src && e.destination == dst)
      return;
  if (d->edges.size() < 2048)
    d->edges.push_back({src, dst});
  propagate(d);
}
uint32_t memoryType(Device *d, uint32_t bits, VkMemoryPropertyFlags flags) {
  for (uint32_t i = 0; i < d->memory.memoryTypeCount; i++)
    if ((bits & (1u << i)) &&
        (d->memory.memoryTypes[i].propertyFlags & flags) == flags)
      return i;
  return ~0u;
}
bool allocate(Device *d, VkMemoryRequirements r, VkMemoryPropertyFlags flags,
              VkDeviceMemory *out) {
  auto type = memoryType(d, r.memoryTypeBits, flags);
  if (type == ~0u)
    return false;
  VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  a.allocationSize = r.size;
  a.memoryTypeIndex = type;
  return FN(d, vkAllocateMemory)(d->handle, &a, nullptr, out) == VK_SUCCESS;
}
void destroyMap(Device *d, Map &m) {
  if (m.pool)
    FN(d, vkDestroyCommandPool)(d->handle, m.pool, nullptr);
  if (m.fence)
    FN(d, vkDestroyFence)(d->handle, m.fence, nullptr);
  if (m.view)
    FN(d, vkDestroyImageView)(d->handle, m.view, nullptr);
  if (m.image)
    FN(d, vkDestroyImage)(d->handle, m.image, nullptr);
  if (m.staging)
    FN(d, vkDestroyBuffer)(d->handle, m.staging, nullptr);
  if (m.memory)
    FN(d, vkFreeMemory)(d->handle, m.memory, nullptr);
  if (m.stagingMemory)
    FN(d, vkFreeMemory)(d->handle, m.stagingMemory, nullptr);
}
uint8_t mapDensity(const Map &m, float x, float y) {
  return ffr::density(x, y, m.eyes.data(), m.eyeCount);
}
bool buildMap(Device *d, Map &m) {
  // The maximum can be 1024 pixels on Pico, producing a nearly uniform 2x2
  // map. Use the finest permitted grid to preserve the peripheral pattern.
  uint32_t tx = std::max(1u, d->props.minFragmentDensityTexelSize.width);
  uint32_t ty = std::max(1u, d->props.minFragmentDensityTexelSize.height);
  uint32_t w = (m.w + tx - 1) / tx, h = (m.h + ty - 1) / ty;
  VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ci.imageType = VK_IMAGE_TYPE_2D;
  ci.format = VK_FORMAT_R8G8_UNORM;
  ci.extent = {w, h, 1};
  ci.mipLevels = 1;
  ci.arrayLayers = m.layers;
  ci.samples = VK_SAMPLE_COUNT_1_BIT;
  ci.tiling = VK_IMAGE_TILING_OPTIMAL;
  ci.usage = VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT |
             VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  auto createResult = FN(d, vkCreateImage)(d->handle, &ci, nullptr, &m.image);
  if (createResult != VK_SUCCESS || d->debug) LOG("density map create: target=%ux%u grid=%ux%u texel=%ux%u layers=%u result=%d",
      m.w, m.h, w, h, tx, ty, m.layers, createResult);
  if (createResult != VK_SUCCESS)
    return false;
  VkMemoryRequirements r;
  FN(d, vkGetImageMemoryRequirements)(d->handle, m.image, &r);
  if (!allocate(d, r, 0, &m.memory) ||
      FN(d, vkBindImageMemory)(d->handle, m.image, m.memory, 0) != VK_SUCCESS)
    return false;
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = m.image;
  vi.viewType =
      m.layers == 1 ? VK_IMAGE_VIEW_TYPE_2D : VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  vi.format = ci.format;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, m.layers};
  if (FN(d, vkCreateImageView)(d->handle, &vi, nullptr, &m.view) != VK_SUCCESS)
    return false;
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = VkDeviceSize(w) * h * 2 * m.layers;
  bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  if (FN(d, vkCreateBuffer)(d->handle, &bi, nullptr, &m.staging) != VK_SUCCESS)
    return false;
  FN(d, vkGetBufferMemoryRequirements)(d->handle, m.staging, &r);
  if (!allocate(d, r,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                &m.stagingMemory) ||
      FN(d, vkBindBufferMemory)(d->handle, m.staging, m.stagingMemory, 0) !=
          VK_SUCCESS)
    return false;
  void *bytes = nullptr;
  if (FN(d, vkMapMemory)(d->handle, m.stagingMemory, 0, bi.size, 0, &bytes) !=
      VK_SUCCESS)
    return false;
  auto p = static_cast<uint8_t *>(bytes);
  uint32_t reducedCells = 0;
  for (uint32_t y = 0; y < h; y++)
    for (uint32_t x = 0; x < w; x++) {
      // Keep a cell at the highest density needed by any of its corners/centre.
      uint8_t value = 0;
      for (float dy : {0.f, .5f, 1.f})
        for (float dx : {0.f, .5f, 1.f})
          value = std::max(
              value, mapDensity(m, std::min(float(m.w) - .5f, (x + dx) * tx),
                                  std::min(float(m.h) - .5f, (y + dy) * ty)));
      p[(y * w + x) * 2] = p[(y * w + x) * 2 + 1] = value;
      if (d->debug) reducedCells += value < 255;
    }
  DEBUG_LOG(d, "density map populated: reducedCells=%u totalCells=%u", reducedCells, w * h);
  if (m.layers == 2)
    std::memcpy(p + w * h * 2, p, w * h * 2);
  FN(d, vkUnmapMemory)(d->handle, m.stagingMemory);
  VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pc.queueFamilyIndex = d->family;
  if (FN(d, vkCreateCommandPool)(d->handle, &pc, nullptr, &m.pool) !=
      VK_SUCCESS)
    return false;
  VkCommandBufferAllocateInfo ac{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ac.commandPool = m.pool;
  ac.commandBufferCount = 1;
  ac.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  if (FN(d, vkAllocateCommandBuffers)(d->handle, &ac, &m.cmd) != VK_SUCCESS)
    return false;
  if (!d->setLoaderData || d->setLoaderData(d->handle, m.cmd) != VK_SUCCESS)
    return false;
  VkCommandBufferBeginInfo bc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  bc.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (FN(d, vkBeginCommandBuffer)(m.cmd, &bc) != VK_SUCCESS)
    return false;
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.image = m.image;
  barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex =
      VK_QUEUE_FAMILY_IGNORED;
  barrier.subresourceRange = vi.subresourceRange;
  barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  FN(d, vkCmdPipelineBarrier)
  (m.cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
   0, nullptr, 0, nullptr, 1, &barrier);
  VkBufferImageCopy copy{};
  copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, m.layers};
  copy.imageExtent = {w, h, 1};
  FN(d, vkCmdCopyBufferToImage)
  (m.cmd, m.staging, m.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
  barrier.oldLayout = barrier.newLayout;
  barrier.newLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
  barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT;
  FN(d, vkCmdPipelineBarrier)
  (m.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
   VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT, 0, 0, nullptr, 0,
   nullptr, 1, &barrier);
  if (FN(d, vkEndCommandBuffer)(m.cmd) != VK_SUCCESS)
    return false;
  VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  return FN(d, vkCreateFence)(d->handle, &fc, nullptr, &m.fence) == VK_SUCCESS;
}
Map *getMap(Device *d, const Image &image, const std::array<ffr::Rect, 2> &eyes,
            unsigned count, uint32_t layers) {
  for (auto &p : d->maps) {
    auto &m = *p;
    if (m.w == image.w && m.h == image.h && m.eyeCount == count &&
        m.layers == layers &&
        std::memcmp(m.eyes.data(), eyes.data(), sizeof(eyes)) == 0)
      return !m.failed && m.submitted &&
                     FN(d, vkGetFenceStatus)(d->handle, m.fence) == VK_SUCCESS
                 ? &m
                 : nullptr;
  }
  if (d->maps.size() >= 32)
    return nullptr; // bounded device-lifetime resources
  auto m = std::make_unique<Map>();
  m->w = image.w;
  m->h = image.h;
  m->eyes = eyes;
  m->eyeCount = count;
  m->layers = layers;
  if (!buildMap(d, *m)) {
    destroyMap(d, *m);
    d->enabled = false;
    LOG("density map allocation failed; foveation disabled");
    return nullptr;
  }
  d->maps.push_back(std::move(m));
  return nullptr; // initialized on the next queue submit; never wait on the
                  // game
}
std::unordered_map<VkQueue, uint32_t> queueFamilies;
void submitMaps(Device *d, VkQueue queue) {
  auto q = queueFamilies.find(queue);
  if (q == queueFamilies.end() || q->second != d->family)
    return;
  for (auto &p : d->maps)
    if (!p->submitted && !p->failed) {
      VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      si.commandBufferCount = 1;
      si.pCommandBuffers = &p->cmd;
      auto r = FN(d, vkQueueSubmit)(queue, 1, &si, p->fence);
      p->submitted = r == VK_SUCCESS;
      p->failed = r != VK_SUCCESS;
      if (r != VK_SUCCESS || d->debug)
        LOG("density map initialization %s result=%d",
            r == VK_SUCCESS ? "submitted" : "failed", r);
      if (r != VK_SUCCESS) d->enabled = false;
    }
}
bool sceneViewport(Device *d, const Commands &c) {
  auto it = d->images.find(c.color);
  if (it == d->images.end() || !c.viewportCount || c.viewportCount > 2)
    return false;
  auto &image = it->second;
  for (unsigned i = 0; i < c.viewportCount; i++) {
    auto v = c.viewports[i];
    float y = std::min(v.y, v.y + v.height), h = std::abs(v.height);
    auto matches = [&](ffr::Rect r) {
      return std::abs(v.x - r.x) <= 2 && std::abs(y - r.y) <= 2 &&
             std::abs(v.width - r.w) <= 2 && std::abs(h - r.h) <= 2;
    };
    if (matches({0, 0, image.w, image.h}))
      return true;
    for (unsigned j = 0; j < image.eyeCount; j++)
      if (matches(image.eyes[j]))
        return true;
  }
  return false;
}
// Secondary buffers are recorded before their target is known. Retain bounded
// viewport batches, then qualify geometry against the actual parent attachment.
void secondaryDraw(Commands &c, bool indexed, uint32_t draws = 1, uint32_t vertices = 0) {
  if (!c.inherited || !c.viewportCount || c.viewportCount > 2) return;
  for (auto &b : c.batches) {
    if (b.count == c.viewportCount && !memcmp(b.viewports.data(), c.viewports.data(), sizeof(c.viewports))) {
      b.draws = std::min(b.draws + draws, 1000u);
      b.indexed = std::min(b.indexed + (indexed ? draws : 0), 1000u);
      b.indexedVertices = std::min(b.indexedVertices + vertices, 1000000u);
      return;
    }
  }
  if (c.batches.size() < 128) c.batches.push_back({c.viewports, c.viewportCount, draws, indexed ? draws : 0, vertices});
}
// Diagnostic-only: inspect MRT passes even though production FFR excludes them.
// Descriptors are copied now; application attachment pointers need not outlive Begin.
void beginMrtDiagnostic(Device *d, Commands &c, const VkRenderingInfo *ci) {
  if (!d->debug || ci->colorAttachmentCount < 2 || !ci->pColorAttachments) return;
  c.mrtSlots = ci->colorAttachmentCount;
  c.mrtAligned = ci->colorAttachmentCount <= 8;
  c.mrtDepth = ci->pDepthAttachment && ci->pDepthAttachment->imageView;
  std::ostringstream out;
  out << "area=" << ci->renderArea.offset.x << ',' << ci->renderArea.offset.y << ' '
      << ci->renderArea.extent.width << 'x' << ci->renderArea.extent.height
      << " slots=" << ci->colorAttachmentCount << " layers=" << ci->layerCount
      << " viewMask=" << ci->viewMask << " flags=" << ci->flags << " pNext=" << (ci->pNext != nullptr);
  auto attachment = [&](const VkRenderingAttachmentInfo *a, const char *kind, unsigned slot) {
    out << ' ' << kind << slot << "={";
    if (!a || !a->imageView) { out << "unused}"; return false; }
    out << "layout=" << int(a->imageLayout) << ",load=" << int(a->loadOp)
        << ",store=" << int(a->storeOp) << ",resolve=" << bool(a->resolveImageView);
    auto v = d->views.find(a->imageView);
    if (v == d->views.end()) { out << ",untracked_view}"; return false; }
    auto i = d->images.find(v->second.image);
    if (i == d->images.end()) { out << ",untracked_image}"; return false; }
    const auto &im = i->second;
    auto mip = std::min(v->second.range.baseMipLevel, 31u);
    uint32_t w = std::max(1u, im.w >> mip), h = std::max(1u, im.h >> mip);
    auto format = v->second.format == VK_FORMAT_UNDEFINED ? im.format : v->second.format;
    out << ",format=" << int(format) << ",size=" << w << 'x' << h
        << ",imageLayers=" << im.layers << ",baseLayer=" << v->second.range.baseArrayLayer
        << ",viewLayers=" << v->second.range.layerCount << ",mip=" << mip
        << ",eyeConnected=" << bool(im.eyeCount) << '}';
    if (kind[0] == 'c') {
      c.mrtEyeMatch |= im.eyeCount != 0;
      for (const auto &entry : d->images) {
        const auto &eye = entry.second;
        if (eye.eyeCount && eye.w == w && eye.h == h && eye.layers == im.layers) c.mrtEyeMatch = true;
      }
    }
    return ci->renderArea.offset.x == 0 && ci->renderArea.offset.y == 0 &&
        w == ci->renderArea.extent.width && h == ci->renderArea.extent.height;
  };
  for (unsigned j = 0; j < std::min(ci->colorAttachmentCount, 8u); ++j) {
    const auto *a = &ci->pColorAttachments[j];
    bool aligned = attachment(a, "color", j);
    if (a->imageView) { ++c.mrtActive; c.mrtAligned &= aligned; }
  }
  bool depthAligned = attachment(ci->pDepthAttachment, "depth", 0);
  c.mrtAligned &= depthAligned;
  c.mrtDescription = out.str();
}
void finishMrtDiagnostic(Device *d, const Commands &c) {
  if (!d->debug || c.mrtSlots < 2) return;
  ++d->mrtPasses;
  // Multiple same-size outputs + depth + drawing are evidence, not proof of deferred rendering.
  bool candidate = c.mrtActive >= 2 && c.mrtDepth && c.mrtAligned && c.diagnosticDraws > 0;
  d->gbufferCandidates += candidate;
  std::string key = c.mrtDescription + (candidate ? " candidate" : " other") +
      (c.mrtEyeMatch ? " eye-size" : " unmatched");
  auto it = d->mrtPatterns.find(key);
  if (it == d->mrtPatterns.end()) {
    if (d->mrtPatterns.size() >= 32) return;
    it = d->mrtPatterns.emplace(key, 0).first;
  }
  auto occurrence = ++it->second;
  if (d->mrtReports >= 96 || (occurrence != 1 && occurrence != 8 && occurrence != 64)) return;
  ++d->mrtReports;
  LOG("MRT diagnostic: gbufferCandidate=%d activeColors=%u depth=%d aligned=%d eyeMatch=%d "
      "occurrence=%llu drawCalls=%llu indexedCalls=%llu indirectCalls=%llu directVertices=%llu %s",
      candidate, c.mrtActive, c.mrtDepth, c.mrtAligned, c.mrtEyeMatch,
      (unsigned long long)occurrence, (unsigned long long)c.diagnosticDraws,
      (unsigned long long)c.diagnosticIndexed, (unsigned long long)c.diagnosticIndirect,
      (unsigned long long)c.diagnosticVertices, c.mrtDescription.c_str());
}
void diagnosticSummary(Device *d) {
  LOG("MRT summary: passes=%llu gbufferCandidates=%llu patterns=%zu reports=%u (dynamic rendering only)",
      (unsigned long long)d->mrtPasses, (unsigned long long)d->gbufferCandidates,
      d->mrtPatterns.size(), d->mrtReports);
  LOG("pass summary: eyes=%llu dynamic=%llu legacy=%llu rawDraws=%llu "
      "indirectCalls=%llu qualifiedDraws=%llu viewportRejects=%llu maps=%zu enabled=%d",
      (unsigned long long)d->eyeCalls, (unsigned long long)d->dynamicCalls,
      (unsigned long long)d->legacyCalls, (unsigned long long)d->rawDraws,
      (unsigned long long)d->indirectCalls, (unsigned long long)d->qualifiedDraws, (unsigned long long)d->viewportRejects,
      d->maps.size(), d->enabled);
  for (const auto &r : d->rejections)
    LOG("pass rejection total: reason=%s count=%llu", r.first.c_str(),
        (unsigned long long)r.second);
}
void rejectedPass(Device *d, const char *reason, const VkRenderingInfo *ci,
                  const Commands &c, const Image *im = nullptr,
                  const ffr::Evidence *e = nullptr) {
  auto n = ++d->rejections[reason];
  if (d->diagnosticLines >= 80 || (n > 2 && n != 64 && n != 1024 && n != 16384)) return;
  ++d->diagnosticLines;
  LOG("pass rejected: reason=%s occurrence=%llu target=%ux%u layers=%u eligible=%d "
      "area=%d,%d %ux%u colors=%u depth=%d depthLayout=%d flags=0x%x "
      "viewMask=0x%x pNext=%d blocked=%d family=%u/%u "
      "connected=%d matchesEye=%d observations=%u draws=%u indexed=%u indexedVertices=%u viewportCount=%u",
      reason, (unsigned long long)n, im ? im->w : 0, im ? im->h : 0,
      im ? im->layers : 0, im && im->eligible,
      ci->renderArea.offset.x, ci->renderArea.offset.y,
      ci->renderArea.extent.width, ci->renderArea.extent.height,
      ci->colorAttachmentCount, c.depth,
      ci->pDepthAttachment ? int(ci->pDepthAttachment->imageLayout) : -1,
      ci->flags, ci->viewMask, ci->pNext != nullptr, c.blocked, c.family, d->family,
      e && e->connected, e && e->matchesEye, e ? e->observations : 0,
      e ? e->draws : 0, e ? e->indexed : 0, e ? e->indexedVertices : 0, c.viewportCount);
}
// Include attachment identity/subresources and all rendering-scope distinctions.
// Image generations distinguish reused Vulkan handles without depending on view
// handle stability (games can create equivalent views repeatedly).
std::string passKey(Device *d, const VkRenderingInfo &ci) {
  std::ostringstream out;
  out << ci.flags << ':' << ci.viewMask << ':' << ci.layerCount << ':'
      << ci.renderArea.offset.x << ':' << ci.renderArea.offset.y << ':'
      << ci.renderArea.extent.width << ':' << ci.renderArea.extent.height << ':' << bool(ci.pNext);
  auto view = [&](VkImageView handle) {
    auto v = d->views.find(handle);
    if (v == d->views.end()) { out << ":unknown:" << handle; return; }
    auto im = d->images.find(v->second.image);
    const auto &r = v->second.range;
    out << ':' << v->second.image << ':' << (im == d->images.end() ? 0 : im->second.identity)
        << ':' << v->second.format << ':' << r.aspectMask << ':' << r.baseMipLevel
        << ':' << r.levelCount << ':' << r.baseArrayLayer << ':' << r.layerCount;
  };
  auto attachment = [&](const VkRenderingAttachmentInfo *a) {
    out << ':' << bool(a);
    if (!a) return;
    view(a->imageView); view(a->resolveImageView);
    out << ':' << a->imageLayout << ':' << a->resolveImageLayout << ':' << a->resolveMode
        << ':' << a->loadOp << ':' << a->storeOp;
  };
  attachment(ci.pColorAttachments); attachment(ci.pDepthAttachment); attachment(ci.pStencilAttachment);
  return out.str();
}
void finish(Device *d, Commands &c) {
  if (c.rendering) learnSampled(d, c);
  if (d->sampledTrace && c.rendering) for (const auto &draw : c.sampledDraws) reportSampled(d, c, draw);
  c.sampledDraws.clear(); c.sampledOverflow = false; c.sampledTarget = {};
  c.sampledTotalDraws = 0; c.sampledPassAllowed = false;
  finishMrtDiagnostic(d, c);
  finishEyeTrace(d, c);
  if (c.rendering && c.color) {
    auto im = d->images.find(c.color);
    if (im != d->images.end()) {
      im->second.recurring.finish(c.passTicket, {0, c.draws, c.indexed, c.indexedVertices});
      // Per-target summaries survive loading screens; logarithmic then sparse
      // periodic reporting avoids a device-global quota hiding gameplay.
      if (d->sampledTrace && c.passTicket.index < 64) {
        auto &n = im->second.reports[c.passTicket.index]; ++n;
        if (n <= 4 || (n & (n - 1)) == 0 || n % 3600 == 0) {
          const auto &p = c.passTicket.prediction;
          LOG("pass prediction: image=%p target=%ux%u samples=%u frame=%llu writer=%zu trained=%u attached=%d predicted=%u/%u/%u actual=%u/%u/%u",
              (void *)c.color, im->second.w, im->second.h, unsigned(im->second.samples),
              (unsigned long long)c.passTicket.frame, c.passTicket.index, p.passes, c.fdmAttached,
              p.draws, p.indexed, p.indexedVertices, c.draws, c.indexed, c.indexedVertices);
        }
      }
    }
  }
  if (c.rendering && c.color && c.depth) {
    auto it = d->images.find(c.color);
    if (it != d->images.end()) {
      auto &history = it->second.history[c.signature];
      history.passes = std::min(history.passes + 1, 1000u);
      history.draws = c.draws;
      history.indexed = c.indexed;
      history.indexedVertices = c.indexedVertices;
    }
  }
  c.color = {};
  c.passTicket = {};
  c.fdmAttached = false;

  c.rendering = false;
  c.mrtSlots = c.mrtActive = 0;
  c.mrtAligned = c.mrtDepth = c.mrtEyeMatch = false;
  c.mrtDescription.clear();
  c.diagnosticTarget = {};
  c.tracePass.clear(); c.traceSamples.clear(); c.traceChildren.clear();
  c.traceSampleCount = c.traceChildCount = 0;
  c.diagnosticSmall = 0;
  c.diagnosticDraws = c.diagnosticIndexed = c.diagnosticIndirect = c.diagnosticVertices = 0;
  c.draws = c.indexed = c.indexedVertices = 0;
}
} // namespace
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance, const char *);
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice, const char *);
EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkCreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a,
                 VkInstance *out) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto link = (VkLayerInstanceCreateInfo *)ci->pNext;
  while (link &&
         (link->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
          link->function != VK_LAYER_LINK_INFO))
    link = (VkLayerInstanceCreateInfo *)link->pNext;
  if (!link)
    return VK_ERROR_INITIALIZATION_FAILED;
  auto gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
  link->u.pLayerInfo = link->u.pLayerInfo->pNext;
  auto create = (PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance");
  auto r = create(ci, a, out);
  if (r == VK_SUCCESS)
    instances[key(*out)] = {*out, gipa, nullptr};
  return r;
}
EXPORT VKAPI_ATTR void VKAPI_CALL
vkDestroyInstance(VkInstance i, const VkAllocationCallbacks *a) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto it = instances.find(key(i));
  if (it == instances.end())
    return;
  auto fn = (PFN_vkDestroyInstance)it->second.gipa(i, "vkDestroyInstance");
  instances.erase(it);
  fn(i, a);
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkCreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo *ci,
               const VkAllocationCallbacks *a, VkDevice *out) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto link = (VkLayerDeviceCreateInfo *)ci->pNext;
  PFN_vkSetDeviceLoaderData setData = nullptr;
  for (auto p = (VkLayerDeviceCreateInfo *)ci->pNext; p;
       p = (VkLayerDeviceCreateInfo *)p->pNext)
    if (p->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
        p->function == VK_LOADER_DATA_CALLBACK)
      setData = p->u.pfnSetDeviceLoaderData;
  while (link && (link->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
                  link->function != VK_LAYER_LINK_INFO))
    link = (VkLayerDeviceCreateInfo *)link->pNext;
  auto ii = instances.find(key(physical));
  if (!link || ii == instances.end())
    return VK_ERROR_INITIALIZATION_FAILED;
  auto inst = ii->second;
  auto next = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
  auto create =
      (PFN_vkCreateDevice)link->u.pLayerInfo->pfnNextGetInstanceProcAddr(
          inst.handle, "vkCreateDevice");
  link->u.pLayerInfo = link->u.pLayerInfo->pNext;
  auto d = std::make_unique<Device>();
  d->physical = physical;
  d->instance = inst;
  d->next = next;
  d->setLoaderData = setData;
  const char *debugEnv = getenv("GN_VR_FFR_DEBUG");
  d->sampledTrace = debugEnv && !strcmp(debugEnv, "1");
  // Retire the broad experimental log stream; DEBUG now requests focused tracing.
  d->debug = false;
  const char *mode = getenv("GN_VR_FFR");
  d->mode = mode ? atoi(mode) : 0;
  const char *engine = getenv("GN_VR_FFR_ENGINE");
  d->knownEngine =
      engine && (!strcmp(engine, "unity") || !strcmp(engine, "unreal"));
  auto features = (PFN_vkGetPhysicalDeviceFeatures2)inst.gipa(
      inst.handle, "vkGetPhysicalDeviceFeatures2");
  auto properties = (PFN_vkGetPhysicalDeviceProperties2)inst.gipa(
      inst.handle, "vkGetPhysicalDeviceProperties2");
  auto enumerate = (PFN_vkEnumerateDeviceExtensionProperties)inst.gipa(
      inst.handle, "vkEnumerateDeviceExtensionProperties");
  VkPhysicalDeviceFragmentDensityMapFeaturesEXT f{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT};
  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
  f2.pNext = &f;
  if (features)
    features(physical, &f2);
  uint32_t n = 0;
  auto extensionCountResult = enumerate(physical, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> ext(n);
  auto extensionListResult = enumerate(physical, nullptr, &n, ext.data());
  bool supported = false;
  for (auto e : ext)
    if (!strcmp(e.extensionName, VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME))
      supported = true;
  auto qprops = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)inst.gipa(
      inst.handle, "vkGetPhysicalDeviceQueueFamilyProperties");
  qprops(physical, &n, nullptr);
  std::vector<VkQueueFamilyProperties> qp(n);
  qprops(physical, &n, qp.data());
  unsigned graphicsQueues = 0;
  for (unsigned i = 0; i < ci->queueCreateInfoCount; i++) {
    auto q = ci->pQueueCreateInfos[i];
    if (q.queueFamilyIndex < n &&
        (qp[q.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
      graphicsQueues += q.queueCount;
      d->family = q.queueFamilyIndex;
    }
  }
  bool existing = false;
  VkBool32 existingFdm = VK_FALSE, existingRegular = VK_FALSE,
           existingDynamic = VK_FALSE;
  for (auto p = (const VkBaseInStructure *)ci->pNext; p; p = p->pNext)
    if (p->sType == f.sType) {
      existing = true;
      const auto *requested = reinterpret_cast<
          const VkPhysicalDeviceFragmentDensityMapFeaturesEXT *>(p);
      existingFdm = requested->fragmentDensityMap;
      existingRegular = requested->fragmentDensityMapNonSubsampledImages;
      existingDynamic = requested->fragmentDensityMapDynamic;
    }
  VkPhysicalDeviceProperties gpu{};
  auto getGpu = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
      inst.gipa(inst.handle, "vkGetPhysicalDeviceProperties"));
  if (getGpu) getGpu(physical, &gpu);
  bool wideSupported = gpu.apiVersion >= VK_API_VERSION_1_3;
  for (const auto &e : ext)
    wideSupported |= !strcmp(e.extensionName, VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME);
  const auto format = ffr::queryFormat(inst.gipa, inst.handle, physical, wideSupported);
  const auto &fp = format.legacy;
  const char *overrideEnv = getenv("GN_VR_FFR_ALLOW_MISSING_FORMAT_BIT");
  const bool overrideRequested = overrideEnv && !strcmp(overrideEnv, "1");
  const bool overrideUsed = overrideRequested && !format.supported() &&
                            format.experimentalFormatOverride();
  const bool formatAccepted = format.supported() || overrideUsed;
  DEBUG_LOG(d, "experimental format override: requested=%d used=%d imageSupported=%d "
      "(bypasses only missing density-map format bit)",
      overrideRequested, overrideUsed, format.imageSupported());

  DEBUG_LOG(d, "format queries: path=wrapper GPU=%s api=%u.%u.%u legacy=0x%x "
      "properties2=%d optimal2=0x%x properties3=%d optimal3=0x%llx "
      "imageQuery=%d imageResult=%d maxExtent=%ux%u maxLayers=%u samples=0x%x source=%s",
      gpu.deviceName, VK_VERSION_MAJOR(gpu.apiVersion), VK_VERSION_MINOR(gpu.apiVersion),
      VK_VERSION_PATCH(gpu.apiVersion), fp.optimalTilingFeatures,
      format.hasModern, format.modern.formatProperties.optimalTilingFeatures,
      format.hasWide, (unsigned long long)format.wide.optimalTilingFeatures,
      format.hasImage, format.imageResult, format.image.imageFormatProperties.maxExtent.width,
      format.image.imageFormatProperties.maxExtent.height,
      format.image.imageFormatProperties.maxArrayLayers,
      format.image.imageFormatProperties.sampleCounts, format.source);
  d->enabled = d->mode >= 1 && d->mode <= 2 && supported &&
               f.fragmentDensityMap &&
               f.fragmentDensityMapNonSubsampledImages && !existing &&
               graphicsQueues == 1 && setData && properties && formatAccepted;
  // Keep the actual queried values: f is overwritten below to form the
  // requested feature chain. Diagnostics must not report those requested values
  // as device capabilities. Each failed gate is reported, not just the first.
  std::string activationReasons;
  auto failedGate = [&](bool passed, const char *reason) {
    if (!passed) {
      if (!activationReasons.empty())
        activationReasons += ",";
      activationReasons += reason;
    }
  };
  failedGate(d->mode >= 1 && d->mode <= 2, "mode_off_or_invalid");
  failedGate(supported, "fdm_extension_missing");
  failedGate(f.fragmentDensityMap, "fragmentDensityMap_false");
  failedGate(f.fragmentDensityMapNonSubsampledImages,
             "regular_images_unsupported");
  failedGate(!existing, "existing_fdm_feature_structure");
  failedGate(graphicsQueues == 1, "graphics_queue_count_not_one");
  failedGate(setData != nullptr, "loader_data_callback_missing");
  failedGate(properties != nullptr, "properties2_entrypoint_missing");
  failedGate(formatAccepted, "r8g8_density_map_configuration_unsupported");
  failedGate(format.hasImage, "image_format_query_missing");
  failedGate(!format.hasImage || format.imageResult == VK_SUCCESS,
             "density_map_image_usage_rejected");
  DEBUG_LOG(d, "activation checks: mode=%d extension=%d feature=%u regularImages=%u "
      "dynamic=%u "
      "features2=%d properties2=%d loaderData=%d existingFeature=%d "
      "graphicsQueues=%u "
      "formatOptimal=0x%x formatLinear=0x%x formatFDM=%d formatTransferDst=%d "
      "extensionQueryResults=%d/%d preflight=%d reasons=%s",
      d->mode, supported, f.fragmentDensityMap,
      f.fragmentDensityMapNonSubsampledImages, f.fragmentDensityMapDynamic,
      features != nullptr, properties != nullptr, setData != nullptr, existing,
      graphicsQueues, fp.optimalTilingFeatures, fp.linearTilingFeatures,
      (fp.optimalTilingFeatures &
       VK_FORMAT_FEATURE_FRAGMENT_DENSITY_MAP_BIT_EXT) != 0,
      (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_TRANSFER_DST_BIT) != 0,
      extensionCountResult, extensionListResult, d->enabled,
      activationReasons.empty() ? "none" : activationReasons.c_str());
  if (existing)
    DEBUG_LOG(d, "existing FDM request: feature=%u regularImages=%u dynamic=%u",
        existingFdm, existingRegular, existingDynamic);
  if (!features || !properties)
    DEBUG_LOG(d, "query aliases: features2KHR=%d properties2KHR=%d (diagnostic only)",
        inst.gipa(inst.handle, "vkGetPhysicalDeviceFeatures2KHR") != nullptr,
        inst.gipa(inst.handle, "vkGetPhysicalDeviceProperties2KHR") != nullptr);
  bool regularSupport = f.fragmentDensityMapNonSubsampledImages;
  VkPhysicalDeviceProperties2 p2{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  p2.pNext = &d->props;
  if (properties)
    properties(physical, &p2);
  auto mem = (PFN_vkGetPhysicalDeviceMemoryProperties)inst.gipa(
      inst.handle, "vkGetPhysicalDeviceMemoryProperties");
  mem(physical, &d->memory);
  std::vector<const char *> names;
  for (unsigned i = 0; i < ci->enabledExtensionCount; i++)
    names.push_back(ci->ppEnabledExtensionNames[i]);
  if (d->enabled && std::none_of(names.begin(), names.end(), [](auto s) {
        return !strcmp(s, VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME);
      }))
    names.push_back(VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME);
  auto modified = *ci;
  f.fragmentDensityMap = VK_TRUE;
  f.fragmentDensityMapNonSubsampledImages = VK_TRUE;
  f.fragmentDensityMapDynamic = VK_FALSE;
  f.pNext = (void *)ci->pNext;
  if (d->enabled) {
    modified.pNext = &f;
    modified.enabledExtensionCount = names.size();
    modified.ppEnabledExtensionNames = names.data();
  }
  DEBUG_LOG(d, "density-map limits: minTexel=%ux%u maxTexel=%ux%u",
      d->props.minFragmentDensityTexelSize.width,
      d->props.minFragmentDensityTexelSize.height,
      d->props.maxFragmentDensityTexelSize.width,
      d->props.maxFragmentDensityTexelSize.height);
  const bool requestedFoveation = d->enabled;
  auto remainingLinks = link->u.pLayerInfo;
  auto r = create(physical, &modified, a, out);
  DEBUG_LOG(d, "vkCreateDevice: requestedFoveation=%d result=%d", requestedFoveation, r);
  // A wrapper may enumerate the feature but reject enabling it. Fail open.
  if (r != VK_SUCCESS && d->enabled) {
    LOG("device rejected FDM (%d); retrying without foveation", r);
    d->enabled = false;
    failedGate(false, "device_rejected_fdm");
    link->u.pLayerInfo = remainingLinks;
    r = create(physical, ci, a, out);
    LOG("vkCreateDevice fallback: result=%d", r);
  }
  if (r == VK_SUCCESS) {
    d->handle = *out;
    d->loadFunctions();
    failedGate(d->vkCmdBeginRendering != nullptr,
               "begin_rendering_entrypoint_missing");
    failedGate(d->vkCmdEndRendering != nullptr,
               "end_rendering_entrypoint_missing");
    if (!d->vkCmdBeginRendering || !d->vkCmdEndRendering)
      d->enabled = false;
    DEBUG_LOG(d, "dynamic rendering entrypoints: beginCore=%d beginKHR=%d endCore=%d "
        "endKHR=%d resolvedBegin=%d resolvedEnd=%d",
        next(*out, "vkCmdBeginRendering") != nullptr,
        next(*out, "vkCmdBeginRenderingKHR") != nullptr,
        next(*out, "vkCmdEndRendering") != nullptr,
        next(*out, "vkCmdEndRenderingKHR") != nullptr,
        d->vkCmdBeginRendering != nullptr, d->vkCmdEndRendering != nullptr);
    LOG("device mode=%d enabled=%d engine=%s FDM=%d regularImages=%d "
        "graphicsQueues=%u reasons=%s",
        d->mode, d->enabled, engine ? engine : "generic", supported,
        regularSupport, graphicsQueues,
        activationReasons.empty() ? "none" : activationReasons.c_str());
    devices[key(*out)] = std::move(d);
  }
  return r;
}
EXPORT VKAPI_ATTR void VKAPI_CALL
vkDestroyDevice(VkDevice h, const VkAllocationCallbacks *a) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  if (!d)
    return;
  for (auto &m : d->maps)
    destroyMap(d, *m);
  auto fn = FN(d, vkDestroyDevice);
  for (auto i = queueFamilies.begin(); i != queueFamilies.end();)
    if (key(i->first) == key(h))
      i = queueFamilies.erase(i);
    else
      ++i;
  devices.erase(key(h));
  fn(h, a);
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImage(VkDevice h, const VkImageCreateInfo *ci,
              const VkAllocationCallbacks *a, VkImage *out) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  auto r = FN(d, vkCreateImage)(h, ci, a, out);
  if (r == VK_SUCCESS && d->enabled) {
    Image im;
    im.identity = ++d->imageIdentity;
    im.samples = ci->samples;
    im.w = ci->extent.width;
    im.h = ci->extent.height;
    im.layers = ci->arrayLayers;
    im.mips = ci->mipLevels;
    im.usage = ci->usage;
    im.format = ci->format;
    im.eligible = ci->imageType == VK_IMAGE_TYPE_2D &&
                  (ci->arrayLayers == 1 || ci->arrayLayers == 2) &&
                  ci->mipLevels == 1 && im.w >= 256 && im.h >= 256 &&
                  !(ci->flags & (VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT |
                                 VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT)) &&
                  (ci->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) &&
                  colorFormat(ci->format);
    d->images[*out] = im;
  }
  return r;
}
EXPORT VKAPI_ATTR void VKAPI_CALL
vkDestroyImage(VkDevice h, VkImage image, const VkAllocationCallbacks *a) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  d->images.erase(image);
  d->sampledLinks.erase(std::remove_if(d->sampledLinks.begin(), d->sampledLinks.end(),
      [&](const auto &l) { return l.source == image || l.destination == image; }), d->sampledLinks.end());
  d->edges.erase(std::remove_if(d->edges.begin(), d->edges.end(),
                                [&](auto e) {
                                  return e.source == image ||
                                         e.destination == image;
                                }),
                 d->edges.end());
  propagate(d);
  FN(d, vkDestroyImage)(h, image, a);
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkCreateImageView(VkDevice h, const VkImageViewCreateInfo *ci,
                  const VkAllocationCallbacks *a, VkImageView *out) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  auto r = FN(d, vkCreateImageView)(h, ci, a, out);
  if (r == VK_SUCCESS && d->enabled)
    d->views[*out] = {ci->image, ci->subresourceRange, ci->format};
  return r;
}
EXPORT VKAPI_ATTR void VKAPI_CALL
vkDestroyImageView(VkDevice h, VkImageView v, const VkAllocationCallbacks *a) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  d->views.erase(v);
  FN(d, vkDestroyImageView)(h, v, a);
}
// Called by our guest OpenXR bridge for actual submitted projection views.
EXPORT void gnFfrRegisterEye(VkDevice h, VkImage image, uint32_t eye,
                             uint32_t x, uint32_t y, uint32_t w,
                             uint32_t height) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  if (!d || !d->enabled || eye > 1)
    return;
  if (d->debug) {
    ++d->eyeCalls;
    if (d->eyeCalls == 1 || d->eyeCalls == 240 || d->eyeCalls == 2400 ||
        d->eyeCalls == 12000 || d->eyeCalls == 60000) diagnosticSummary(d);
  }
  auto it = d->images.find(image);
  if (it == d->images.end())
    return;
  auto &im = it->second;
  if (!w || !height || x > im.w || y > im.h || w > im.w - x ||
      height > im.h - y)
    return;
  ++d->sampledEpoch;
  if (eye == 0) ++d->recordingFrame;
  // One rectangle for separate-eye textures; two for horizontally packed eyes.
  unsigned slot = (x || y) ? 1 : 0;
  ffr::Rect rect{x, y, w, height};
  if (im.registered && std::memcmp(&im.eyes[slot], &rect, sizeof(rect)) == 0)
    return;
  im.registered = true;
  im.eyes[slot] = rect;
  im.eyeCount = std::max(im.eyeCount, slot + 1);
  if (d->debug && d->messages < 16)
    LOG("registered eye %u image=%p rect=%u,%u %ux%u", eye, (void *)image, x, y,
        w, height);
  propagate(d);
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkCreateCommandPool(VkDevice h, const VkCommandPoolCreateInfo *ci,
                    const VkAllocationCallbacks *a, VkCommandPool *out) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  auto r = FN(d, vkCreateCommandPool)(h, ci, a, out);
  if (r == VK_SUCCESS)
    d->pools[*out] = ci->queueFamilyIndex;
  return r;
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(
    VkDevice h, VkCommandPool pool, const VkAllocationCallbacks *a) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  for (auto i = d->commands.begin(); i != d->commands.end();)
    if (i->second.pool == pool)
      i = d->commands.erase(i);
    else
      ++i;
  d->pools.erase(pool);
  FN(d, vkDestroyCommandPool)(h, pool, a);
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(
    VkDevice h, const VkCommandBufferAllocateInfo *ci, VkCommandBuffer *out) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  auto r = FN(d, vkAllocateCommandBuffers)(h, ci, out);
  if (r == VK_SUCCESS)
    for (unsigned i = 0; i < ci->commandBufferCount; i++) {
      Commands c;
      c.pool = ci->commandPool;
      c.family = d->pools[ci->commandPool];
      c.blocked = ci->level != VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      d->commands[out[i]] = c;
    }
  return r;
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(
    VkDevice h, VkCommandPool pool, uint32_t n, const VkCommandBuffer *c) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  for (unsigned i = 0; i < n; i++)
    d->commands.erase(c[i]);
  FN(d, vkFreeCommandBuffers)(h, pool, n, c);
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkBeginCommandBuffer(VkCommandBuffer h, const VkCommandBufferBeginInfo *ci) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  auto &c = d->commands[h];
  auto pool = c.pool;
  auto fam = c.family;
  auto blocked = c.blocked;
  c = {};
  c.pool = pool;
  c.family = fam;
  c.blocked = blocked;
  if (d->debug) {
    std::ostringstream out;
    out << "secondary=" << blocked << " beginFlags=" << ci->flags
        << " inheritance=" << bool(ci->pInheritanceInfo);
    if (ci->pInheritanceInfo) out << " renderPass=" << (void*)ci->pInheritanceInfo->renderPass;
    c.traceInheritance = out.str();
  }
  if (blocked && (ci->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) &&
      ci->pInheritanceInfo && !ci->pInheritanceInfo->renderPass) {
    for (auto p = static_cast<const VkBaseInStructure *>(ci->pInheritanceInfo->pNext); p; p = p->pNext) {
      if (p->sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO) {
        auto inherited = reinterpret_cast<const VkCommandBufferInheritanceRenderingInfo *>(p);
        if (d->debug) c.traceInheritance += " colors=" + std::to_string(inherited->colorAttachmentCount) +
            " renderingFlags=" + std::to_string(inherited->flags) + " mask=" + std::to_string(inherited->viewMask);
        c.inherited = inherited->colorAttachmentCount == 1 && inherited->viewMask <= 3 &&
                      !(inherited->flags & ~VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT);
        c.diagnosticInheritance = d->debug && inherited->colorAttachmentCount >= 2 &&
            !(inherited->flags & ~VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT);
        c.viewMask = inherited->viewMask;
      }
    }
  }
  auto result = FN(d, vkBeginCommandBuffer)(h, ci);
  if (result != VK_SUCCESS) { c.inherited = false; c.diagnosticInheritance = false; }
  return result;
}
// Observe older render-pass paths without changing their commands or state.
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(
    VkCommandBuffer h, const VkRenderPassBeginInfo *ci, VkSubpassContents contents) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  if (d->debug && ++d->legacyCalls == 1) LOG("legacy render pass observed: FFR only handles dynamic rendering");
  auto fn = reinterpret_cast<PFN_vkCmdBeginRenderPass>(d->next(d->handle, "vkCmdBeginRenderPass"));
  fn(h, ci, contents);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass2(
    VkCommandBuffer h, const VkRenderPassBeginInfo *ci, const VkSubpassBeginInfo *begin) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  if (d->debug && ++d->legacyCalls == 1) LOG("legacy render pass2 observed: FFR only handles dynamic rendering");
  auto fn = reinterpret_cast<PFN_vkCmdBeginRenderPass2>(d->next(d->handle, "vkCmdBeginRenderPass2"));
  if (!fn) fn = reinterpret_cast<PFN_vkCmdBeginRenderPass2>(d->next(d->handle, "vkCmdBeginRenderPass2KHR"));
  fn(h, ci, begin);
}
EXPORT VKAPI_ATTR void VKAPI_CALL
vkCmdBeginRendering(VkCommandBuffer h, const VkRenderingInfo *ci) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  auto &c = d->commands[h];
  if (d->debug) ++d->dynamicCalls;
  finish(d, c);
  c.rendering = true;
  c.depth = false;
  c.signature = 0;
  c.viewMask = ci->viewMask;
  c.secondaryContents = (ci->flags & VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT) != 0;
  const auto unsupportedFlags = ci->flags & ~VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT;
  VkRenderingInfo info = *ci;
  VkRenderingFragmentDensityMapAttachmentInfoEXT fdm{
      VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_INFO_EXT};
  // Extension-specific rendering state (local reads, shading-rate attachments,
  // device-group regions, etc.) requires explicit compatibility work.
  bool hasFdm = ci->pNext != nullptr;
  beginMrtDiagnostic(d, c, ci);
  beginEyeTrace(d, c, ci);
  if (sampledTracking(d) && ci->colorAttachmentCount == 1 && ci->pColorAttachments) {
    c.sampledPassAllowed = ci->pColorAttachments[0].storeOp == VK_ATTACHMENT_STORE_OP_STORE && !ci->pNext && !unsupportedFlags && ci->viewMask <= 3 && ci->layerCount <= 2 && !c.blocked;
    auto v = d->views.find(ci->pColorAttachments[0].imageView);
    if (v != d->views.end()) { c.sampledTarget = v->second.image; c.sampledRange = v->second.range; c.sampledArea = ci->renderArea; }
  }
  const char *rejection = "untracked_color_view";
  const Image *candidate = nullptr;
  ffr::Evidence observed{};
  if (!d->enabled) rejection = "disabled";
  else if (c.blocked) rejection = "blocked_command_buffer";
  else if (c.family != d->family) rejection = "queue_family";
  else if (unsupportedFlags) rejection = "rendering_flags";
  else if (ci->viewMask > 3 || ci->layerCount > 2) rejection = "view_layout";
  else if (ci->colorAttachmentCount != 1 || !ci->pColorAttachments) rejection = "color_attachment_count";

  if (d->enabled && !c.blocked && c.family == d->family && !unsupportedFlags &&
      ci->viewMask <= 3 && ci->layerCount <= 2 &&
      ci->colorAttachmentCount == 1 && ci->pColorAttachments) {
    auto vi = d->views.find(ci->pColorAttachments[0].imageView);
    if (vi != d->views.end() && vi->second.range.baseMipLevel) rejection = "nonzero_mip";
    if (vi != d->views.end() && !vi->second.range.baseMipLevel) {
      rejection = "untracked_color_image";
      auto ii = d->images.find(vi->second.image);
      if (ii != d->images.end()) {
        auto &im = ii->second;
        candidate = &im;
        c.color = ii->first;
        c.depth = ci->pDepthAttachment && ci->pDepthAttachment->imageView;
        if (c.depth) {
          auto layout = ci->pDepthAttachment->imageLayout;
          c.depth =
              layout == VK_IMAGE_LAYOUT_GENERAL ||
              layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL ||
              layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL ||
              layout == VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL ||
              layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL ||
              layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL ||
              layout == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL ||
              layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL ||
              layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL;
          const bool readOnly = layout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL ||
              layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL ||
              layout == VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL ||
              layout == VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL;
          c.signature = (uint32_t(ci->pColorAttachments[0].loadOp) & 3u) |
                        ((uint32_t(ci->pDepthAttachment->loadOp) & 3u) << 2) |
                        (readOnly ? 16u : 0u) | (c.secondaryContents ? 32u : 0u);
        }
        c.passTicket = im.recurring.begin(d->recordingFrame, passKey(d, *ci));
        const auto &history = c.passTicket.prediction;
        // Learn implicit dynamic-rendering MSAA resolves too.
        auto rv = d->views.find(ci->pColorAttachments[0].resolveImageView);
        if (rv != d->views.end())
          edge(d, ii->first, rv->second.image, im.w, im.h, {}, {}, 0, 0);
        auto eyes = im.eyes;
        unsigned count = im.eyeCount;
        bool matches = false;
        if (!count)
          for (const auto &other : d->images) {
            auto &e = other.second;
            if (e.eyeCount && e.w == im.w && e.h == im.h &&
                e.layers == im.layers) {
              eyes = e.eyes;
              count = e.eyeCount;
              matches = true;
              break;
            }
          }
        bool sampledConnected = false;
        if (!im.eyeCount && (d->sampledTrace || (im.eligible && c.depth && history.passes >= 8 && history.indexed >= 2))) sampledConnected = sampledAncestry(d, ii->first, eyes, count);
        ffr::Evidence evidence{im.eligible && ci->renderArea.offset.x == 0 &&
                                   ci->renderArea.offset.y == 0 &&
                                   ci->renderArea.extent.width == im.w &&
                                   ci->renderArea.extent.height == im.h,
                               c.depth,
                               im.eyeCount != 0,
                               matches,
                               history.passes,
                               history.draws,
                               history.indexed,
                               d->knownEngine,
                               history.indexedVertices, sampledConnected};
        observed = evidence;
        if (hasFdm) rejection = "rendering_pnext";
        else if (!evidence.eligible) rejection = "image_or_render_area";
        else if (!evidence.depth) rejection = "missing_or_unsupported_depth";
        else if (!evidence.connected && !evidence.matchesEye && !evidence.sampledConnection) rejection = "no_eye_match";
        else if (evidence.observations < 3) rejection = "warmup";
        else if (evidence.draws < 8 || evidence.indexed < 4) rejection = "geometry_threshold";
        else rejection = "aggressive_threshold_or_mode";
        const bool selected = ffr::select(evidence, d->mode);
        if (d->sampledTrace && sampledConnected && (!selected || hasFdm)) {
          std::string key = "scene:" + std::to_string(im.w) + "x" + std::to_string(im.h) + ":" +
              std::to_string(evidence.eligible) + std::to_string(evidence.depth) +
              std::to_string(history.passes >= 8) + std::to_string(hasFdm);
          if (!d->sampledGates.count(key) && d->sampledGates.size() < 16) {
            d->sampledGates.emplace(key,1);
            LOG("sampled scene gate: target=%ux%u eligible=%d depth=%d observations=%u draws=%u indexed=%u indexInvocations=%u pNext=%d",
                im.w, im.h, evidence.eligible, evidence.depth, history.passes, history.draws, history.indexed,
                history.indexedVertices, hasFdm);
          }
        }
        if (!hasFdm && selected && count) {
          rejection = "map_not_ready";
          auto map = getMap(d, im, eyes, count, ci->viewMask >= 2 ? 2u : 1u);
          if (map) {
            rejection = nullptr;
            c.fdmAttached = true;
            if (sampledConnected && d->sampledSelections++ < 3)
              LOG("sampled-input FFR applied: target=%ux%u layers=%u draws=%u indexed=%u indexInvocations=%u",
                  im.w, im.h, im.layers, history.draws, history.indexed, history.indexedVertices);
            fdm.pNext = info.pNext;
            fdm.imageView = map->view;
            fdm.imageLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
            info.pNext = &fdm;
            if (d->messages < (d->debug ? 16u : 1u) && ++d->messages)
              LOG("selected %ux%u mode=%d evidence=%s draws=%u indexed=%u",
                  im.w, im.h, d->mode,
                  im.eyeCount ? "eye ancestry" : sampledConnected ? "sampled-input ancestry" : "scene heuristic",
                  history.draws, history.indexed);
          }
        }
      }
    }
  }
  if (d->debug && rejection && d->mode) rejectedPass(d, rejection, ci, c, candidate, &observed);
  FN(d, vkCmdBeginRendering)(h, &info);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer h) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  finish(d, d->commands[h]);
  FN(d, vkCmdEndRendering)(h);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer h,
                                                   uint32_t first,
                                                   uint32_t count,
                                                   const VkViewport *views) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  auto &c = d->commands[h];
  c.viewportCount =
      first == 0 ? count : std::max(c.viewportCount, first + count);
  for (uint32_t i = 0; i < count && first + i < 2; i++)
    c.viewports[first + i] = views[i];
  FN(d, vkCmdSetViewport)(h, first, count, views);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdSetViewportWithCount(
    VkCommandBuffer h, uint32_t count, const VkViewport *views) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  auto &c = d->commands[h];
  c.viewportCount = count;
  for (uint32_t i = 0; i < count && i < 2; i++)
    c.viewports[i] = views[i];
  FN(d, vkCmdSetViewportWithCount)(h, count, views);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer h, uint32_t first, uint32_t count, const VkRect2D *rects) {
  std::lock_guard<std::recursive_mutex> guard(lock); auto d = dev(h); auto &c = d->commands[h];
  c.scissorCount = first == 0 ? count : std::max(c.scissorCount, first + count);
  for (unsigned j = 0; j < count && uint64_t(first) + j < c.scissors.size(); ++j) c.scissors[first+j] = rects[j];
  reinterpret_cast<PFN_vkCmdSetScissor>(d->next(d->handle, "vkCmdSetScissor"))(h, first, count, rects);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdSetScissorWithCount(VkCommandBuffer h, uint32_t count, const VkRect2D *rects) {
  std::lock_guard<std::recursive_mutex> guard(lock); auto d = dev(h); auto &c = d->commands[h];
  c.scissorCount = count;
  for (unsigned j = 0; j < std::min(count, 2u); ++j) c.scissors[j] = rects[j];
  auto fn = reinterpret_cast<PFN_vkCmdSetScissorWithCount>(d->next(d->handle, "vkCmdSetScissorWithCount"));
  if (!fn) fn = reinterpret_cast<PFN_vkCmdSetScissorWithCount>(d->next(d->handle, "vkCmdSetScissorWithCountEXT"));
  fn(h, count, rects);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer h, uint32_t n,
                                            uint32_t instances_, uint32_t first,
                                            uint32_t instance) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  auto &c = d->commands[h];
  if (d->debug) ++d->rawDraws;
  if (d->debug && n && instances_) {
    ++c.diagnosticDraws;
    c.diagnosticVertices += uint64_t(n) * instances_;
  }
  if (sampledTracking(d) && n && instances_) c.sampledTotalDraws = std::min(c.sampledTotalDraws + 1, 1000u);
  sampledDraw(d, c, (n == 3 || n == 4) && instances_ > 0 && instances_ <= 2);
  traceDraw(d, c, "draw", n, instances_);
  if (d->debug && n && instances_ && n <= 3) ++c.diagnosticSmall;
  if (n > 3 && instances_) secondaryDraw(c, false);
  if (c.rendering && n > 3 && instances_) {
    if (sceneViewport(d, c)) { c.draws++; if (d->debug) d->qualifiedDraws++; }
    else if (d->debug) d->viewportRejects++;
  }
  FN(d, vkCmdDraw)(h, n, instances_, first, instance);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer h,
                                                   uint32_t n, uint32_t count,
                                                   uint32_t first,
                                                   int32_t offset,
                                                   uint32_t instance) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  auto &c = d->commands[h];
  if (d->debug) ++d->rawDraws;
  if (d->debug && n && count) {
    ++c.diagnosticDraws; ++c.diagnosticIndexed;
    c.diagnosticVertices += uint64_t(n) * count;
  }
  if (sampledTracking(d) && n && count) c.sampledTotalDraws = std::min(c.sampledTotalDraws + 1, 1000u);
  sampledDraw(d, c, (n == 3 || n == 6) && count > 0 && count <= 2);
  traceDraw(d, c, "indexed", n, count);
  if (d->debug && n && count && n <= 6) ++c.diagnosticSmall;
  const uint32_t vertices = uint32_t(std::min(uint64_t(n) * count, uint64_t(1000000)));
  if (n > 6 && count) secondaryDraw(c, true, 1, vertices);
  if (c.rendering && n > 6 && count) {
    if (sceneViewport(d, c)) { c.draws++; c.indexed++; c.indexedVertices = std::min(c.indexedVertices + vertices, 1000000u); if (d->debug) d->qualifiedDraws++; }
    else if (d->debug) d->viewportRejects++;
  }
  FN(d, vkCmdDrawIndexed)(h, n, count, first, offset, instance);
}
// GPU indirect buffers are never mapped or read back. For count-buffer draws,
// maxDrawCount is only an upper bound: count one submission, not that maximum.
static void indirectDraw(Device *d, Commands &c, uint32_t count, bool indexed) {
  if (d->debug) { ++d->rawDraws; ++d->indirectCalls; }
  if (d->debug && count) {
    ++c.diagnosticDraws; ++c.diagnosticIndirect;
    c.diagnosticIndexed += indexed;
  }
  if (sampledTracking(d) && count) { c.sampledOverflow = true; c.sampledTotalDraws = std::min(c.sampledTotalDraws + 1, 1000u); }
  traceDraw(d, c, indexed ? "indexed-indirect" : "indirect", count, 0);
  const uint32_t draws = std::min(count, 1000u);
  if (!draws) return;
  secondaryDraw(c, indexed, draws);
  if (c.rendering) {
    if (sceneViewport(d, c)) {
      c.draws = std::min(c.draws + draws, 1000u);
      if (indexed) c.indexed = std::min(c.indexed + draws, 1000u);
      if (d->debug) d->qualifiedDraws += draws;
    } else if (d->debug) d->viewportRejects += draws;
  }
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirect(VkCommandBuffer h, VkBuffer buffer,
    VkDeviceSize offset, uint32_t count, uint32_t stride) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  indirectDraw(d, d->commands[h], count, false);
  FN(d, vkCmdDrawIndirect)(h, buffer, offset, count, stride);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirect(VkCommandBuffer h, VkBuffer buffer,
    VkDeviceSize offset, uint32_t count, uint32_t stride) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  indirectDraw(d, d->commands[h], count, true);
  FN(d, vkCmdDrawIndexedIndirect)(h, buffer, offset, count, stride);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirectCount(VkCommandBuffer h, VkBuffer buffer,
    VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countOffset, uint32_t maximum, uint32_t stride) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  indirectDraw(d, d->commands[h], maximum ? 1u : 0u, false);
  FN(d, vkCmdDrawIndirectCount)(h, buffer, offset, countBuffer, countOffset, maximum, stride);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirectCountKHR(VkCommandBuffer h, VkBuffer buffer,
    VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countOffset, uint32_t maximum, uint32_t stride) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  indirectDraw(d, d->commands[h], maximum ? 1u : 0u, false);
  FN(d, vkCmdDrawIndirectCountKHR)(h, buffer, offset, countBuffer, countOffset, maximum, stride);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndirectCountAMD(VkCommandBuffer h, VkBuffer buffer,
    VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countOffset, uint32_t maximum, uint32_t stride) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  indirectDraw(d, d->commands[h], maximum ? 1u : 0u, false);
  FN(d, vkCmdDrawIndirectCountAMD)(h, buffer, offset, countBuffer, countOffset, maximum, stride);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirectCount(VkCommandBuffer h, VkBuffer buffer,
    VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countOffset, uint32_t maximum, uint32_t stride) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  indirectDraw(d, d->commands[h], maximum ? 1u : 0u, true);
  FN(d, vkCmdDrawIndexedIndirectCount)(h, buffer, offset, countBuffer, countOffset, maximum, stride);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirectCountKHR(VkCommandBuffer h, VkBuffer buffer,
    VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countOffset, uint32_t maximum, uint32_t stride) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  indirectDraw(d, d->commands[h], maximum ? 1u : 0u, true);
  FN(d, vkCmdDrawIndexedIndirectCountKHR)(h, buffer, offset, countBuffer, countOffset, maximum, stride);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexedIndirectCountAMD(VkCommandBuffer h, VkBuffer buffer,
    VkDeviceSize offset, VkBuffer countBuffer, VkDeviceSize countOffset, uint32_t maximum, uint32_t stride) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  indirectDraw(d, d->commands[h], maximum ? 1u : 0u, true);
  FN(d, vkCmdDrawIndexedIndirectCountAMD)(h, buffer, offset, countBuffer, countOffset, maximum, stride);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdExecuteCommands(
    VkCommandBuffer h, uint32_t count, const VkCommandBuffer *buffers) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  auto &parent = d->commands[h];
  if (sampledTracking(d) && parent.rendering) {
    for (unsigned j = 0; j < count; ++j) {
      auto child = d->commands.find(buffers[j]);
      if (child == d->commands.end()) { parent.sampledOverflow = true; continue; }
      const auto &c = child->second;
      parent.sampledTotalDraws = std::min(parent.sampledTotalDraws + c.sampledTotalDraws, 1000u);
      parent.sampledOverflow |= c.sampledOverflow || !c.inherited || c.family != parent.family || c.viewMask != parent.viewMask;
      for (const auto &draw : c.sampledDraws) {
        if (parent.sampledDraws.size() < 8) parent.sampledDraws.push_back(draw);
        else parent.sampledOverflow = true;
      }
    }
  }
  uint32_t added = 0;
  if (d->debug && parent.rendering && parent.diagnosticTarget) {
    for (unsigned j = 0; j < count; ++j) {
      ++parent.traceChildCount;
      auto it = d->commands.find(buffers[j]);
      if (it == d->commands.end()) {
        if (parent.traceChildCount <= 4) parent.traceChildren += " {untracked-child}";
        continue;
      }
      const auto &child = it->second;
      // Raw diagnostic accounting deliberately ignores production inheritance,
      // size, viewport and geometry filters so exclusions remain visible.
      parent.diagnosticDraws += child.diagnosticDraws;
      parent.diagnosticIndexed += child.diagnosticIndexed;
      parent.diagnosticIndirect += child.diagnosticIndirect;
      parent.diagnosticVertices += child.diagnosticVertices;
      parent.diagnosticSmall += child.diagnosticSmall;
      if (parent.traceChildCount <= 4) {
        std::ostringstream out;
        out << " {child " << child.traceInheritance << " family=" << child.family
            << " acceptedInheritance=" << child.inherited << " batches=" << child.batches.size()
            << " raw=" << child.diagnosticDraws << child.traceSamples << '}';
        parent.traceChildren += out.str();
      }
    }
  }
  if (d->debug && parent.rendering && parent.mrtSlots >= 2 && parent.secondaryContents) {
    for (unsigned i = 0; i < count; ++i) {
      auto it = d->commands.find(buffers[i]);
      if (it == d->commands.end()) continue;
      const auto &child = it->second;
      if (!child.diagnosticInheritance || child.family != parent.family || child.viewMask != parent.viewMask) continue;
      parent.diagnosticDraws += child.diagnosticDraws;
      parent.diagnosticIndexed += child.diagnosticIndexed;
      parent.diagnosticIndirect += child.diagnosticIndirect;
      parent.diagnosticVertices += child.diagnosticVertices;
    }
  }
  if (parent.rendering && parent.secondaryContents && parent.color && parent.depth) {
    for (uint32_t i = 0; i < count; ++i) {
      auto it = d->commands.find(buffers[i]);
      if (it == d->commands.end()) continue;
      const auto &child = it->second;
      if (!child.inherited || child.family != parent.family || child.viewMask != parent.viewMask) continue;
      for (const auto &b : child.batches) {
        Commands viewport;
        viewport.color = parent.color;
        viewport.viewportCount = b.count;
        viewport.viewports = b.viewports;
        if (sceneViewport(d, viewport)) {
          parent.draws = std::min(parent.draws + b.draws, 1000u);
          parent.indexed = std::min(parent.indexed + b.indexed, 1000u);
          parent.indexedVertices = std::min(parent.indexedVertices + b.indexedVertices, 1000000u);
          added += b.draws;
          if (d->debug) d->qualifiedDraws += b.draws;
        } else if (d->debug) d->viewportRejects += b.draws;
      }
    }
    if (d->debug && d->secondaryMessages++ < 12)
      LOG("secondary execution: buffers=%u qualified=%u parentDraws=%u indexed=%u signature=%u",
          count, added, parent.draws, parent.indexed, parent.signature);
  }
  // Never change child inheritance or the application's command list. The map
  // is attached to the parent at BeginRendering, using prior-pass evidence.
  auto fn = reinterpret_cast<PFN_vkCmdExecuteCommands>(d->next(d->handle, "vkCmdExecuteCommands"));
  fn(h, count, buffers);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer h, VkImage src,
                                                 VkImageLayout sl, VkImage dst,
                                                 VkImageLayout dl, uint32_t n,
                                                 const VkImageCopy *regions) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  for (unsigned i = 0; i < n; i++) {
    auto r = regions[i];
    traceTransfer(d, "copy", src, dst, r.extent.width, r.extent.height,
        r.srcOffset, r.dstOffset, r.srcSubresource, r.dstSubresource);
    edge(d, src, dst, r.extent.width, r.extent.height, r.srcOffset, r.dstOffset,
         r.srcSubresource.mipLevel, r.dstSubresource.mipLevel);
  }
  FN(d, vkCmdCopyImage)(h, src, sl, dst, dl, n, regions);
}
EXPORT VKAPI_ATTR void VKAPI_CALL
vkCmdResolveImage(VkCommandBuffer h, VkImage src, VkImageLayout sl, VkImage dst,
                  VkImageLayout dl, uint32_t n, const VkImageResolve *regions) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  for (unsigned i = 0; i < n; i++) {
    auto r = regions[i];
    traceTransfer(d, "resolve", src, dst, r.extent.width, r.extent.height,
        r.srcOffset, r.dstOffset, r.srcSubresource, r.dstSubresource);
    edge(d, src, dst, r.extent.width, r.extent.height, r.srcOffset, r.dstOffset,
         r.srcSubresource.mipLevel, r.dstSubresource.mipLevel);
  }
  FN(d, vkCmdResolveImage)(h, src, sl, dst, dl, n, regions);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage2(VkCommandBuffer h,
                                                  const VkCopyImageInfo2 *ci) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  for (unsigned i = 0; i < ci->regionCount; i++) {
    auto r = ci->pRegions[i];
    traceTransfer(d, "copy2", ci->srcImage, ci->dstImage, r.extent.width, r.extent.height,
        r.srcOffset, r.dstOffset, r.srcSubresource, r.dstSubresource);
    edge(d, ci->srcImage, ci->dstImage, r.extent.width, r.extent.height,
         r.srcOffset, r.dstOffset, r.srcSubresource.mipLevel,
         r.dstSubresource.mipLevel);
  }
  FN(d, vkCmdCopyImage2)(h, ci);
}
EXPORT VKAPI_ATTR void VKAPI_CALL
vkCmdResolveImage2(VkCommandBuffer h, const VkResolveImageInfo2 *ci) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  for (unsigned i = 0; i < ci->regionCount; i++) {
    auto r = ci->pRegions[i];
    traceTransfer(d, "resolve2", ci->srcImage, ci->dstImage, r.extent.width, r.extent.height,
        r.srcOffset, r.dstOffset, r.srcSubresource, r.dstSubresource);
    edge(d, ci->srcImage, ci->dstImage, r.extent.width, r.extent.height,
         r.srcOffset, r.dstOffset, r.srcSubresource.mipLevel,
         r.dstSubresource.mipLevel);
  }
  FN(d, vkCmdResolveImage2)(h, ci);
}
namespace {
// A flags2 structure overrides the legacy flags field. Copy the prefix through
// it without mutating the caller's pNext chain (DXVK may reuse it
// concurrently).
bool patchFlags2(VkGraphicsPipelineCreateInfo &ci,
                 std::vector<std::unique_ptr<uint8_t[]>> &storage) {
  auto first = static_cast<const VkBaseInStructure *>(ci.pNext);
  auto target = first;
  while (target &&
         target->sType !=
             VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR)
    target = target->pNext;
  if (!target)
    return true;
  VkBaseOutStructure *previous = nullptr;
  for (auto node = first; node; node = node->pNext) {
    size_t size = 0;
#define STRUCT(tag, type)                                                      \
  case tag:                                                                    \
    size = sizeof(type);                                                       \
    break
    switch (node->sType) {
      STRUCT(VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO_KHR,
             VkPipelineCreateFlags2CreateInfoKHR);
      STRUCT(VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
             VkPipelineRenderingCreateInfo);
      STRUCT(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
             VkGraphicsPipelineLibraryCreateInfoEXT);
      STRUCT(VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
             VkPipelineLibraryCreateInfoKHR);
      STRUCT(VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
             VkPipelineCreationFeedbackCreateInfo);
      STRUCT(VK_STRUCTURE_TYPE_PIPELINE_ROBUSTNESS_CREATE_INFO_EXT,
             VkPipelineRobustnessCreateInfoEXT);
      STRUCT(VK_STRUCTURE_TYPE_ATTACHMENT_SAMPLE_COUNT_INFO_AMD,
             VkAttachmentSampleCountInfoAMD);
      STRUCT(VK_STRUCTURE_TYPE_MULTIVIEW_PER_VIEW_ATTRIBUTES_INFO_NVX,
             VkMultiviewPerViewAttributesInfoNVX);
      STRUCT(VK_STRUCTURE_TYPE_PIPELINE_COMPILER_CONTROL_CREATE_INFO_AMD,
             VkPipelineCompilerControlCreateInfoAMD);
      STRUCT(VK_STRUCTURE_TYPE_PIPELINE_DISCARD_RECTANGLE_STATE_CREATE_INFO_EXT,
             VkPipelineDiscardRectangleStateCreateInfoEXT);
      STRUCT(
          VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_ENUM_STATE_CREATE_INFO_NV,
          VkPipelineFragmentShadingRateEnumStateCreateInfoNV);
      STRUCT(
          VK_STRUCTURE_TYPE_PIPELINE_FRAGMENT_SHADING_RATE_STATE_CREATE_INFO_KHR,
          VkPipelineFragmentShadingRateStateCreateInfoKHR);
      STRUCT(
          VK_STRUCTURE_TYPE_PIPELINE_REPRESENTATIVE_FRAGMENT_TEST_STATE_CREATE_INFO_NV,
          VkPipelineRepresentativeFragmentTestStateCreateInfoNV);
    default:
      return false;
    }
#undef STRUCT
    auto bytes = std::make_unique<uint8_t[]>(size);
    std::memcpy(bytes.get(), node, size);
    auto copy = reinterpret_cast<VkBaseOutStructure *>(bytes.get());
    storage.push_back(std::move(bytes));
    if (previous)
      previous->pNext = copy;
    else
      ci.pNext = copy;
    if (node == target) {
      reinterpret_cast<VkPipelineCreateFlags2CreateInfoKHR *>(copy)->flags |=
          VK_PIPELINE_CREATE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT;
      return true;
    }
    previous = copy;
  }
  return false;
}
} // namespace
EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkCreateGraphicsPipelines(VkDevice h, VkPipelineCache cache, uint32_t n,
                          const VkGraphicsPipelineCreateInfo *ci,
                          const VkAllocationCallbacks *a, VkPipeline *out) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  std::vector<VkGraphicsPipelineCreateInfo> copy(ci, ci + n);
  std::vector<std::unique_ptr<uint8_t[]>> chains;
  // Dynamic rendering permits these pipelines both with and without an FDM.
  for (auto &p : copy)
    if (d->enabled && !p.renderPass) {
      if (!patchFlags2(p, chains)) {
        // Unknown chain layout cannot be rewritten safely. Explicitly reject
        // this pipeline rather than forwarding an invalid FDM combination.
        LOG("unsupported flags2 pNext chain; disable experimental FFR for this "
            "game");
        for (uint32_t i = 0; i < n; i++)
          out[i] = VK_NULL_HANDLE;
        return VK_ERROR_FEATURE_NOT_PRESENT;
      }
      p.flags |=
          VK_PIPELINE_CREATE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT;
    }
  return FN(d, vkCreateGraphicsPipelines)(h, cache, n, copy.data(), a, out);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice h, uint32_t family,
                                                   uint32_t index,
                                                   VkQueue *out) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  FN(d, vkGetDeviceQueue)(h, family, index, out);
  queueFamilies[*out] = family;
}
EXPORT VKAPI_ATTR void VKAPI_CALL
vkGetDeviceQueue2(VkDevice h, const VkDeviceQueueInfo2 *ci, VkQueue *out) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  FN(d, vkGetDeviceQueue2)(h, ci, out);
  if (*out)
    queueFamilies[*out] = ci->queueFamilyIndex;
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue q, uint32_t n,
                                                    const VkSubmitInfo *si,
                                                    VkFence f) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(q);
  submitMaps(d, q);
  return FN(d, vkQueueSubmit)(q, n, si, f);
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue q, uint32_t n,
                                                     const VkSubmitInfo2 *si,
                                                     VkFence f) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(q);
  submitMaps(d, q);
  return FN(d, vkQueueSubmit2)(q, n, si, f);
}
#include "descriptor_trace_hooks.inc"
namespace {
PFN_vkVoidFunction intercept(const char *n) {
#define HOOK(f)                                                                \
  if (!strcmp(n, #f))                                                          \
  return reinterpret_cast<PFN_vkVoidFunction>(f)
  HOOK(vkGetInstanceProcAddr);
  HOOK(vkGetDeviceProcAddr);
  HOOK(vkCreateInstance);
  HOOK(vkDestroyInstance);
  HOOK(vkCreateDevice);
  HOOK(vkDestroyDevice);
  HOOK(vkCreateDescriptorSetLayout);
  HOOK(vkDestroyDescriptorSetLayout);
  HOOK(vkCreatePipelineLayout);
  HOOK(vkDestroyPipelineLayout);
  HOOK(vkAllocateDescriptorSets);
  HOOK(vkFreeDescriptorSets);
  HOOK(vkResetDescriptorPool);
  HOOK(vkDestroyDescriptorPool);
  HOOK(vkUpdateDescriptorSets);
  HOOK(vkCmdBindDescriptorSets);
  HOOK(vkCreateDescriptorUpdateTemplate);
  HOOK(vkDestroyDescriptorUpdateTemplate);
  HOOK(vkUpdateDescriptorSetWithTemplate);
  HOOK(vkCmdPushDescriptorSetKHR);
  HOOK(vkCmdPushDescriptorSetWithTemplateKHR);
  HOOK(vkCreateImage);
  HOOK(vkDestroyImage);
  HOOK(vkCreateImageView);
  HOOK(vkDestroyImageView);
  HOOK(vkCreateCommandPool);
  HOOK(vkDestroyCommandPool);
  HOOK(vkAllocateCommandBuffers);
  HOOK(vkFreeCommandBuffers);
  HOOK(vkBeginCommandBuffer);
  HOOK(vkCmdBeginRenderPass);
  HOOK(vkCmdBeginRenderPass2);
  HOOK(vkCmdBeginRendering);
  HOOK(vkCmdEndRendering);
  HOOK(vkCmdSetScissor);
  HOOK(vkCmdSetScissorWithCount);
  HOOK(vkCmdSetViewport);
  HOOK(vkCmdSetViewportWithCount);
  HOOK(vkCmdExecuteCommands);
  HOOK(vkCmdDraw);
  HOOK(vkCmdDrawIndexed);
  HOOK(vkCmdDrawIndirect);
  HOOK(vkCmdDrawIndexedIndirect);
  HOOK(vkCmdDrawIndirectCount);
  HOOK(vkCmdDrawIndirectCountKHR);
  HOOK(vkCmdDrawIndirectCountAMD);
  HOOK(vkCmdDrawIndexedIndirectCount);
  HOOK(vkCmdDrawIndexedIndirectCountKHR);
  HOOK(vkCmdDrawIndexedIndirectCountAMD);

  HOOK(vkCmdCopyImage);
  HOOK(vkCmdResolveImage);
  HOOK(vkCmdCopyImage2);
  HOOK(vkCmdResolveImage2);
  HOOK(vkCreateGraphicsPipelines);
  HOOK(vkGetDeviceQueue);
  HOOK(vkGetDeviceQueue2);
  HOOK(vkQueueSubmit);
  HOOK(vkQueueSubmit2);
#undef HOOK
#define ALIAS(f)                                                               \
  if (!strcmp(n, #f "KHR"))                                                    \
  return reinterpret_cast<PFN_vkVoidFunction>(f)
  if (!strcmp(n, "vkCmdSetScissorWithCountEXT")) return reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetScissorWithCount);
  if (!strcmp(n, "vkCmdSetViewportWithCountEXT"))
    return reinterpret_cast<PFN_vkVoidFunction>(vkCmdSetViewportWithCount);
  ALIAS(vkCmdBeginRenderPass2);
  ALIAS(vkCmdBeginRendering);
  ALIAS(vkCmdEndRendering);
  ALIAS(vkCreateDescriptorUpdateTemplate);
  ALIAS(vkDestroyDescriptorUpdateTemplate);
  ALIAS(vkUpdateDescriptorSetWithTemplate);
  ALIAS(vkCmdCopyImage2);
  ALIAS(vkCmdResolveImage2);
  ALIAS(vkQueueSubmit2);
#undef ALIAS
  return nullptr;
}
} // namespace
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetDeviceProcAddr(VkDevice h, const char *n) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  auto d = dev(h);
  if (!d)
    return nullptr;
  auto downstream = d->next(h, n);
  if (!downstream)
    return nullptr;
  if (auto f = intercept(n))
    return f;
  return downstream;
}
EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vkGetInstanceProcAddr(VkInstance h, const char *n) {
  std::lock_guard<std::recursive_mutex> guard(lock);
  if (auto f = intercept(n))
    return f;
  auto i = instances.find(key(h));
  return i == instances.end() ? nullptr : i->second.gipa(h, n);
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *p) {
  if (p->loaderLayerInterfaceVersion < 2)
    return VK_ERROR_INITIALIZATION_FAILED;
  p->loaderLayerInterfaceVersion = 2;
  p->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
  p->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
  p->pfnGetPhysicalDeviceProcAddr = nullptr;
  return VK_SUCCESS;
}
