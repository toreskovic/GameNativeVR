#include "xr_performance.h"
#include "xr_filter_sampling.h"
#include "../../windows/openxr_runtime/gamenative_foveated_packing.h"
#include "xr_present_foveation.h"
#include "xr_vulkan_present.h"
#include "xr_lsfg_timing.h"
#include "../lsfg/lsfg_common.hpp"
#include "xr_lsfg_capture.h"
#include "xr_vulkan_dispatch.h"
#include "xr_vulkan_present_spv.h"
#include "xr_vulkan_diagnostic.h"
#include <algorithm>
#include <android/log.h>
#include <cstring>
#include <unistd.h>
namespace xrimmersive {
namespace {
VkImageMemoryBarrier imageBarrier(VkImage i, VkImageLayout old,
                                  VkImageLayout next, VkAccessFlags src,
                                  VkAccessFlags dst,
                                  uint32_t from = VK_QUEUE_FAMILY_IGNORED,
                                  uint32_t to = VK_QUEUE_FAMILY_IGNORED) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.image = i;
  b.oldLayout = old;
  b.newLayout = next;
  b.srcAccessMask = src;
  b.dstAccessMask = dst;
  b.srcQueueFamilyIndex = from;
  b.dstQueueFamilyIndex = to;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  return b;
}
} // namespace
struct XrVulkanPresenter::Impl {
  XrVulkanContext *ctx{};
  XrSwapchain swapchain{};
  VkFormat format{};
  uint32_t w = 0, h = 0, layers = 0;
  int upscaler = 0, border = 0;
  bool fxaa = false;
  bool subsampled = false;
  bool quadImportWarning = false;
  int ffrDebug = 0;
  float sharpness = .7f, fov = 1;
  PresentationDensityMap densityMap;
  VkRenderPass pass{};
  VkPipelineLayout layout{};
  std::array<int,6> filterKeys[2]{};
  bool filterCentral[2]{};
  std::array<XrFovf,2> maskFovs{};
  uint64_t maskRevision=UINT64_MAX, maskGeneration=0;
  std::vector<XrVector2f> maskVertices;
  uint32_t maskOffsets[2]{}, maskCounts[2]{};
  PFN_vkCmdBindVertexBuffers bindVertices{};
  VkPipeline maskedPipelines[3]{};
  VkPipeline pipelines[3]{}; // ordinary, packed-safe, packed-central
  VkDescriptorSetLayout setLayout{};
  VkDescriptorPool descriptors{};
  VkCommandPool pool{};
  VkSampler sampler{};
  PFN_vkGetSemaphoreFdKHR exportFd{};
  PFN_vkImportSemaphoreFdKHR importFd{};
  struct Slot {
    lsfg::Buffer maskBuffer;
    size_t maskCapacity=0;
    uint64_t maskGeneration=UINT64_MAX;
    VkImage image{};
    VkImageView views[2]{};
    VkFramebuffer targets[2]{};
    VkCommandBuffer cmd{};
    VkFence fence{};
    VkSemaphore done{}, waits[2]{};
    VkDescriptorSet sets[2]{};
    std::unique_ptr<windowsvr::VulkanEyeCapture> imports;
    VkBuffer upload{};
    VkDeviceMemory uploadMemory{};
    void *mapped{};
    VkDeviceSize capacity = 0;
    std::unique_ptr<lsfg::LsfgImage> overlay;
    uint64_t version = UINT64_MAX;
    bool uploaded = false;
    VkQueryPool timing{};
    bool timingPending=false;
    unsigned timedEffects=0;
  };
  lsfg::Device deviceInfo;
  std::vector<Slot> slots;
  bool initialized = false;
  uint32_t timestampBits = 0;
  int64_t lastTimingSample = 0;
#ifdef GN_XR_SWAPCHAIN_PATTERN
  PFN_vkCmdClearAttachments clearAttachments{};
  bool patternReported = false;
#endif
  void shutdown() {
    if (!ctx)
      return;
    ctx->idle();
    for (auto &s : slots) {
      if(s.timing) vkDestroyQueryPool(ctx->device,s.timing,nullptr);
      s.imports.reset();
      s.overlay.reset();
      if (s.mapped)
        vkUnmapMemory(ctx->device, s.uploadMemory);
      if (s.upload)
        vkDestroyBuffer(ctx->device, s.upload, nullptr);
      if (s.uploadMemory)
        vkFreeMemory(ctx->device, s.uploadMemory, nullptr);
      for (int e = 0; e < 2; ++e) {
        if (s.targets[e])
          vkDestroyFramebuffer(ctx->device, s.targets[e], nullptr);
        if (s.views[e])
          vkDestroyImageView(ctx->device, s.views[e], nullptr);
        if (s.waits[e])
          vkDestroySemaphore(ctx->device, s.waits[e], nullptr);
      }
      if (s.done)
        vkDestroySemaphore(ctx->device, s.done, nullptr);
      if (s.fence)
        vkDestroyFence(ctx->device, s.fence, nullptr);
    }
    slots.clear();
    for (auto pipeline : maskedPipelines)
      if (pipeline) vkDestroyPipeline(ctx->device,pipeline,nullptr);
    for (auto pipeline : pipelines)
      if (pipeline) vkDestroyPipeline(ctx->device, pipeline, nullptr);
    if (layout)
      vkDestroyPipelineLayout(ctx->device, layout, nullptr);
    if (pass)
      vkDestroyRenderPass(ctx->device, pass, nullptr);
    densityMap.reset();
    if (descriptors)
      vkDestroyDescriptorPool(ctx->device, descriptors, nullptr);
    if (setLayout)
      vkDestroyDescriptorSetLayout(ctx->device, setLayout, nullptr);
    if (sampler)
      vkDestroySampler(ctx->device, sampler, nullptr);
    if (pool)
      vkDestroyCommandPool(ctx->device, pool, nullptr);
    ctx = nullptr;
  }
  ~Impl() { shutdown(); }
  bool initialize() {
    deviceInfo = lsfg::Device(ctx->device, ctx->physical);
    bindVertices=reinterpret_cast<PFN_vkCmdBindVertexBuffers>(vkd.GetDeviceProcAddr(ctx->device,"vkCmdBindVertexBuffers"));
    if(!bindVertices) return false;
    exportFd = reinterpret_cast<PFN_vkGetSemaphoreFdKHR>(
        vkd.GetDeviceProcAddr(ctx->device, "vkGetSemaphoreFdKHR"));
    importFd = reinterpret_cast<PFN_vkImportSemaphoreFdKHR>(
        vkd.GetDeviceProcAddr(ctx->device, "vkImportSemaphoreFdKHR"));
    if (!exportFd || !importFd)
      return false;
    uint32_t n = 0;
    if (XR_FAILED(xrEnumerateSwapchainImages(swapchain, 0, &n, nullptr)) || !n)
      return false;
    std::vector<XrSwapchainImageVulkanKHR> images(
        n, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR});
    if (XR_FAILED(xrEnumerateSwapchainImages(
            swapchain, n, &n,
            reinterpret_cast<XrSwapchainImageBaseHeader *>(images.data()))))
      return false;
    slots.resize(n);

