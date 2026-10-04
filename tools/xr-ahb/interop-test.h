#pragma once
#define VK_USE_PLATFORM_ANDROID_KHR
#include <android/hardware_buffer.h>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <initializer_list>
#include <vulkan/vulkan.h>
struct ProbeHalDevice;
struct ProbeHalModule;
struct ProbeHalMethods {
  int (*open)(const ProbeHalModule *, const char *, ProbeHalDevice **);
};
struct ProbeHalModule {
  uint32_t tag;
  uint16_t moduleVersion, halVersion;
  const char *id, *name, *author;
  ProbeHalMethods *methods;
  void *dso;
  uint64_t reserved[25];
};
struct ProbeHalDevice {
  uint32_t tag, version;
  ProbeHalModule *module;
  uint64_t reserved[12];
  int (*close)(ProbeHalDevice *);
  PFN_vkEnumerateInstanceExtensionProperties enumerate;
  PFN_vkCreateInstance create;
  PFN_vkGetInstanceProcAddr get;
};

// Standalone, serial, cross-driver pixel test. No game or OpenXR state is used.
// Each context dispatches through its own library; never mix driver handles.
#include "interop-pattern.h"
#include "interop-readback.h"
#include "interop-vertex.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <vector>
#define CHECK(expr)                                                            \
  do {                                                                         \
    VkResult result = (expr);                                                  \
    if (result) {                                                              \
      fprintf(stderr, "%s: %d\n", #expr, result);                              \
      exit(1);                                                                 \
    }                                                                          \
  } while (0)
#ifndef GN_TEST_WIDTH
#define GN_TEST_WIDTH 512
#define GN_TEST_HEIGHT 512
#endif
constexpr unsigned W = GN_TEST_WIDTH, H = GN_TEST_HEIGHT;
constexpr VkFormat FORMAT = VK_FORMAT_R8G8B8A8_UNORM;
struct Context {
  void *library{};
  PFN_vkGetInstanceProcAddr get{};
  PFN_vkGetDeviceProcAddr gd{};
  VkInstance instance{};
  VkPhysicalDevice physical{};
  VkDevice device{};
  VkQueue queue{};
  uint32_t family{};
  VkPhysicalDeviceMemoryProperties memory{};
  VkCommandPool pool{};
  template <class T> T instanceProc(const char *n) {
    auto p = get(instance, n);
    if (!p) {
      fprintf(stderr, "missing %s\n", n);
      exit(1);
    }
    return (T)p;
  }
  template <class T> T proc(const char *n) {
    auto p = gd(device, n);
    if (!p) {
      fprintf(stderr, "missing %s\n", n);
      exit(1);
    }
    return (T)p;
  }
  uint32_t type(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memory.memoryTypeCount; i++)
      if ((bits & (1u << i)) &&
          (memory.memoryTypes[i].propertyFlags & flags) == flags)
        return i;
    fprintf(stderr, "No compatible memory type\n");
    exit(1);
  }
  explicit Context(const char *path, bool formatLists = false) {
    library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!library) {
      puts(dlerror());
      exit(1);
    }
    get = (PFN_vkGetInstanceProcAddr)dlsym(library, "vkGetInstanceProcAddr");
    if (!get)
      get = (PFN_vkGetInstanceProcAddr)dlsym(library,
                                             "vk_icdGetInstanceProcAddr");
    if (!get) {
      auto hal = (ProbeHalModule *)dlsym(library, "HMI");
      ProbeHalDevice *d = nullptr;
      if (hal && hal->methods && !hal->methods->open(hal, "vk0", &d))
        get = d->get;
    }
    if (!get)
      exit(1);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    CHECK(((PFN_vkCreateInstance)get(nullptr, "vkCreateInstance"))(&ci, nullptr,
                                                                   &instance));
#define I(name) instanceProc<PFN_vk##name>("vk" #name)
    uint32_t n = 1;
    CHECK(I(EnumeratePhysicalDevices)(instance, &n, &physical));
    VkPhysicalDeviceProperties props{};
    I(GetPhysicalDeviceProperties)(physical, &props);
    printf("Device: %s api=%x driver=%x\n", props.deviceName, props.apiVersion,
           props.driverVersion);
    I(GetPhysicalDeviceMemoryProperties)(physical, &memory);
    n = 0;
    I(GetPhysicalDeviceQueueFamilyProperties)(physical, &n, nullptr);
    std::vector<VkQueueFamilyProperties> q(n);
    I(GetPhysicalDeviceQueueFamilyProperties)(physical, &n, q.data());
    while (family < n && !(q[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
      family++;
    if (family == n)
      exit(1);
    float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    const char *ext[] = {
        VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME,
        VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME,
        VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
        VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
        VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME};
    VkPhysicalDeviceFragmentDensityMapFeaturesEXT df{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT};
    df.fragmentDensityMap = df.fragmentDensityMapNonSubsampledImages = VK_TRUE;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    di.pNext = &df;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = sizeof(ext) / sizeof(*ext) - (formatLists ? 0 : 1);
    di.ppEnabledExtensionNames = ext;
    CHECK(I(CreateDevice)(physical, &di, nullptr, &device));
    gd = I(GetDeviceProcAddr);
#undef I
#define V(name) c.proc<PFN_vk##name>("vk" #name)
    auto &c = *this;
    V(GetDeviceQueue)(device, family, 0, &queue);
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.queueFamilyIndex = family;
    CHECK(V(CreateCommandPool)(device, &pc, nullptr, &pool));
  }
  VkCommandBuffer begin() {
    auto &c = *this;
    VkCommandBufferAllocateInfo a{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    a.commandPool = pool;
    a.commandBufferCount = 1;
    VkCommandBuffer cmd{};
    CHECK(V(AllocateCommandBuffers)(device, &a, &cmd));
    VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    CHECK(V(BeginCommandBuffer)(cmd, &b));
    return cmd;
  }
  void finish(VkCommandBuffer cmd) {
    auto &c = *this;
    CHECK(V(EndCommandBuffer)(cmd));
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence{};
    CHECK(V(CreateFence)(device, &fi, nullptr, &fence));
    VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    s.commandBufferCount = 1;
    s.pCommandBuffers = &cmd;
    CHECK(V(QueueSubmit)(queue, 1, &s, fence));
    CHECK(V(WaitForFences)(device, 1, &fence, VK_TRUE, 10000000000ull));
    V(DestroyFence)(device, fence, nullptr);
    V(FreeCommandBuffers)(device, pool, 1, &cmd);
  }
  ~Context() {
    auto &c = *this;
    V(DestroyCommandPool)(device, pool, nullptr);
    V(DestroyDevice)(device, nullptr);
    instanceProc<PFN_vkDestroyInstance>("vkDestroyInstance")(
        instance, nullptr); /* HAL/library retained until process exit. */
  }
};
struct Image {
  Context &c;
  VkFormat format;
  VkImage image{};
  VkDeviceMemory memory{};
  VkImageView view{};
  Image(Context &c, VkFormat format, unsigned w, unsigned h,
        VkImageUsageFlags usage, bool sub = false, bool external = false,
        AHardwareBuffer *import = nullptr, bool mutableFormat = false)
      : c(c), format(format) {
    VkExternalMemoryImageCreateInfo ext{
        VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    ext.handleTypes =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    const VkFormat viewFormats[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
    VkImageFormatListCreateInfo list{VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO};
    list.viewFormatCount = 2; list.pViewFormats = viewFormats;
    ext.pNext = mutableFormat ? &list : nullptr;
    ci.pNext = external ? static_cast<const void*>(&ext) : (mutableFormat ? &list : nullptr);
    ci.flags = (sub ? VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT : 0) | (mutableFormat ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0);
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = usage;
    CHECK(V(CreateImage)(c.device, &ci, nullptr, &image));
    VkMemoryDedicatedAllocateInfo dedicated{
        VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = image;
    VkExportMemoryAllocateInfo exp{
        VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    exp.pNext = &dedicated;
    exp.handleTypes = ext.handleTypes;
    VkImportAndroidHardwareBufferInfoANDROID imp{
        VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
    imp.pNext = &dedicated;
    imp.buffer = import;
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    if (import) {
      VkAndroidHardwareBufferFormatPropertiesANDROID fmt{
          VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
      VkAndroidHardwareBufferPropertiesANDROID p{
          VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
      p.pNext = &fmt;
      CHECK(V(GetAndroidHardwareBufferPropertiesANDROID)(c.device, import, &p));
      printf("Import format=%d bytes=%llu bits=%x\n", fmt.format,
             (unsigned long long)p.allocationSize, p.memoryTypeBits);
      if (fmt.format != format) {
        fprintf(stderr, "AHB format mismatch\n");
        exit(1);
      }
      ma.pNext = &imp;
      ma.allocationSize = p.allocationSize;
      ma.memoryTypeIndex = c.type(p.memoryTypeBits, 0);
    } else if (external) {
      ma.pNext = &exp;
      ma.allocationSize = 0;
      ma.memoryTypeIndex = 0;
    } else {
      VkMemoryRequirements r{};
      V(GetImageMemoryRequirements)(c.device, image, &r);
      ma.allocationSize = r.size;
      ma.memoryTypeIndex =
          c.type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    CHECK(V(AllocateMemory)(c.device, &ma, nullptr, &memory));
    CHECK(V(BindImageMemory)(c.device, image, memory, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    CHECK(V(CreateImageView)(c.device, &vi, nullptr, &view));
  }
  ~Image() {
    V(DestroyImageView)(c.device, view, nullptr);
    V(DestroyImage)(c.device, image, nullptr);
    V(FreeMemory)(c.device, memory, nullptr);
  }
};
void barrier(Context &c, VkCommandBuffer cmd, VkImage image,
             VkImageLayout oldLayout, VkImageLayout newLayout,
             VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags from,
             VkPipelineStageFlags to,
             uint32_t srcFamily = VK_QUEUE_FAMILY_IGNORED,
             uint32_t dstFamily = VK_QUEUE_FAMILY_IGNORED) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.image = image;
  b.oldLayout = oldLayout;
  b.newLayout = newLayout;
  b.srcAccessMask = src;
  b.dstAccessMask = dst;
  b.srcQueueFamilyIndex = srcFamily;
  b.dstQueueFamilyIndex = dstFamily;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  V(CmdPipelineBarrier)(cmd, from, to, 0, 0, nullptr, 0, nullptr, 1, &b);
}
struct Draw {
  Context &c;
  VkRenderPass pass{};
  VkFramebuffer fb{};
  VkPipelineLayout layout{};
  VkPipeline pipeline{};
  VkSampler sampler{};
  VkDescriptorSetLayout sl{};
  VkDescriptorPool dp{};
  VkDescriptorSet set{};
  Draw(Context &c, Image &target, Image *density, Image *input, bool subsampled,
       VkImageLayout initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
       const uint32_t *fragment = nullptr, size_t fragmentBytes = 0, bool central = false)
      : c(c) {
    if (input) {
      VkSamplerCreateInfo s{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
      s.flags = subsampled ? VK_SAMPLER_CREATE_SUBSAMPLED_BIT_EXT : 0;
      s.minFilter = s.magFilter = VK_FILTER_LINEAR;
      s.addressModeU = s.addressModeV = s.addressModeW =
          VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      CHECK(V(CreateSampler)(c.device, &s, nullptr, &sampler));
      VkDescriptorSetLayoutBinding b{0,
                                     VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                     1, VK_SHADER_STAGE_FRAGMENT_BIT, &sampler};
      VkDescriptorSetLayoutCreateInfo l{
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
      VkDescriptorSetLayoutBinding bindings[] = {b, b};
      bindings[1].binding = 1;
      l.bindingCount = fragment ? 2 : 1;
      l.pBindings = bindings;
      CHECK(V(CreateDescriptorSetLayout)(c.device, &l, nullptr, &sl));
      VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                              fragment ? 2u : 1u};
      VkDescriptorPoolCreateInfo p{
          VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
      p.maxSets = p.poolSizeCount = 1;
      p.pPoolSizes = &ps;
      CHECK(V(CreateDescriptorPool)(c.device, &p, nullptr, &dp));
      VkDescriptorSetAllocateInfo a{
          VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
      a.descriptorPool = dp;
      a.descriptorSetCount = 1;
      a.pSetLayouts = &sl;
      CHECK(V(AllocateDescriptorSets)(c.device, &a, &set));
      VkDescriptorImageInfo ii{sampler, input->view,
                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      wr.dstSet = set;
      wr.descriptorCount = 1;
      wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      wr.pImageInfo = &ii;
      V(UpdateDescriptorSets)(c.device, 1, &wr, 0, nullptr);
      if (fragment) {
        wr.dstBinding = 1;
        V(UpdateDescriptorSets)(c.device, 1, &wr, 0, nullptr);
      }
    }
    VkPipelineLayoutCreateInfo pl{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = input ? 1 : 0;
    pl.pSetLayouts = &sl;
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
    if (fragment) {
      pl.pushConstantRangeCount = 1;
      pl.pPushConstantRanges = &push;
    }
    CHECK(V(CreatePipelineLayout)(c.device, &pl, nullptr, &layout));
    VkAttachmentDescription attachments[2]{};
    auto &a = attachments[0];
    a.format = target.format;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.initialLayout = initialLayout;
    a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    auto &d = attachments[1];
    d.format = VK_FORMAT_R8G8_UNORM;
    d.samples = VK_SAMPLE_COUNT_1_BIT;
    d.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    d.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    d.initialLayout = d.finalLayout =
        VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
    VkRenderPassFragmentDensityMapCreateInfoEXT dm{
        VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT};
    dm.fragmentDensityMapAttachment = {
        1, VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT};
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &color;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.pNext = density ? &dm : nullptr;
    rp.attachmentCount = density ? 2 : 1;
    rp.pAttachments = attachments;
    rp.subpassCount = 1;
    rp.pSubpasses = &sp;
    CHECK(V(CreateRenderPass)(c.device, &rp, nullptr, &pass));
    VkImageView views[] = {target.view,
                           density ? density->view : VK_NULL_HANDLE};
    VkFramebufferCreateInfo f{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    f.renderPass = pass;
    f.attachmentCount = density ? 2 : 1;
    f.pAttachments = views;
    f.width = W;
    f.height = H;
    f.layers = 1;
    CHECK(V(CreateFramebuffer)(c.device, &f, nullptr, &fb));
    auto shader = [&](const uint32_t *code, size_t size) {
      VkShaderModuleCreateInfo s{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
      s.pCode = code;
      s.codeSize = size;
      VkShaderModule m{};
      CHECK(V(CreateShaderModule)(c.device, &s, nullptr, &m));
      return m;
    };
    auto vs = shader(interopVertex, sizeof(interopVertex));
    auto fs = fragment ? shader(fragment, fragmentBytes)
              : input  ? shader(interopReadback, sizeof(interopReadback))
                       : shader(interopPattern, sizeof(interopPattern));
    VkPipelineShaderStageCreateInfo stages[] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
         VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr}};
    VkPipelineVertexInputStateCreateInfo vi{
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport viewport{0, 0, float(W), float(H), 0, 1};
    VkRect2D area{{0, 0}, {W, H}};
    VkPipelineViewportStateCreateInfo vp{
        VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    vp.pViewports = &viewport;
    vp.pScissors = &area;
    VkPipelineRasterizationStateCreateInfo rs{
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState ba{};
    ba.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo bs{
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    bs.attachmentCount = 1;
    bs.pAttachments = &ba;
    VkGraphicsPipelineCreateInfo gp{
        VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    const VkBool32 values[] = {VK_TRUE, central ? VK_TRUE : VK_FALSE};
    const VkSpecializationMapEntry entries[] = {{1, 0, sizeof(VkBool32)}, {2, sizeof(VkBool32), sizeof(VkBool32)}};
    const VkSpecializationInfo packedSpec{2, entries, sizeof(values), values};
    if (fragment) stages[1].pSpecializationInfo = &packedSpec;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &bs;
    gp.layout = layout;
    gp.renderPass = pass;
    CHECK(V(CreateGraphicsPipelines)(c.device, VK_NULL_HANDLE, 1, &gp, nullptr,
                                     &pipeline));
    V(DestroyShaderModule)(c.device, vs, nullptr);
    V(DestroyShaderModule)(c.device, fs, nullptr);
  }
  void record(VkCommandBuffer cmd) {
    VkRenderPassBeginInfo r{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    r.renderPass = pass;
    r.framebuffer = fb;
    r.renderArea = {{0, 0}, {W, H}};
    V(CmdBeginRenderPass)(cmd, &r, VK_SUBPASS_CONTENTS_INLINE);
    V(CmdBindPipeline)(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (set)
      V(CmdBindDescriptorSets)
    (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, nullptr);
    V(CmdDraw)(cmd, 3, 1, 0, 0);
    V(CmdEndRenderPass)(cmd);
  }
  ~Draw() {
    V(DestroyPipeline)(c.device, pipeline, nullptr);
    V(DestroyFramebuffer)(c.device, fb, nullptr);
    V(DestroyRenderPass)(c.device, pass, nullptr);
    V(DestroyPipelineLayout)(c.device, layout, nullptr);
    if (dp)
      V(DestroyDescriptorPool)(c.device, dp, nullptr);
    if (sl)
      V(DestroyDescriptorSetLayout)(c.device, sl, nullptr);
    if (sampler)
      V(DestroySampler)(c.device, sampler, nullptr);
  }
};
