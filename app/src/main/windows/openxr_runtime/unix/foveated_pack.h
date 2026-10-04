/* Same-device transport draw. Per-output resources live as long as its AHB;
 * never update a descriptor while its command buffer is in flight. */
#pragma once
#include <vulkan/vulkan.h>
#include <string.h>
#include "foveated_pack_spv.h"
#define GN_PACK_FUNCS(X)                                                       \
  X(CreateImageView)                                                           \
  X(DestroyImageView) X(CreateSampler) X(DestroySampler)                       \
      X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout)               \
          X(CreateDescriptorPool) X(DestroyDescriptorPool)                     \
              X(AllocateDescriptorSets) X(UpdateDescriptorSets)                \
                  X(CreatePipelineLayout) X(DestroyPipelineLayout)             \
                      X(CreateRenderPass) X(DestroyRenderPass)                 \
                          X(CreateFramebuffer) X(DestroyFramebuffer)           \
                              X(CreateShaderModule) X(DestroyShaderModule)     \
                                  X(CreateGraphicsPipelines)                   \
                                      X(DestroyPipeline) X(CmdBeginRenderPass) \
                                          X(CmdEndRenderPass)                  \
                                              X(CmdBindPipeline)               \
                                                  X(CmdBindDescriptorSets)     \
                                                      X(CmdPushConstants)      \
                                                          X(CmdDraw)
struct gn_pack {
  VkDevice device;
#define MEMBER(n) PFN_vk##n n;
  GN_PACK_FUNCS(MEMBER)
#undef MEMBER
  VkImageView target_view, source_view;
  VkImage source;
  uint32_t layer, width, height;
  VkSampler sampler;
  VkDescriptorSetLayout set_layout;
  VkDescriptorPool pool;
  VkDescriptorSet set;
  VkPipelineLayout layout;
  VkRenderPass pass;
  VkFramebuffer framebuffer;
  VkPipeline pipeline;
  float cuts[8];
};
static void gn_pack_destroy(struct gn_pack *p) {
  if (!p->device)
    return;
#define DESTROY(field, call)                                                   \
  if (p->field && p->call)                                                     \
  p->call(p->device, p->field, NULL)
  DESTROY(pipeline, DestroyPipeline);
  DESTROY(framebuffer, DestroyFramebuffer);
  DESTROY(pass, DestroyRenderPass);
  DESTROY(layout, DestroyPipelineLayout);
  DESTROY(pool, DestroyDescriptorPool);
  DESTROY(set_layout, DestroyDescriptorSetLayout);
  DESTROY(sampler, DestroySampler);
  DESTROY(source_view, DestroyImageView);
  DESTROY(target_view, DestroyImageView);
#undef DESTROY
  memset(p, 0, sizeof(*p));
}
static int gn_pack_init(struct gn_pack *p, VkDevice device,
                        PFN_vkGetDeviceProcAddr get, VkImage output,
                        uint32_t width, uint32_t height, VkFormat source_format,
                        uint32_t logical_width, uint32_t logical_height) {
  p->device = device;
  p->width = width;
  p->height = height;
  uint32_t ox=(logical_width/8)*2, oy=(logical_height/8)*2;
  p->cuts[0]=(float)ox/logical_width; p->cuts[1]=1-p->cuts[0];
  p->cuts[2]=(float)oy/logical_height; p->cuts[3]=1-p->cuts[2];
  p->cuts[4]=(float)(ox/2)/width; p->cuts[5]=1-p->cuts[4];
  p->cuts[6]=(float)(oy/2)/height; p->cuts[7]=1-p->cuts[6];
  VkFormat target_format=(source_format==VK_FORMAT_R8G8B8A8_SRGB || source_format==VK_FORMAT_B8G8R8A8_SRGB)
      ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
#define LOAD(n)                                                                \
  p->n = (PFN_vk##n)get(device, "vk" #n);                                      \
  if (!p->n)                                                                   \
    goto fail;
  GN_PACK_FUNCS(LOAD)
#undef LOAD
#define TRY(x)                                                                 \
  do {                                                                         \
    if ((x) != VK_SUCCESS)                                                     \
      goto fail;                                                               \
  } while (0)
  VkImageViewCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = output,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = target_format,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
  TRY(p->CreateImageView(device, &vi, NULL, &p->target_view));
  VkSamplerCreateInfo sm = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
  TRY(p->CreateSampler(device, &sm, NULL, &p->sampler));
  VkDescriptorSetLayoutBinding binding = {
      0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
      VK_SHADER_STAGE_FRAGMENT_BIT, NULL};
  VkDescriptorSetLayoutCreateInfo sl = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1,
      .pBindings = &binding};
  TRY(p->CreateDescriptorSetLayout(device, &sl, NULL, &p->set_layout));
  VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
  VkDescriptorPoolCreateInfo dp = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &size};
  TRY(p->CreateDescriptorPool(device, &dp, NULL, &p->pool));
  VkDescriptorSetAllocateInfo da = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = p->pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &p->set_layout};
  TRY(p->AllocateDescriptorSets(device, &da, &p->set));
  VkPushConstantRange range = {VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(p->cuts)};
  VkPipelineLayoutCreateInfo pl = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &p->set_layout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &range};
  TRY(p->CreatePipelineLayout(device, &pl, NULL, &p->layout));
  VkAttachmentDescription attachment = {
      .format = target_format,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
      .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
      .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
      .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference color = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub = {.pipelineBindPoint =
                                  VK_PIPELINE_BIND_POINT_GRAPHICS,
                              .colorAttachmentCount = 1,
                              .pColorAttachments = &color};
  VkRenderPassCreateInfo rp = {.sType =
                                   VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                               .attachmentCount = 1,
                               .pAttachments = &attachment,
                               .subpassCount = 1,
                               .pSubpasses = &sub};
  TRY(p->CreateRenderPass(device, &rp, NULL, &p->pass));
  VkFramebufferCreateInfo fb = {.sType =
                                    VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                .renderPass = p->pass,
                                .attachmentCount = 1,
                                .pAttachments = &p->target_view,
                                .width = width,
                                .height = height,
                                .layers = 1};
  TRY(p->CreateFramebuffer(device, &fb, NULL, &p->framebuffer));
  VkShaderModule modules[2] = {0};
  VkShaderModuleCreateInfo sc = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(gn_pack_vertex),
      .pCode = gn_pack_vertex};
  TRY(p->CreateShaderModule(device, &sc, NULL, &modules[0]));
  sc.codeSize = sizeof(gn_pack_fragment);
  sc.pCode = gn_pack_fragment;
  VkResult result = p->CreateShaderModule(device, &sc, NULL, &modules[1]);
  if (result != VK_SUCCESS) {
    p->DestroyShaderModule(device, modules[0], NULL);
    goto fail;
  }
  VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT,
       .module = modules[0],
       .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
       .module = modules[1],
       .pName = "main"}};
  VkPipelineVertexInputStateCreateInfo vertex = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
  VkViewport viewport = {0, 0, (float)width, (float)height, 0, 1};
  VkRect2D area = {{0, 0}, {width, height}};
  VkPipelineViewportStateCreateInfo vp = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1,
      .pViewports = &viewport,
      .scissorCount = 1,
      .pScissors = &area};
  VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL,
      .lineWidth = 1};
  VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
  VkPipelineColorBlendAttachmentState blend = {.colorWriteMask = 15};
  VkPipelineColorBlendStateCreateInfo bs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1,
      .pAttachments = &blend};
  VkGraphicsPipelineCreateInfo gp = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2,
      .pStages = stages,
      .pVertexInputState = &vertex,
      .pInputAssemblyState = &ia,
      .pViewportState = &vp,
      .pRasterizationState = &rs,
      .pMultisampleState = &ms,
      .pColorBlendState = &bs,
      .layout = p->layout,
      .renderPass = p->pass};
  result = p->CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, NULL,
                                      &p->pipeline);
  p->DestroyShaderModule(device, modules[0], NULL);
  p->DestroyShaderModule(device, modules[1], NULL);
  if (result != VK_SUCCESS)
    goto fail;
  return 1;
