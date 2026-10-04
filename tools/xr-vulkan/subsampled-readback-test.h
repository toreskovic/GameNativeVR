// Test-only compositor analogue: reconstruct subsampled storage through an
// immutable sampler into a conventional image before CPU readback. Never copy
// subsampled image bytes directly: that does not reconstruct their contents.
#include "subsampled-readback.h" // Generated from subsampled-readback.frag.
struct SubsampledReadback {
  VkDevice device;
  VkImage image{};
  VkDeviceMemory memory{};
  VkImageView view{};
  VkSampler sampler{};
  VkDescriptorSetLayout setLayout{};
  VkDescriptorPool pool{};
  VkDescriptorSet set{};
  VkPipelineLayout layout{};
  VkRenderPass pass{};
  VkFramebuffer framebuffer{};
  VkPipeline pipeline{};
  SubsampledReadback(VkDevice device, const lsfg::Device& alloc, VkFormat format,
                    unsigned w, unsigned h, VkImageView source,
                    VkGraphicsPipelineCreateInfo gp):device(device) {
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType=VK_IMAGE_TYPE_2D;ci.format=format;ci.extent={w,h,1};
    ci.mipLevels=ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;
    ci.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    CHECK(vkCreateImage(device,&ci,nullptr,&image));
    VkMemoryRequirements mr{};vkGetImageMemoryRequirements(device,image,&mr);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize=mr.size;ma.memoryTypeIndex=alloc.FindMemoryType(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    CHECK(vkAllocateMemory(device,&ma,nullptr,&memory));CHECK(vkBindImageMemory(device,image,memory,0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=format;
    vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    CHECK(vkCreateImageView(device,&vi,nullptr,&view));
    VkSamplerCreateInfo sm{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sm.flags=VK_SAMPLER_CREATE_SUBSAMPLED_BIT_EXT;
    sm.magFilter=sm.minFilter=VK_FILTER_LINEAR;
    sm.addressModeU=sm.addressModeV=sm.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    CHECK(vkCreateSampler(device,&sm,nullptr,&sampler));
    VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,&sampler};
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};sl.bindingCount=1;sl.pBindings=&binding;
    CHECK(vkCreateDescriptorSetLayout(device,&sl,nullptr,&setLayout));
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};dp.maxSets=dp.poolSizeCount=1;dp.pPoolSizes=&size;
    CHECK(vkCreateDescriptorPool(device,&dp,nullptr,&pool));
    VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};da.descriptorPool=pool;da.descriptorSetCount=1;da.pSetLayouts=&setLayout;
    CHECK(vkAllocateDescriptorSets(device,&da,&set));
    VkDescriptorImageInfo sampled{sampler,source,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};wr.dstSet=set;wr.descriptorCount=1;wr.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;wr.pImageInfo=&sampled;
    vkUpdateDescriptorSets(device,1,&wr,0,nullptr);
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&setLayout;
    CHECK(vkCreatePipelineLayout(device,&pl,nullptr,&layout));
    VkAttachmentDescription attachment{};attachment.format=format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    attachment.finalLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};sub.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;sub.colorAttachmentCount=1;sub.pColorAttachments=&color;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=rp.subpassCount=1;rp.pAttachments=&attachment;rp.pSubpasses=&sub;
    CHECK(vkCreateRenderPass(device,&rp,nullptr,&pass));
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=pass;fb.attachmentCount=1;fb.pAttachments=&view;fb.width=w;fb.height=h;fb.layers=1;
    CHECK(vkCreateFramebuffer(device,&fb,nullptr,&framebuffer));
    VkShaderModuleCreateInfo sc{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sc.pCode=readbackFragment;sc.codeSize=sizeof(readbackFragment);
    VkShaderModule fragment{};CHECK(vkCreateShaderModule(device,&sc,nullptr,&fragment));
    VkPipelineShaderStageCreateInfo stages[]={gp.pStages[0],gp.pStages[1]};
    stages[1].module=fragment;stages[1].pSpecializationInfo=nullptr;
    gp.pStages=stages;gp.layout=layout;gp.renderPass=pass;
    CHECK(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&gp,nullptr,&pipeline));
    vkDestroyShaderModule(device,fragment,nullptr);
  }
  void record(VkCommandBuffer cmd,VkImage source,VkRect2D area) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.image=source;
    b.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;b.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&b);
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};rp.renderPass=pass;rp.framebuffer=framebuffer;rp.renderArea=area;
    vkCmdBeginRenderPass(cmd,&rp,VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
    vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_GRAPHICS,layout,0,1,&set,0,nullptr);
    vkCmdDraw(cmd,3,1,0,0);vkCmdEndRenderPass(cmd);
  }
  ~SubsampledReadback() {
    vkDestroyPipeline(device,pipeline,nullptr);vkDestroyFramebuffer(device,framebuffer,nullptr);vkDestroyRenderPass(device,pass,nullptr);
    vkDestroyPipelineLayout(device,layout,nullptr);vkDestroyDescriptorPool(device,pool,nullptr);vkDestroyDescriptorSetLayout(device,setLayout,nullptr);vkDestroySampler(device,sampler,nullptr);
    vkDestroyImageView(device,view,nullptr);vkDestroyImage(device,image,nullptr);vkFreeMemory(device,memory,nullptr);
  }
};