    if(layers==2 && !kXrSwapchainPattern && ctx->presentationDensity) {
      std::lock_guard<std::mutex> lock(ctx->queueMutex);
      if(!densityMap.initialize(deviceInfo,ctx->queue,ctx->family,{w,h},ctx->densityTexel,
          (border ? fov : 1.f)))
        __android_log_print(ANDROID_LOG_WARN,"VrVulkan","Presentation density allocation/upload failed; using full rate");
    }
    // A subsampled target must never silently fall back to a pass without FDM.
    // Let the owner discard this swapchain and retry ordinary images instead.
    if(subsampled && !densityMap.view()) return false;
    VkAttachmentDescription attachment{};
    attachment.format = format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &color;
    VkSubpassDependency dep{VK_SUBPASS_EXTERNAL,
                            0,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                            0,
                            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                            0};
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    VkAttachmentDescription attachments[2]={attachment,{}};
    attachments[1].format=VK_FORMAT_R8G8_UNORM;
    attachments[1].samples=VK_SAMPLE_COUNT_1_BIT;
    attachments[1].loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;
    attachments[1].storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachments[1].stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[1].stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[1].initialLayout=attachments[1].finalLayout=VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
    VkRenderPassFragmentDensityMapCreateInfoEXT densityInfo{VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT};
    densityInfo.fragmentDensityMapAttachment={1,VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT};
    rp.pNext=densityMap.view()?&densityInfo:nullptr;
    rp.attachmentCount=densityMap.view()?2:1;
    rp.pAttachments=attachments;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 1;
    rp.pDependencies = &dep;
    if (vkCreateRenderPass(ctx->device, &rp, nullptr, &pass) != VK_SUCCESS) {
      if(subsampled || !densityMap.view()) return false;
      densityMap.reset(); rp.pNext=nullptr; rp.attachmentCount=1;
      if(vkCreateRenderPass(ctx->device,&rp,nullptr,&pass)!=VK_SUCCESS) return false;
    }
    if(layers==2) __android_log_print(ANDROID_LOG_INFO,"VrVulkan",
        "Presentation density: active=%d subsampled=%d fullRateRadius=%.3f outerRadii=0.800/1.050 rates=1x1/2x2/4x4 extent=%ux%u",
        densityMap.view()!=VK_NULL_HANDLE,subsampled,ffr::kInnerRadius*(border?fov:1.f),w,h);
    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo sl{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = 2;
    sl.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(ctx->device, &sl, nullptr, &setLayout) !=
        VK_SUCCESS)
      return false;
    VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
    VkPipelineLayoutCreateInfo pl{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &setLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    if (vkCreatePipelineLayout(ctx->device, &pl, nullptr, &layout) !=
        VK_SUCCESS)
      return false;
    VkShaderModule vertex{}, fragment{};
    auto module = [&](auto &code, VkShaderModule &out) {
      VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      ci.codeSize = sizeof(code);
      ci.pCode = code;
      return vkCreateShaderModule(ctx->device, &ci, nullptr, &out) ==
             VK_SUCCESS;
    };
    if (!module(presentVertex, vertex))
      return false;
    bool ok = upscaler == 3 ? module(presentEdge, fragment)
                            : module(presentBasic, fragment);
    if (!ok) {
      vkDestroyShaderModule(ctx->device, vertex, nullptr);
      return false;
    }
    VkPipelineShaderStageCreateInfo stages[] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_VERTEX_BIT, vertex, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main", nullptr}};
    // Remove all FXAA code from the driver-compiled shader when disabled.
    VkBool32 values[] = {fxaa ? VK_TRUE : VK_FALSE, VK_FALSE, VK_FALSE};
    const VkSpecializationMapEntry entries[] = {{0, 0, sizeof(VkBool32)}, {1, sizeof(VkBool32), sizeof(VkBool32)}, {2, 2*sizeof(VkBool32), sizeof(VkBool32)}};
    const VkSpecializationInfo fxaaSpecialization{3, entries, sizeof(values), values};
    stages[1].pSpecializationInfo = &fxaaSpecialization;
    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.lineWidth = 1;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo blend{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &ba;
    VkDynamicState ds[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{
        VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = ds;
    VkGraphicsPipelineCreateInfo gp{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &blend;
    gp.pDynamicState = &dy;
    gp.layout = layout;
    gp.renderPass = pass;
    VkResult result = VK_SUCCESS;
    // Flat UI never uses packed transport or visibility meshes. Avoid compiling
    // five unused variants before the first loading-screen frame can be shown.
    const unsigned pipelineCount = layers == 2 ? 3u : 1u;
    for (unsigned packed = 0; packed < pipelineCount; ++packed) {
      values[1] = packed != 0;
      values[2] = packed == 2;
      result = vkCreateGraphicsPipelines(ctx->device, VK_NULL_HANDLE, 1, &gp, nullptr, &pipelines[packed]);
      if (result != VK_SUCCESS) break;
    }
    vkDestroyShaderModule(ctx->device, vertex, nullptr);
    if(result==VK_SUCCESS && layers==2) {
      if (!module(presentMaskedVertex,vertex)) {
        vkDestroyShaderModule(ctx->device, fragment, nullptr);
        return false;
      }
      stages[0].module=vertex;
      const VkVertexInputBindingDescription binding{0,sizeof(XrVector2f),VK_VERTEX_INPUT_RATE_VERTEX};
      const VkVertexInputAttributeDescription attribute{0,0,VK_FORMAT_R32G32_SFLOAT,0};
      vi.vertexBindingDescriptionCount=1;vi.pVertexBindingDescriptions=&binding;
      vi.vertexAttributeDescriptionCount=1;vi.pVertexAttributeDescriptions=&attribute;
      for(unsigned packed=0;packed<3;++packed) {
        values[1]=packed!=0;values[2]=packed==2;
        result=vkCreateGraphicsPipelines(ctx->device,VK_NULL_HANDLE,1,&gp,nullptr,&maskedPipelines[packed]);
        if(result!=VK_SUCCESS) break;
      }
      vkDestroyShaderModule(ctx->device,vertex,nullptr);
    }
    vkDestroyShaderModule(ctx->device, fragment, nullptr);
    if (result != VK_SUCCESS)
      return false;
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.queueFamilyIndex = ctx->family;
    pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(ctx->device, &pc, nullptr, &pool) != VK_SUCCESS)
      return false;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                              n * layers * 2};
    VkDescriptorPoolCreateInfo dp{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = n * layers;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &size;
    if (vkCreateDescriptorPool(ctx->device, &dp, nullptr, &descriptors) !=
        VK_SUCCESS)
      return false;
    VkSamplerCreateInfo sm{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sm.magFilter = sm.minFilter = VK_FILTER_LINEAR;
    sm.addressModeU = sm.addressModeV = sm.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(ctx->device, &sm, nullptr, &sampler) != VK_SUCCESS)
      return false;
    for (uint32_t i = 0; i < n; ++i) {
      auto &s = slots[i];
      s.image = images[i].image;
      VkCommandBufferAllocateInfo ca{
          VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
      ca.commandPool = pool;
      ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
      ca.commandBufferCount = 1;
      if (vkAllocateCommandBuffers(ctx->device, &ca, &s.cmd) != VK_SUCCESS)
        return false;
      VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      fc.flags = VK_FENCE_CREATE_SIGNALED_BIT;
      if (vkCreateFence(ctx->device, &fc, nullptr, &s.fence) != VK_SUCCESS)
        return false;
      VkExportSemaphoreCreateInfo ex{
          VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
      ex.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
      VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
      sc.pNext = &ex;
      if (vkCreateSemaphore(ctx->device, &sc, nullptr, &s.done) != VK_SUCCESS)
        return false;
      sc.pNext = nullptr;
      for (uint32_t e = 0; e < layers; ++e) {
        if (vkCreateSemaphore(ctx->device, &sc, nullptr, &s.waits[e]) !=
            VK_SUCCESS)
          return false;
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = s.image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, e, 1};
        if (vkCreateImageView(ctx->device, &view, nullptr, &s.views[e]) !=
            VK_SUCCESS)
          return false;
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = pass;
        const VkImageView attachmentViews[]={s.views[e],densityMap.view()};
        fb.attachmentCount = densityMap.view()?2:1;
        fb.pAttachments = attachmentViews;
        fb.width = w;
        fb.height = h;
        fb.layers = 1;
        if (vkCreateFramebuffer(ctx->device, &fb, nullptr, &s.targets[e]) !=
            VK_SUCCESS)
          return false;
        VkDescriptorSetAllocateInfo da{
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = descriptors;
        da.descriptorSetCount = 1;
        da.pSetLayouts = &setLayout;
        if (vkAllocateDescriptorSets(ctx->device, &da, &s.sets[e]) !=
            VK_SUCCESS)
          return false;
      }
      s.imports = std::make_unique<windowsvr::VulkanEyeCapture>(
          ctx->device, ctx->ahbProperties, false);
      if (!s.imports->valid())
        return false;
    }
#ifdef GN_XR_SWAPCHAIN_PATTERN
    clearAttachments = reinterpret_cast<PFN_vkCmdClearAttachments>(
        vkd.GetDeviceProcAddr(ctx->device, "vkCmdClearAttachments"));
    if (!clearAttachments)
      return false;
    __android_log_print(ANDROID_LOG_INFO, "VrVulkanPattern",
                        "DIAGNOSTIC ONLY: %ux%u layers=%u format=%d images=%u; "
                        "attachment clears, no input sampling or frame generation",
                        w, h, layers, int(format), n);
#endif
    initialized = true;
    return true;
  }
  bool upload(Slot &s, const std::vector<uint8_t> &pixels, int width,
              int height, uint64_t version) {
    const uint32_t iw = width > 0 ? width : 1, ih = height > 0 ? height : 1;
    const VkDeviceSize bytes = VkDeviceSize(iw) * ih * 4;
    if (!s.overlay || s.overlay->Extent().width != iw ||
        s.overlay->Extent().height != ih) {
      s.overlay = std::make_unique<lsfg::LsfgImage>(
          deviceInfo, VkExtent2D{iw, ih}, VK_FORMAT_R8G8B8A8_UNORM);
      if (!s.overlay->Valid())
        return false;
      s.uploaded = false;
      s.version = UINT64_MAX;
    }
    if (s.version == version && s.uploaded)
      return true;
    if (s.capacity < bytes) {
      if (s.mapped)
        vkUnmapMemory(ctx->device, s.uploadMemory);
      if (s.upload)
        vkDestroyBuffer(ctx->device, s.upload, nullptr);
      if (s.uploadMemory)
        vkFreeMemory(ctx->device, s.uploadMemory, nullptr);
      s.upload = {};
      s.uploadMemory = {};
      s.mapped = nullptr;
      s.capacity = 0;
      VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
      bi.size = bytes;
      bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
      if (vkCreateBuffer(ctx->device, &bi, nullptr, &s.upload) != VK_SUCCESS)
        return false;
      VkMemoryRequirements req{};
      vkGetBufferMemoryRequirements(ctx->device, s.upload, &req);
      uint32_t type = deviceInfo.FindMemoryType(
          req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (type == UINT32_MAX)
        return false;
      VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      ai.allocationSize = req.size;
      ai.memoryTypeIndex = type;
      if (vkAllocateMemory(ctx->device, &ai, nullptr, &s.uploadMemory) !=
              VK_SUCCESS ||
          vkBindBufferMemory(ctx->device, s.upload, s.uploadMemory, 0) !=
              VK_SUCCESS ||
          vkMapMemory(ctx->device, s.uploadMemory, 0, req.size, 0, &s.mapped) !=
              VK_SUCCESS)
        return false;
      s.capacity = bytes;
    }
    if (pixels.size() >= bytes)
      memcpy(s.mapped, pixels.data(), bytes);
    else
      memset(s.mapped, 0, bytes);
    auto b = imageBarrier(
        s.overlay->Handle(),
        s.uploaded ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_GENERAL, s.uploaded ? VK_ACCESS_SHADER_READ_BIT : 0,
        VK_ACCESS_TRANSFER_WRITE_BIT);
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {iw, ih, 1};
    vkCmdCopyBufferToImage(s.cmd, s.upload, s.overlay->Handle(),
                           VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
    b = imageBarrier(s.overlay->Handle(), VK_IMAGE_LAYOUT_GENERAL,
                     VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_ACCESS_SHADER_READ_BIT);
    vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &b);
    s.uploaded = true;
    s.version = version;
    return true;
  }
  bool render(std::array<Source, 2> sources, int &fd, bool guest,
              const std::vector<uint8_t> *pixels = nullptr, int pw = 0,
              int ph = 0, uint64_t version = 0, float sx = 1, float sy = 1) {
    fd = -1;
    if (!initialized)
      return false;
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ac{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    {
      std::lock_guard<std::mutex> lock(ctx->queueMutex);
      if (XR_FAILED(xrAcquireSwapchainImage(swapchain, &ac, &index)))
        return false;
    }
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(swapchain, &wait)))
      return false;
    auto release = [&] {
      XrSwapchainImageReleaseInfo r{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      std::lock_guard<std::mutex> lock(ctx->queueMutex);
      return XR_SUCCEEDED(xrReleaseSwapchainImage(swapchain, &r));
    };
    auto &s = slots[index];
    if (vkWaitForFences(ctx->device, 1, &s.fence, VK_TRUE, UINT64_MAX) !=
        VK_SUCCESS) {
      release();
      return false;
    }
    if(s.timingPending) {
      uint64_t ticks[2]{};
      // The existing slot fence has retired. Never wait for query results.
      if(vkGetQueryPoolResults(ctx->device,s.timing,0,2,sizeof(ticks),ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT)==VK_SUCCESS) {
        auto duration=[&](unsigned i) { return windowsvr::gpuTimestampDuration(ticks[i],ticks[i+1],timestampBits,ctx->properties.limits.timestampPeriod); };
        vrPerformance.stage(VrPerformanceMetrics::Presentation,duration(0),performanceNow(),s.timedEffects);
      }
      s.timingPending=false;
    }
    const auto now=performanceNow();
    bool measure=!kXrSwapchainPattern && !pixels && layers==2 && vrPerformance.visible && now-lastTimingSample>=1000000000LL;
    if(measure) lastTimingSample=now;
    if(measure && !s.timing) {
      uint32_t count=0;
      vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical,&count,nullptr);
      std::vector<VkQueueFamilyProperties> queues(count);
      vkGetPhysicalDeviceQueueFamilyProperties(ctx->physical,&count,queues.data());
      timestampBits=queues[ctx->family].timestampValidBits;
      VkQueryPoolCreateInfo info{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      info.queryType=VK_QUERY_TYPE_TIMESTAMP; info.queryCount=2;
      if(timestampBits && ctx->properties.limits.timestampPeriod>0)
        vkCreateQueryPool(ctx->device,&info,nullptr,&s.timing);
    }
    measure=measure && s.timing;
    if (guest && !kXrSwapchainPattern) {
      std::array<windowsvr::EyeFrame, 2> frames{sources[0].frame,
                                                sources[1].frame};
      if (!s.imports->import(frames)) {
        if (!pixels || layers != 1) {
          release();
          return false;
        }
        // The Android loading/menu bitmap is independent of the game's AHB.
        // A failed flat-buffer import must not turn that UI into an empty XR
        // submission (and leave the runtime displaying its loading spinner).
        if (!quadImportWarning) {
          __android_log_print(ANDROID_LOG_WARN, "VrVulkan",
              "Flat game buffer import unavailable; presenting Android UI bitmap");
          quadImportWarning = true;
        }
        guest = false;
        sources = {};
      }
      for (uint32_t e = 0; guest && e < layers; ++e) {
        sources[e].image = s.imports->sourceImage(e);
        sources[e].view = s.imports->sourceView(e);
        sources[e].external = true;
        // Guest AHB producers can use a different Vulkan driver.
        sources[e].owner = VK_QUEUE_FAMILY_FOREIGN_EXT;
      }
    }
    vkResetCommandBuffer(s.cmd, 0);
    VkCommandBufferBeginInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cb.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(s.cmd, &cb) != VK_SUCCESS) {
      release();
      return false;
    }
    if (!kXrSwapchainPattern && pixels && !upload(s, *pixels, pw, ph, version)) {
      release();
      return false;
    }
    VkSemaphore waits[2]{};
    const VkPipelineStageFlags sourceStages = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    VkPipelineStageFlags masks[2] = {sourceStages, sourceStages};
    uint32_t count = 0;
    for (uint32_t e = 0; e < layers; ++e) {
#ifdef GN_XR_SWAPCHAIN_PATTERN
      VkRenderPassBeginInfo patternPass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      patternPass.renderPass = pass;
      patternPass.framebuffer = s.targets[e];
      patternPass.renderArea = {{0, 0}, {w, h}};
      const VkClearValue patternBlack{{{0.f,0.f,0.f,1.f}}};
      patternPass.clearValueCount=1;patternPass.pClearValues=&patternBlack;
      vkCmdBeginRenderPass(s.cmd, &patternPass, VK_SUBPASS_CONTENTS_INLINE);
      recordXrSwapchainPattern(clearAttachments, s.cmd, w, h,
                               layers == 1 ? 3 : e + 1);
      vkCmdEndRenderPass(s.cmd);
      continue;
#endif
      auto &src = sources[e];
      if (!src.view && pixels) {
        src.image = s.overlay->Handle();
        src.view = s.overlay->View();
      }
      if (!src.view) {
        release();
        return false;
      }
      if (src.frame.acquireFenceFd >= 0) {
        int copy = dup(src.frame.acquireFenceFd);
        if (copy < 0) {
          release();
          return false;
        }
        VkImportSemaphoreFdInfoKHR imp{
            VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
        imp.semaphore = s.waits[e];
        imp.flags = VK_SEMAPHORE_IMPORT_TEMPORARY_BIT;
        imp.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
        imp.fd = copy;
        if (importFd(ctx->device, &imp) != VK_SUCCESS) {
          close(copy);
          release();
          return false;
        }
        waits[count++] = s.waits[e];
      }
    }
    // Acquire both eyes before either render pass. An acquire between the two
    // passes would also order the second eye behind unrelated first-eye work.
    for (uint32_t e = 0; e < layers && !kXrSwapchainPattern; ++e) {
      auto &src = sources[e];
      auto b = imageBarrier(
          src.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
          src.external ? 0 : VK_ACCESS_SHADER_WRITE_BIT,
          VK_ACCESS_SHADER_READ_BIT,
          src.external ? src.owner : VK_QUEUE_FAMILY_IGNORED,
          src.external ? ctx->family : VK_QUEUE_FAMILY_IGNORED);
      b.subresourceRange.baseArrayLayer = guest ? src.frame.bufferLayer : 0;
      vkCmdPipelineBarrier(s.cmd, src.external ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT
                                        : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           sourceStages, 0, 0, nullptr,
                           0, nullptr, 1, &b);
    }
    // Lightweight AA is fused into both final presentation variants.
    bool usesSgsr=false;
    if(measure) {
      vkCmdResetQueryPool(s.cmd,s.timing,0,2);
    }
    for (uint32_t e = 0; e < layers && !kXrSwapchainPattern && !pixels; ++e) {
      auto &src = sources[e];
      auto &f = src.frame;
      const uint32_t sw = f.sourceWidth > 0 ? f.sourceWidth : f.width;
      const uint32_t sh = f.sourceHeight > 0 ? f.sourceHeight : f.height;
      const bool scaled=upscaler && (sw < w * (border ? fov : 1) || sh < h * (border ? fov : 1));
      usesSgsr |= scaled;

    }
    if(!pixels && layers==2) {
      vrPerformance.effects(fxaa,usesSgsr,fxaa,upscaler!=0);
      const auto &f=sources[0].frame;
      vrPerformance.resolution(f.sourceWidth>0?f.sourceWidth:f.width,
                               f.sourceHeight>0?f.sourceHeight:f.height,w,h);
    }
    if(measure) {
      vkCmdWriteTimestamp(s.cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,s.timing,0);
      s.timedEffects=(fxaa?1u:0u)|(usesSgsr?2u:0u);
    }
    bool maskReady=false;
    if(layers==2 && !pixels && !maskVertices.empty()) {
      const size_t bytes=maskVertices.size()*sizeof(XrVector2f);
      if(s.maskCapacity<bytes) {
        s.maskBuffer=lsfg::Buffer(deviceInfo,bytes,VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        s.maskCapacity=s.maskBuffer.Valid()?bytes:0;
      }
      maskReady=s.maskBuffer.Valid();
      if(maskReady && s.maskGeneration!=maskGeneration) {
        s.maskBuffer.Upload(maskVertices.data(),bytes);s.maskGeneration=maskGeneration;
      }
    }
    for (uint32_t e = 0; e < layers && !kXrSwapchainPattern; ++e) {
      auto &src = sources[e];
      VkDescriptorImageInfo infos[] = {
          {sampler, src.view, VK_IMAGE_LAYOUT_GENERAL},
          {sampler, pixels ? s.overlay->View() : src.view,
           VK_IMAGE_LAYOUT_GENERAL}};
      VkWriteDescriptorSet writes[2]{};
      for (int i = 0; i < 2; ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = s.sets[e];
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
      }
      vkUpdateDescriptorSets(ctx->device, 2, writes, 0, nullptr);
      VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
      rp.renderPass = pass;
      rp.framebuffer = s.targets[e];
      rp.renderArea = {{0, 0}, {w, h}};
      const float tint=ffrDebug==3 && maskReady && maskCounts[e] ? 1.f : 0.f;
      const VkClearValue black{{{tint,0.f,tint,1.f}}};
      rp.clearValueCount=1;rp.pClearValues=&black;
      vkCmdBeginRenderPass(s.cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
      VkViewport vp{0, 0, float(w), float(h), 0, 1};
      VkRect2D sc{{0, 0}, {w, h}};
      vkCmdSetViewport(s.cmd, 0, 1, &vp);
      vkCmdSetScissor(s.cmd, 0, 1, &sc);
      const auto &eye = src.frame;
      const std::array<int,6> key{eye.width, eye.height, eye.sourceX, eye.sourceY,
          eye.sourceWidth > 0 ? eye.sourceWidth : eye.width,
          eye.sourceHeight > 0 ? eye.sourceHeight : eye.height};
      if(key != filterKeys[e]) {
        filterKeys[e] = key;
        filterCentral[e] = centralFilterEligible(key[0],key[1],key[2],key[3],key[4],key[5]);
      }
      const bool central = eye.foveatedPacked && filterCentral[e];
      vkCmdBindPipeline(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        (maskReady && maskCounts[e] ? maskedPipelines : pipelines)[central ? 2 : eye.foveatedPacked ? 1 : 0]);
      vkCmdBindDescriptorSets(s.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0,
                              1, &s.sets[e], 0, nullptr);
      const auto &f = src.frame;
      float iw = std::max(1, f.width), ih = std::max(1, f.height),
            sw = f.sourceWidth > 0 ? f.sourceWidth : iw,
            sh = f.sourceHeight > 0 ? f.sourceHeight : ih;
      struct Push {
        float crop[4], viewport[4], bounds[4], control[4], flags[4], content[4], fxaaPatch[4], packing[4];
      };
      Push push{
          {f.sourceX / iw, (f.sourceY + (f.flipY ? sh : 0)) / ih, sw / iw,
           (f.flipY ? -sh : sh) / ih},
          {1 / iw, 1 / ih, iw, ih},
          {float(f.sourceX), float(f.sourceY), f.sourceX + sw - 1,
           f.sourceY + sh - 1},
          {sharpness, fov, float(border),
           (format == VK_FORMAT_R8G8B8A8_SRGB ||
            format == VK_FORMAT_B8G8R8A8_SRGB)
               ? 1.f
               : 0.f},
          {upscaler &&
                   (sw < w * (border ? fov : 1) || sh < h * (border ? fov : 1))
               ? float(upscaler)
               : 0.f,
           f.swapRedBlue ? 1.f : 0.f, pixels ? 1.f : 0.f, guest ? 1.f : 0.f},
          {sx, sy, fxaa && !pixels ? 1.f : 0.f, 0.f},
          {float(ffrDebug==3?0:ffrDebug), 0, 0, 0}, {}};
      gn_packing_parameters(uint32_t(iw), uint32_t(ih), push.packing);
      static_assert(sizeof(Push) == 128);
      vkCmdPushConstants(s.cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(push), &push);
      if(maskReady && maskCounts[e]) {
        const VkBuffer buffer=s.maskBuffer.Handle();const VkDeviceSize offset=0;
        bindVertices(s.cmd,0,1,&buffer,&offset);
        vkCmdDraw(s.cmd,maskCounts[e],1,maskOffsets[e],0);
      } else vkCmdDraw(s.cmd, 3, 1, 0, 0);
      vkCmdEndRenderPass(s.cmd);
    }
    if(measure) vkCmdWriteTimestamp(s.cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,s.timing,1);
    // Release only after the final source read, with no ownership barrier
    // separating the independent eye draws.
    for (uint32_t e = 0; e < layers && !kXrSwapchainPattern; ++e) {
      auto &src = sources[e];
      if (src.external) {
        auto b = imageBarrier(src.image, VK_IMAGE_LAYOUT_GENERAL,
                         VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT, 0,
                         ctx->family, src.owner);
        b.subresourceRange.baseArrayLayer = guest ? src.frame.bufferLayer : 0;
        vkCmdPipelineBarrier(s.cmd, sourceStages,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
      }
    }
    if (vkEndCommandBuffer(s.cmd) != VK_SUCCESS) {
      release();
      return false;
    }
    vkResetFences(ctx->device, 1, &s.fence);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = count;
    si.pWaitSemaphores = waits;
    si.pWaitDstStageMask = masks;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &s.done;
    if (ctx->submit(1, &si, s.fence) != VK_SUCCESS) {
      initialized = false;
      release();
      return false;
    }
    s.timingPending=measure;
    VkSemaphoreGetFdInfoKHR ex{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
    ex.semaphore = s.done;
    ex.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
    if (exportFd(ctx->device, &ex, &fd) != VK_SUCCESS) {
      initialized = false;
      ctx->idle();
      release();
      return false;
    }
    const bool released = release();
#ifdef GN_XR_SWAPCHAIN_PATTERN
    if (!patternReported) {
      __android_log_print(ANDROID_LOG_INFO, "VrVulkanPattern",
                          "First pattern submitted: image=%u layers=%u release=%d",
                          index, layers, released);
      patternReported = true;
    }
#endif
    return released;
  }
};
void XrVulkanPresenter::setVisibilityMasks(const std::array<VisibilityMesh,2>& masks,const std::array<XrFovf,2>& fovs,uint64_t revision) {
  if(p_->maskRevision==revision && !memcmp(p_->maskFovs.data(),fovs.data(),sizeof(fovs))) return;
  p_->maskRevision=revision;p_->maskFovs=fovs;++p_->maskGeneration;
  p_->maskVertices.clear();
  for(int e=0;e<2;++e) {
    p_->maskOffsets[e]=p_->maskVertices.size();
    projectVisibility(masks[e],fovs[e],p_->maskVertices);
    p_->maskCounts[e]=p_->maskVertices.size()-p_->maskOffsets[e];
  }
}
XrVulkanPresenter::XrVulkanPresenter() : p_(std::make_unique<Impl>()) {}
XrVulkanPresenter::~XrVulkanPresenter() = default;
bool XrVulkanPresenter::initialize(XrVulkanContext *c, XrSwapchain s,
                                   VkFormat f, uint32_t w, uint32_t h,
                                   uint32_t layers, int up, float sharp,
                                   float scale, int border, bool fxaa, int ffrDebug,
                                   bool subsampled) {
  p_->ctx = c;
  p_->swapchain = s;
  p_->format = f;
  p_->w = w;
  p_->h = h;
  p_->layers = layers;
  p_->upscaler = up;
  p_->sharpness = sharp;
  p_->fov = scale;
  p_->border = border;
  p_->fxaa = fxaa;
  p_->subsampled = subsampled;
  p_->ffrDebug = ffrDebug;
  return p_->initialize();
}
bool XrVulkanPresenter::render(std::array<Source, 2> s, int &fd) {
  return p_->render(s, fd, false);
}
bool XrVulkanPresenter::renderGuest(
    const std::array<windowsvr::EyeFrame, 2> &frames, int &fd) {
  std::array<Source, 2> s{};
  for (int e = 0; e < 2; ++e)
    s[e].frame = frames[e];
  return p_->render(s, fd, true);
}
bool XrVulkanPresenter::renderQuad(AHardwareBuffer *game,
                                   const std::vector<uint8_t> &pixels, int w,
                                   int h, uint64_t version, float sx,
                                   float sy) {
  std::array<Source, 2> s{};
  if (game) {
    AHardwareBuffer_Desc d{};
    AHardwareBuffer_describe(game, &d);
    for (int e = 0; e < 2; ++e) {
      auto &f = s[e].frame;
      f.buffer = game;
      f.kind = windowsvr::BufferKind::HardwareBuffer;
      f.width = d.width;
      f.height = d.height;
      f.registrationSerial = reinterpret_cast<uintptr_t>(game);
      f.imageIndex = 0;
      s[e].owner = VK_QUEUE_FAMILY_FOREIGN_EXT;
    }
  }
  int fd = -1;
  bool ok = p_->render(s, fd, game != nullptr, &pixels, w, h, version, sx, sy);
  if (fd >= 0)
    close(fd);
  return ok;
}
void XrVulkanPresenter::shutdown() {
  p_.reset();
  p_ = std::make_unique<Impl>();
}
} // namespace xrimmersive