fail:
  gn_pack_destroy(p);
  return 0;
#undef TRY
}
static int gn_pack_source(struct gn_pack *p, VkImage source, VkFormat format,
                          uint32_t layer) {
  if (p->source_view && p->source == source && p->layer == layer)
    return 1;
  if (p->source_view)
    p->DestroyImageView(p->device, p->source_view, NULL);
  p->source_view = VK_NULL_HANDLE;
  p->source = source;
  p->layer = layer;
  VkImageViewCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = source,
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = format,
      .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1}};
  if (p->CreateImageView(p->device, &vi, NULL, &p->source_view) != VK_SUCCESS)
    return 0;
  VkDescriptorImageInfo image = {p->sampler, p->source_view,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .dstSet = p->set,
                                .descriptorCount = 1,
                                .descriptorType =
                                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                .pImageInfo = &image};
  p->UpdateDescriptorSets(p->device, 1, &write, 0, NULL);
  return 1;
}
static void gn_pack_record(struct gn_pack *p, VkCommandBuffer cmd,
                           VkFormat format) {
  VkRenderPassBeginInfo rp = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                              .renderPass = p->pass,
                              .framebuffer = p->framebuffer,
                              .renderArea = {{0, 0}, {p->width, p->height}}};
  (void)format;
  p->CmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
  p->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pipeline);
  p->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p->layout, 0,
                           1, &p->set, 0, NULL);
  p->CmdPushConstants(cmd, p->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(p->cuts),
                      p->cuts);
  p->CmdDraw(cmd, 54, 1, 0, 0);
  p->CmdEndRenderPass(cmd);
}
#undef GN_PACK_FUNCS
