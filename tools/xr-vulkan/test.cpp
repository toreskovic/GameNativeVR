// Offscreen execution of the exact presentation SPIR-V. No OpenXR runtime
// needed.
#include "../../app/src/main/cpp/lsfg/lsfg_common.hpp"
#include "../../app/src/main/cpp/xrimmersive/xr_vulkan_present_spv.h"
#include "../../app/src/main/cpp/xrimmersive/xr_vulkan_diagnostic.h"
#include "../../app/src/main/cpp/xrimmersive/xr_present_foveation.h"
#ifdef FDM_SMOKE
#include "../../app/src/main/cpp/xrimmersive/xr_vulkan_dispatch.h"
#endif
#include "../../app/src/main/cpp/vrffr/selection.h"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <iostream>
#include <vector>
#define CHECK(x)                                                               \
  do {                                                                         \
    auto result = (x);                                                         \
    if (result != VK_SUCCESS) {                                                \
      std::cerr << #x << " failed " << result << '\n';                         \
      std::abort();                                                            \
    }                                                                          \
  } while (0)
using namespace xrimmersive;
#ifdef SUBSAMPLED_SMOKE
#include "subsampled-readback-test.h"
#ifndef FDM_SMOKE
#error SUBSAMPLED_SMOKE requires FDM_SMOKE
#endif
#endif
int main() {
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ii{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ii.pApplicationInfo = &app;
  VkInstance instance;
  CHECK(vkCreateInstance(&ii, nullptr, &instance));
  uint32_t n = 0;
  CHECK(vkEnumeratePhysicalDevices(instance, &n, nullptr));
  assert(n);
  std::vector<VkPhysicalDevice> devices(n);
  CHECK(vkEnumeratePhysicalDevices(instance, &n, devices.data()));
  auto gpu = devices[0];
  vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, nullptr);
  std::vector<VkQueueFamilyProperties> qs(n);
  vkGetPhysicalDeviceQueueFamilyProperties(gpu, &n, qs.data());
  uint32_t family = 0;
  while (family < n && !(qs[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
    ++family;
  assert(family < n);
  float priority = 1;
  VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qi.queueFamilyIndex = family;
  qi.queueCount = 1;
  qi.pQueuePriorities = &priority;
  VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  di.queueCreateInfoCount = 1;
  di.pQueueCreateInfos = &qi;
#ifdef FDM_SMOKE
  VkPhysicalDeviceFragmentDensityMapFeaturesEXT densityFeatures{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT};
  VkPhysicalDeviceFeatures2 densityQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};densityQuery.pNext=&densityFeatures;
  vkGetPhysicalDeviceFeatures2(gpu,&densityQuery);
  assert(densityFeatures.fragmentDensityMap && densityFeatures.fragmentDensityMapNonSubsampledImages);
  densityFeatures.fragmentDensityMapDynamic=VK_FALSE;
  const char* extension=VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME;
  di.pNext=&densityFeatures;di.enabledExtensionCount=1;di.ppEnabledExtensionNames=&extension;
#endif
  VkDevice device;
  CHECK(vkCreateDevice(gpu, &di, nullptr, &device));
  VkQueue queue;
  vkGetDeviceQueue(device, family, 0, &queue);
  assert(vkd_load(instance, device, vkGetInstanceProcAddr));
#ifdef FDM_SMOKE
  loadXrVulkanDispatch(instance,device,vkGetInstanceProcAddr);
#endif
  lsfg::Device alloc(device, gpu);
  {
#ifdef FDM_SMOKE
    constexpr unsigned w=512,h=512,bytes=w*h*4;
#else
    constexpr unsigned w = 64, h = 48, bytes = w * h * 4;
#endif
#ifdef FDM_SMOKE
    PresentationDensityMap densityMap;
    assert(densityMap.initialize(alloc,queue,family,{w,h},{16,16},1.f));
#endif
    lsfg::LsfgImage input(alloc, {w, h}, VK_FORMAT_R8G8B8A8_UNORM);
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size = bytes * 2;
    bc.usage =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer staging;
    CHECK(vkCreateBuffer(device, &bc, nullptr, &staging));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, staging, &req);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = req.size;
    ma.memoryTypeIndex = alloc.FindMemoryType(
        req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory memory;
    CHECK(vkAllocateMemory(device, &ma, nullptr, &memory));
    CHECK(vkBindBufferMemory(device, staging, memory, 0));
    void *mapped;
    CHECK(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    auto pixels = static_cast<unsigned char *>(mapped);
    for (unsigned y = 0; y < h; ++y)
      for (unsigned x = 0; x < w; ++x) {
        auto p = pixels + (y * w + x) * 4;
        p[0] = 32 + x * 3;
        p[1] = 24 + y * 4;
        p[2] = 100;
        p[3] = 255;
      }
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.queueFamilyIndex = family;
    pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(device, &pc, nullptr, &pool));
    VkCommandBufferAllocateInfo ca{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ca.commandPool = pool;
    ca.commandBufferCount = 1;
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(device, &ca, &cmd));
    VkSamplerCreateInfo sm{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sm.minFilter = sm.magFilter = VK_FILTER_LINEAR;
    sm.addressModeU = sm.addressModeV = sm.addressModeW =
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler;
    CHECK(vkCreateSampler(device, &sm, nullptr, &sampler));
    VkDescriptorSetLayoutBinding bindings[] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
         VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo sl{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.bindingCount = 2;
    sl.pBindings = bindings;
    VkDescriptorSetLayout setLayout;
    CHECK(vkCreateDescriptorSetLayout(device, &sl, nullptr, &setLayout));
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo dp{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 1;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &size;
    VkDescriptorPool descriptors;
    CHECK(vkCreateDescriptorPool(device, &dp, nullptr, &descriptors));
    VkDescriptorSetAllocateInfo da{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    da.descriptorPool = descriptors;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &setLayout;
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(device, &da, &set));
    VkDescriptorImageInfo sampled{sampler, input.View(),
                                  VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo filtered=sampled;
    VkWriteDescriptorSet writes[2]{};
    for (int i = 0; i < 2; ++i) {
      writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[i].dstSet = set;
      writes[i].dstBinding = i;
      writes[i].descriptorCount = 1;
      writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      writes[i].pImageInfo = i ? &filtered : &sampled;
    }
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
    struct Push {
      float crop[4], viewport[4], bounds[4], control[4], flags[4], content[4], fxaaPatch[4], packing[4];
    };
    VkPushConstantRange range{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pl{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &setLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    VkPipelineLayout layout;
    CHECK(vkCreatePipelineLayout(device, &pl, nullptr, &layout));
    for (auto format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB}) {
      VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      image.imageType = VK_IMAGE_TYPE_2D;
      image.format = format;
      image.extent = {w, h, 1};
      image.mipLevels = image.arrayLayers = 1;
      image.samples = VK_SAMPLE_COUNT_1_BIT;
      image.usage =
          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
#ifdef SUBSAMPLED_SMOKE
      image.flags=VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT;
      image.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
#endif
      VkImage target;
      CHECK(vkCreateImage(device, &image, nullptr, &target));
      vkGetImageMemoryRequirements(device, target, &req);
      ma.allocationSize = req.size;
      ma.memoryTypeIndex = alloc.FindMemoryType(
          req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      VkDeviceMemory targetMemory;
      CHECK(vkAllocateMemory(device, &ma, nullptr, &targetMemory));
      CHECK(vkBindImageMemory(device, target, targetMemory, 0));
      VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
      vi.image = target;
      vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vi.format = format;
      vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      VkImageView view;
      CHECK(vkCreateImageView(device, &vi, nullptr, &view));
      VkAttachmentDescription attachment{};
      attachment.format = format;
      attachment.samples = VK_SAMPLE_COUNT_1_BIT;
      attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription sub{};
      sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      sub.colorAttachmentCount = 1;
      sub.pColorAttachments = &color;
      VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
      rp.attachmentCount = 1;
      rp.pAttachments = &attachment;
      rp.subpassCount = 1;
      rp.pSubpasses = &sub;
#ifdef FDM_SMOKE
      VkAttachmentDescription densityAttachments[2]={attachment,{}};
      auto& d=densityAttachments[1];d.format=VK_FORMAT_R8G8_UNORM;d.samples=VK_SAMPLE_COUNT_1_BIT;
      d.loadOp=VK_ATTACHMENT_LOAD_OP_LOAD;d.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
      d.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;d.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
      d.initialLayout=d.finalLayout=VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
      VkRenderPassFragmentDensityMapCreateInfoEXT dm{VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT};
      dm.fragmentDensityMapAttachment={1,VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT};
      rp.pNext=&dm;rp.attachmentCount=2;rp.pAttachments=densityAttachments;
#endif
      VkRenderPass pass;
      CHECK(vkCreateRenderPass(device, &rp, nullptr, &pass));
      VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
      fb.renderPass = pass;
      fb.attachmentCount = 1;
      fb.pAttachments = &view;
      fb.width = w;
      fb.height = h;
      fb.layers = 1;
#ifdef FDM_SMOKE
      VkImageView densityViews[]={view,densityMap.view()};fb.attachmentCount=2;fb.pAttachments=densityViews;
#endif
      VkFramebuffer framebuffer;
      CHECK(vkCreateFramebuffer(device, &fb, nullptr, &framebuffer));
      for (bool edge : {false, true}) {
        auto module = [&](const uint32_t *code, size_t bytes) {
          VkShaderModuleCreateInfo ci{
              VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
          ci.pCode = code;
          ci.codeSize = bytes;
          VkShaderModule m;
          CHECK(vkCreateShaderModule(device, &ci, nullptr, &m));
          return m;
        };
#ifdef VISIBILITY_SMOKE
        auto vertex=module(presentMaskedVertex,sizeof(presentMaskedVertex));
        // Two triangles covering the inner rectangle. Hidden pixels must clear
        // black, and interpolation/filter coordinates inside must stay exact.
        const float maskUvs[]={.25f,.25f,.75f,.25f,.75f,.75f,.25f,.25f,.75f,.75f,.25f,.75f};
        lsfg::Buffer maskBuffer(alloc,sizeof(maskUvs),VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        assert(maskBuffer.Valid());maskBuffer.Upload(maskUvs,sizeof(maskUvs));
#else
        auto vertex = module(presentVertex, sizeof(presentVertex));
#endif
        auto fragment = edge ? module(presentEdge, sizeof(presentEdge))
                             : module(presentBasic, sizeof(presentBasic));
        VkPipelineShaderStageCreateInfo stages[] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
             VK_SHADER_STAGE_VERTEX_BIT, vertex, "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
             VK_SHADER_STAGE_FRAGMENT_BIT, fragment, "main", nullptr}};
        VkPipelineVertexInputStateCreateInfo vert{
            VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
#ifdef VISIBILITY_SMOKE
        const VkVertexInputBindingDescription binding{0,2*sizeof(float),VK_VERTEX_INPUT_RATE_VERTEX};
        const VkVertexInputAttributeDescription attribute{0,0,VK_FORMAT_R32G32_SFLOAT,0};
        vert.vertexBindingDescriptionCount=1;vert.pVertexBindingDescriptions=&binding;
        vert.vertexAttributeDescriptionCount=1;vert.pVertexAttributeDescriptions=&attribute;
#endif
        VkPipelineInputAssemblyStateCreateInfo ia{
            VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkViewport viewport{0, 0, float(w), float(h), 0, 1};
        VkRect2D scissor{{0, 0}, {w, h}};
        VkPipelineViewportStateCreateInfo vp{
            VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        vp.pViewports = &viewport;
        vp.pScissors = &scissor;
        VkPipelineRasterizationStateCreateInfo rs{
            VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.lineWidth = 1;
        VkPipelineMultisampleStateCreateInfo ms{
            VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState ba{};
        ba.colorWriteMask = 15;
        VkPipelineColorBlendStateCreateInfo blend{
            VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &ba;
        VkGraphicsPipelineCreateInfo gp{
            VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        gp.stageCount = 2;
        gp.pStages = stages;
        gp.pVertexInputState = &vert;
        gp.pInputAssemblyState = &ia;
        gp.pViewportState = &vp;
        gp.pRasterizationState = &rs;
        gp.pMultisampleState = &ms;
        gp.pColorBlendState = &blend;
        gp.layout = layout;
        gp.renderPass = pass;
        VkPipeline pipeline;
        CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, nullptr,
                                        &pipeline));
        const VkBool32 disabled = VK_FALSE;
        const VkSpecializationMapEntry entry{0,0,sizeof(disabled)};
        const VkSpecializationInfo specialization{1,&entry,sizeof(disabled),&disabled};
        stages[1].pSpecializationInfo = &specialization;
        VkPipeline pipelineOff;
        CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&pipelineOff));
#ifdef SUBSAMPLED_SMOKE
        SubsampledReadback reconstructed(device,alloc,format,w,h,view,gp);
#endif
        std::vector<unsigned char> directFxaa, filteredSgsr;
#ifdef SUBSAMPLED_SMOKE
        const int testCount=3;
#elif defined(FDM_SMOKE)
        const int testCount=1;
#else
#ifdef VISIBILITY_SMOKE
        const int testCount=12;
#else
        const int testCount=29;
#endif
#endif
        for (int test = 0; test < testCount; ++test) {
#ifdef VISIBILITY_SMOKE
          if(test>=8 && test<=10) continue;
#endif
          for (unsigned y=0; y<h; ++y) for (unsigned x=0; x<w; ++x) {
            auto pixel=pixels+(y*w+x)*4;
            pixel[0]=32+x*3; pixel[1]=24+y*4; pixel[2]=100; pixel[3]=255;
#ifdef FDM_SMOKE
            pixel[0]=32;pixel[1]=24;pixel[2]=(x%2)*255;
#endif
            if (test>=12) {
              const unsigned char v=x>y*4/3 ? 255 : 0;
              pixel[0]=pixel[1]=pixel[2]=v; pixel[3]=87;
            }
            if(test>=24) pixel[0]=pixel[1]=pixel[2]=64;
            if (test==16) pixel[1]=pixel[2]=0;
            if (test==17) pixel[0]=pixel[1]=0;
            if (test==15) {
              const bool inside=x>=5 && x<34 && y>=3 && y<22;
              pixel[0]=inside?20:255; pixel[1]=inside?100:0; pixel[2]=inside?220:0;
            }
          }
          Push p{{0, 0, 1, 1},
                 {1.f / w, 1.f / h, float(w), float(h)},
                 {0, 0, w - 1, h - 1},
                 {.7f, 1, 0, format == VK_FORMAT_R8G8B8A8_SRGB ? 1.f : 0.f},
                 {test == 1 || test == 7 ? 2.f : 0.f, 0, 0, 0},
                 {1, 1, 0, 0}};
          if (test >= 11) p.content[2] = 1;
          if(test==19 || test==21 || test==23) p.content[2]=0;
          if (test == 14 || test == 15 || test>=19) p.flags[0] = 2;
          if(test==20 || test==21) { p.control[1]=.7f; p.control[2]=2; }
          if(test==22 || test==23) { p.crop[1]=1; p.crop[3]=-1; }
          if (test == 2) {
            p.crop[1] = 1;
            p.crop[3] = -1;
          }
          if (test == 3)
            p.flags[1] = 1;
          if (test == 4 || test == 5) {
            p.control[1] = .7f;
            p.control[2] = test - 3;
          }
          if (test == 6)
            p.flags[2] = 1;
          if (test == 7) {
            p.crop[0] = p.crop[1] = .25f;
            p.crop[2] = p.crop[3] = .5f;
            p.bounds[0] = w / 4;
            p.bounds[1] = h / 4;
            p.bounds[2] = w * 3 / 4 - 1;
            p.bounds[3] = h * 3 / 4 - 1;
          }
          if(test>=24) {
            p.flags[0]=test==27?2.f:0.f;
            p.flags[1]=test==28?1.f:0.f;
            p.content[2]=test==26?1.f:0.f;
            p.fxaaPatch[0]=(test==24 || test==28)?1.f:2.f;
          }
          CHECK(vkResetCommandPool(device, pool, 0));
          VkCommandBufferBeginInfo begin{
              VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
          CHECK(vkBeginCommandBuffer(cmd, &begin));
          VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
          b.image = input.Handle();
          b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
          b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
          b.srcQueueFamilyIndex = b.dstQueueFamilyIndex =
              VK_QUEUE_FAMILY_IGNORED;
          b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
          b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                               nullptr, 1, &b);
          VkBufferImageCopy copy{};
          copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          copy.imageExtent = {w, h, 1};
          vkCmdCopyBufferToImage(cmd, staging, input.Handle(),
                                 VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
          b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
          b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
          vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0,
                               nullptr, 0, nullptr, 1, &b);
          sampled.imageView = filtered.imageView = input.View();
          if ((test >= 13 && test <= 15) || test==20 || test==22) {
            const unsigned x=test==15?5:0, y=test==15?3:0, fw=test==15?29:w, fh=test==15?19:h;
            p.bounds[0]=x; p.bounds[1]=y; p.bounds[2]=x+fw-1; p.bounds[3]=y+fh-1;
            if(test==15) {
              p.crop[0]=float(x)/w; p.crop[1]=float(y+fh)/h;
              p.crop[2]=float(fw)/w; p.crop[3]=-float(fh)/h;
            }
          }
#ifdef SUBSAMPLED_SMOKE
          // Exercise both SGSR variants and fused AA against a pattern whose
          // protected center must survive storage/reconstruction exactly.
          p.flags[0]=test?2.f:0.f;
          p.content[2]=test==2?1.f:0.f;
#endif
          vkUpdateDescriptorSets(device,2,writes,0,nullptr);
          VkRenderPassBeginInfo start{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
          start.renderPass = pass;
          start.framebuffer = framebuffer;
          start.renderArea = scissor;
          const VkClearValue black{{{0.f,0.f,0.f,1.f}}};
          start.clearValueCount=1;start.pClearValues=&black;
          vkCmdBeginRenderPass(cmd, &start, VK_SUBPASS_CONTENTS_INLINE);
          vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, test==18?pipelineOff:pipeline);
          vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout,
                                  0, 1, &set, 0, nullptr);
          vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             sizeof(p), &p);
          if (test >= 8 && test <= 10)
            recordXrSwapchainPattern(vkCmdClearAttachments, cmd, w, h, test - 7);
          else {
#ifdef VISIBILITY_SMOKE
            const VkBuffer maskHandle=maskBuffer.Handle();const VkDeviceSize maskOffset=0;
            vkCmdBindVertexBuffers(cmd,0,1,&maskHandle,&maskOffset);
            vkCmdDraw(cmd,6,1,0,0);
#else
            vkCmdDraw(cmd, 3, 1, 0, 0);
#endif
          }
          vkCmdEndRenderPass(cmd);
          VkImage readbackTarget=target;
#ifdef SUBSAMPLED_SMOKE
          reconstructed.record(cmd,target,scissor);
          readbackTarget=reconstructed.image;
#endif
          b.image = readbackTarget;
          b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
          b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
          b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
          b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
          vkCmdPipelineBarrier(
              cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
          copy.bufferOffset = bytes;
          vkCmdCopyImageToBuffer(cmd, readbackTarget,
                                 VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging,
                                 1, &copy);
          VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
          host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
          host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
          vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0,
                               nullptr, 0, nullptr);
          CHECK(vkEndCommandBuffer(cmd));
          VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
          submit.commandBufferCount = 1;
          submit.pCommandBuffers = &cmd;
          CHECK(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
          CHECK(vkQueueWaitIdle(queue));
          if(test>=24) {
            const auto output=pixels+bytes;
            for(unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x) {
              const float dx=(x+.5f)*2/w-1,dy=(y+.5f)*2/h-1;
              const float r2=dx*dx+dy*dy;
              float tint[3]={0,0,0};
              if(test==24 || test==28) {
                // Game tile colors now come from game shaders, not presentation.
              } else {
                float t=std::clamp((r2-.2025f)/(.2209f-.2025f),0.f,1.f);
                t=t*t*(3-2*t); tint[0]=t; tint[2]=1-t;
              }
              for(int c=0;c<3;++c) {
                float expected=(test==24 || test==25 || test==28)?64.f/255.f:(64.f/255.f*.65f+tint[c]*.35f);
                if(format==VK_FORMAT_R8G8B8A8_SRGB)
                  expected=expected<=.0031308f?expected*12.92f:1.055f*std::pow(expected,1.f/2.4f)-.055f;
                assert(std::abs(int(output[(y*w+x)*4+c])-int(std::lround(expected*255)))<=2);
              }
              assert(output[(y*w+x)*4+3]==87);
            }
            std::cout << "FFR debug test=" << test << " passed\n";
            continue;
          }
          if (test>=12) {
            const auto output=pixels+bytes;
            unsigned smoothed=0;
            for (unsigned y=0; y<h; ++y) for (unsigned x=0; x<w; ++x) {
              const auto p=output+(y*w+x)*4;
              assert(p[3]==87); // Alpha survives direct, intermediate and SGSR paths.
              if(test==15) {
                const int color[3]={20,100,220};
                for(int c=0;c<3;++c) {
                  float v=color[c]/255.f;
                  if(format==VK_FORMAT_R8G8B8A8_SRGB) v=v<=.0031308f?v*12.92f:1.055f*std::pow(v,1.f/2.4f)-.055f;
                  assert(std::abs(int(p[c])-int(std::lround(v*255)))<=2);
                }
              } else {
                const int channel=test==17?2:0;
                const bool changed=p[channel]>0 && p[channel]<255;
                smoothed += changed;
                const float rx=(x+.5f)*2/w-1, ry=(y+.5f)*2/h-1;
                if(test==12 || test==13 || test==16 || test==17)
                  if(rx*rx+ry*ry>=.2209f) assert(!changed);
                if(test<20 && std::abs(int(x)-int(y*4/3))>8) assert(p[channel]==(x>y*4/3?255:0));
              }
            }
            if(test==18) assert(smoothed==0);
            else if(test!=15 && test!=19 && test!=21 && test!=23) assert(smoothed>20);
            if(test==12) directFxaa.assign(output,output+bytes);
            if(test==13) for(unsigned i=0;i<bytes;++i) {
              // Repeated fused draws must be deterministic in both formats.
              auto linear=[&](unsigned char v) {
                float c=v/255.f;
                return format==VK_FORMAT_R8G8B8A8_SRGB && i%4!=3
                  ? 255.f*(c<=.04045f?c/12.92f:std::pow((c+.055f)/1.055f,2.4f)) : float(v);
              };
              assert(std::abs(linear(output[i])-linear(directFxaa[i]))<=2);
            }
            if(test==14 || test==20 || test==22) filteredSgsr.assign(output,output+bytes);
            if(test==19 || test==21 || test==23) {
              // SGSR alone is not sufficient: enabling AA must alter the
              // central edge in each variant, including flipped/FOV output.
              unsigned different=0;
              for(unsigned i=0;i<bytes;++i) different+=output[i]!=filteredSgsr[i];
              assert(different>20);
            }
            std::cout << "Lightweight AA format=" << format << " edge=" << edge << " test=" << test << " smoothed=" << smoothed << '\n';
            continue;
          }
          int worst = 0;
          for (unsigned y = 0; y < h; ++y)
            for (unsigned x = 0; x < w; ++x)
              for (int c = 0; c < 4; ++c) {
                int expected;
                float u = (x + .5f) / w, v = (y + .5f) / h;
                if (test == 2)
                  v = 1 - v;
                if (test == 4 || test == 5) {
                  u = std::clamp((u - .5f) / .7f + .5f, .5f / w, 1 - .5f / w);
                  v = std::clamp((v - .5f) / .7f + .5f, .5f / h, 1 - .5f / h);
                }
                if (test == 7) {
                  u = .25f + u * .5f;
                  v = .25f + v * .5f;
                }
                int channel = test == 3 && c == 0   ? 2
                              : test == 3 && c == 2 ? 0
                                                    : c;
                expected = channel == 0   ? std::lround(32 + (u * w - .5f) * 3)
                           : channel == 1 ? std::lround(24 + (v * h - .5f) * 4)
                           : channel == 2 ? 100
                                          : 255;
                if (test >= 8 && test <= 10) {
                  bool channels[3] = {x < w / 2 || y >= h / 2,
                                      x >= w / 2, x < w / 2 && y >= h / 2};
                  // Top-left red, top-right green, bottom-left blue,
                  // bottom-right yellow; then perimeter and center marker.
                  channels[0] = (x < w / 2) == (y < h / 2);
                  bool center = x >= w * 3 / 8 && x < w * 5 / 8 &&
                                y >= h * 3 / 8 && y < h * 5 / 8;
                  bool white = x < std::max(1u, w / 50) ||
                               x >= w - std::max(1u, w / 50) ||
                               y < std::max(1u, h / 50) ||
                               y >= h - std::max(1u, h / 50);
                  for (unsigned bar = 0; bar < unsigned(test - 7); ++bar) {
                    auto left = w * (13 + bar * 2) / 32;
                    white |= x >= left && x < left + w / 32 &&
                             y >= h * 13 / 32 && y < h * 19 / 32;
                  }
                  expected = c == 3 || white || (!center && channels[c]) ? 255 : 0;
                }
#ifdef FDM_SMOKE
                expected=c==0?32:c==1?24:c==2?int(x%2)*255:255;
#endif
                // Scene projection matches GLES: sampled values are fragment
                // outputs, so an sRGB attachment encodes RGB on store. Bitmap
                // input (test 6) is already encoded and preserves its bytes.
                if ((test < 8 || test == 11) && test != 6 && c < 3 &&
                    format == VK_FORMAT_R8G8B8A8_SRGB) {
                  const float linear = expected / 255.f;
                  expected = std::lround(255.f * (linear <= .0031308f
                      ? linear * 12.92f
                      : 1.055f * std::pow(linear, 1.f / 2.4f) - .055f));
                }
#ifdef VISIBILITY_SMOKE
                if(x<w/4 || x>=3*w/4 || y<h/4 || y>=3*h/4) expected=c==3?255:0;
#endif
                int error = std::abs(int(pixels[bytes + (y * w + x) * 4 + c]) -
                                     expected);
#ifdef FDM_SMOKE
                const float dx=(x+.5f)*2/w-1,dy=(y+.5f)*2/h-1;
                if(c!=2 || dx*dx+dy*dy<.25f) assert(error<=2);
#endif
                worst = std::max(worst, error);
              }
          std::cout << "format=" << format << " edge=" << edge
                    << " test=" << test << " maxError=" << worst << '\n';
#ifdef FDM_SMOKE
          assert(worst>2); // Confirm actual coarsening, not merely accepted attachment creation.
#else
          assert(worst <= (test == 5 ? 5 : 2));
#endif
        }
        vkDestroyPipeline(device, pipelineOff, nullptr);
        vkDestroyPipeline(device, pipeline, nullptr);
        vkDestroyShaderModule(device, vertex, nullptr);
        vkDestroyShaderModule(device, fragment, nullptr);
      }
      vkDestroyFramebuffer(device, framebuffer, nullptr);
      vkDestroyRenderPass(device, pass, nullptr);
      vkDestroyImageView(device, view, nullptr);
      vkDestroyImage(device, target, nullptr);
      vkFreeMemory(device, targetMemory, nullptr);
    }
    vkDestroyPipelineLayout(device, layout, nullptr);
    vkDestroyDescriptorPool(device, descriptors, nullptr);
    vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    vkDestroySampler(device, sampler, nullptr);
    vkDestroyCommandPool(device, pool, nullptr);
    vkUnmapMemory(device, memory);
    vkDestroyBuffer(device, staging, nullptr);
    vkFreeMemory(device, memory, nullptr);
  }
  vkDestroyDevice(device, nullptr);
  vkDestroyInstance(instance, nullptr);
}
