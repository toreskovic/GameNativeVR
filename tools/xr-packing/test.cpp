// Runs the production packing draw on Turnip, then production presentation
// shaders on the system driver. Tests the actual shared AHB, not a CPU
// imitation.
#include "../../app/src/main/cpp/xrimmersive/xr_lsfg_capture_spv.h"
#include "../../app/src/main/cpp/xrimmersive/xr_vulkan_present_spv.h"
#include "../xr-ahb/interop-test.h"
#include "../../app/src/main/windows/openxr_runtime/gamenative_foveated_packing.h"
extern "C" void *test_pack_create(VkDevice, PFN_vkGetDeviceProcAddr, VkImage,
                                  VkImage, VkFormat, unsigned, unsigned, unsigned, unsigned);
extern "C" void test_pack_record(void *, VkCommandBuffer, VkFormat);
extern "C" void test_pack_destroy(void *);
struct Readback {
  Context &c;
  VkBuffer buffer{};
  VkDeviceMemory memory{};
  Readback(Context &c) : c(c) {
    VkBufferCreateInfo b{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    b.size = W * H * 4;
    b.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    CHECK(V(CreateBuffer)(c.device, &b, nullptr, &buffer));
    VkMemoryRequirements r{};
    V(GetBufferMemoryRequirements)(c.device, buffer, &r);
    VkMemoryAllocateInfo a{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    a.allocationSize = r.size;
    a.memoryTypeIndex =
        c.type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    CHECK(V(AllocateMemory)(c.device, &a, nullptr, &memory));
    CHECK(V(BindBufferMemory)(c.device, buffer, memory, 0));
  }
  void record(VkCommandBuffer cmd, Image &image, VkImageLayout layout) {
    barrier(c, cmd, image.image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy b{};
    b.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.imageExtent = {W, H, 1};
    V(CmdCopyImageToBuffer)
    (cmd, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &b);
    VkMemoryBarrier h{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    h.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    h.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    V(CmdPipelineBarrier)
    (cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &h,
     0, nullptr, 0, nullptr);
  }
  bool check(bool flip = false, bool crop = false, bool srgb = false, bool sharp = false) {
    void *m{};
    CHECK(V(MapMemory)(c.device, memory, 0, VK_WHOLE_SIZE, 0, &m));
    auto *p = (unsigned char *)m;
    unsigned bad = 0, n = 0, max = 0;
    for (unsigned y = 4; y < H - 4; y++)
      for (unsigned x = 4; x < W - 4; x++) {
        float sx = crop ? W*.25f + (x + .5f) * .5f : x + .5f;
        float sy = crop ? H*.25f + (y + .5f) * .5f : y + .5f;
        if (flip)
          sy = H - sy;
        unsigned xx = sx, yy = sy;
        if (xx % 64 < 8 || xx % 64 > 55 || yy % 64 < 8 || yy % 64 > 55)
          continue;
        auto *a = p + 4 * (y * W + x);
        unsigned er = std::lround((xx / 64 + 1) * 255.0 / 8),
                 eg = std::lround((yy / 64 + 1) * 255.0 / 8);
        if (srgb) {
          auto encode = [](unsigned v) {
            float x = v / 255.f;
            return unsigned(std::lround(
                (x <= .0031308f ? 12.92f * x
                                : 1.055f * std::pow(x, 1.f / 2.4f) - .055f) *
                255.f));
          };
          er = encode(er);
          eg = encode(eg);
        }
        unsigned d = std::max({unsigned(std::abs(int(a[0]) - int(er))),
                               unsigned(std::abs(int(a[1]) - int(eg))),
                               unsigned(255 - a[3])});
        if (sharp && x > (W/8)*2+2 && x < W-(W/8)*2-2 &&
            y > (H/8)*2+2 && y < H-(H/8)*2-2)
          d = std::max(d, unsigned(std::abs(int(a[2])-int((x%2)*255))));
        max = std::max(max, d);
        bad += d > 2;
        n++;
      }
    V(UnmapMemory)(c.device, memory);
    printf("checked=%u bad=%u max=%u\n", n, bad, max);
    return !bad;
  }
  std::vector<unsigned char> pixels() {
    void* mapped{};
    CHECK(V(MapMemory)(c.device,memory,0,VK_WHOLE_SIZE,0,&mapped));
    auto* bytes=static_cast<unsigned char*>(mapped);
    std::vector<unsigned char> result(bytes,bytes+W*H*4);
    V(UnmapMemory)(c.device,memory);
    return result;
  }
  ~Readback() {
    V(DestroyBuffer)(c.device, buffer, nullptr);
    V(FreeMemory)(c.device, memory, nullptr);
  }
};
bool captureCheck(Context &c, Image &input, bool flip, bool crop, bool warp,
                  bool srgb) {
  Image target(c, FORMAT, W, H,
               VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
  VkSamplerCreateInfo sm{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  sm.magFilter = sm.minFilter = VK_FILTER_LINEAR;
  sm.addressModeU = sm.addressModeV = sm.addressModeW =
      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  VkSampler sampler{};
  CHECK(V(CreateSampler)(c.device, &sm, nullptr, &sampler));
  VkDescriptorSetLayoutBinding b[] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
       VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       nullptr},
      {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT,
       nullptr}};
  VkDescriptorSetLayoutCreateInfo sl{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  sl.bindingCount = 3;
  sl.pBindings = b;
  VkDescriptorSetLayout setLayout{};
  CHECK(V(CreateDescriptorSetLayout)(c.device, &sl, nullptr, &setLayout));
  VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
  VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl.setLayoutCount = 1;
  pl.pSetLayouts = &setLayout;
  pl.pushConstantRangeCount = 1;
  pl.pPushConstantRanges = &range;
  VkPipelineLayout layout{};
  CHECK(V(CreatePipelineLayout)(c.device, &pl, nullptr, &layout));
  VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  mi.pCode = xrimmersive::windowsvr::kCaptureSpv;
  mi.codeSize = sizeof(xrimmersive::windowsvr::kCaptureSpv);
  VkShaderModule module{};
  CHECK(V(CreateShaderModule)(c.device, &mi, nullptr, &module));
  VkComputePipelineCreateInfo pi{
      VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
  pi.layout = layout;
  pi.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              nullptr,
              0,
              VK_SHADER_STAGE_COMPUTE_BIT,
              module,
              "main",
              nullptr};
  const VkBool32 packedInput = VK_TRUE;
  const VkSpecializationMapEntry entry{1, 0, sizeof(packedInput)};
  const VkSpecializationInfo spec{1, &entry, sizeof(packedInput), &packedInput};
  pi.stage.pSpecializationInfo = &spec;
  VkPipeline pipeline{};
  CHECK(V(CreateComputePipelines)(c.device, VK_NULL_HANDLE, 1, &pi, nullptr,
                                  &pipeline));
  VkDescriptorPoolSize sizes[] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
      {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2}};
  VkDescriptorPoolCreateInfo poolInfo{
      VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  poolInfo.maxSets = 1;
  poolInfo.poolSizeCount = 2;
  poolInfo.pPoolSizes = sizes;
  VkDescriptorPool pool{};
  CHECK(V(CreateDescriptorPool)(c.device, &poolInfo, nullptr, &pool));
  VkDescriptorSetAllocateInfo da{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  da.descriptorPool = pool;
  da.descriptorSetCount = 1;
  da.pSetLayouts = &setLayout;
  VkDescriptorSet set{};
  CHECK(V(AllocateDescriptorSets)(c.device, &da, &set));
  VkDescriptorImageInfo images[] = {
      {sampler, input.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
      {VK_NULL_HANDLE, target.view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, target.view, VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet writes[3]{};
  for (int i = 0; i < 3; i++) {
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[i].dstSet = set;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                 : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[i].pImageInfo = &images[i];
  }
  V(UpdateDescriptorSets)(c.device, 3, writes, 0, nullptr);
  struct Params {
    float crop[4], rotation[3][4], sourceFov[4], targetFov[4];
    int flags[4];
    float packing[4];
  } push{};
  float start = crop ? .25f : 0, extent = crop ? .5f : 1;
  push.crop[0] = start;
  push.crop[1] = flip ? start + extent : start;
  push.crop[2] = extent;
  push.crop[3] = flip ? -extent : extent;
  for (int i = 0; i < 3; i++)
    push.rotation[i][i] = 1;
  push.rotation[0][3] = W;
  push.rotation[1][3] = H;
  for (int i = 0; i < 4; i++)
    push.sourceFov[i] = push.targetFov[i] = i % 2 ? 1 : -1;
  push.flags[0] = warp;
  gn_packing_parameters(W, H, push.packing);
  Readback read(c);
  auto cmd = c.begin();
  barrier(c, cmd, target.image, VK_IMAGE_LAYOUT_UNDEFINED,
          VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
  V(CmdBindPipeline)(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  V(CmdBindDescriptorSets)
  (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
  V(CmdPushConstants)
  (cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
  V(CmdDispatch)(cmd, (W+7) / 8, (H+7) / 8, 1);
  read.record(cmd, target, VK_IMAGE_LAYOUT_GENERAL);
  c.finish(cmd);
  printf("capture flip=%d crop=%d warp=%d ", flip, crop, warp);
  bool ok = read.check(flip, crop, srgb);
  V(DestroyPipeline)(c.device, pipeline, nullptr);
  V(DestroyShaderModule)(c.device, module, nullptr);
  V(DestroyPipelineLayout)(c.device, layout, nullptr);
  V(DestroyDescriptorPool)(c.device, pool, nullptr);
  V(DestroyDescriptorSetLayout)(c.device, setLayout, nullptr);
  V(DestroySampler)(c.device, sampler, nullptr);
  return ok;
}
int main(int argc, char **argv) {
  setbuf(stdout, nullptr);
  if (argc != 3)
    return 2;
  Context producer(argv[1], true), consumer(argv[2]);
  auto usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  unsigned cases = 0;
  bool good = true;
  for (auto inputFormat : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB,
                           VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB}) {
    if (W != 512 && inputFormat != FORMAT) continue;
    bool srgb = inputFormat == VK_FORMAT_R8G8B8A8_SRGB ||
                inputFormat == VK_FORMAT_B8G8R8A8_SRGB;
    printf("Input format=%d\n", inputFormat);
    Image input(producer, inputFormat, W, H, usage);
    Draw pattern(producer, input, nullptr, nullptr, false);
    Image packed(producer, FORMAT, gn_packed_extent(W), gn_packed_extent(H), usage, false, true, nullptr, inputFormat == VK_FORMAT_R8G8B8A8_SRGB || inputFormat == VK_FORMAT_B8G8R8A8_SRGB);
    void *pack =
        test_pack_create(producer.device, producer.gd, input.image,
                         packed.image, inputFormat, gn_packed_extent(W), gn_packed_extent(H), W, H);
    if (!pack)
      return 1;
    {
      auto &c = producer;
      auto cmd = c.begin();
      pattern.record(cmd);
      barrier(c, cmd, input.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
      barrier(c, cmd, packed.image, VK_IMAGE_LAYOUT_UNDEFINED,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
      test_pack_record(pack, cmd, inputFormat);
      barrier(c, cmd, packed.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c.family,
              VK_QUEUE_FAMILY_FOREIGN_EXT);
      c.finish(cmd);
    }
    test_pack_destroy(pack);
    AHardwareBuffer *ahb{};
    {
      auto &c = producer;
      VkMemoryGetAndroidHardwareBufferInfoANDROID gi{
          VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
      gi.memory = packed.memory;
      CHECK(V(GetMemoryAndroidHardwareBufferANDROID)(c.device, &gi, &ahb));
    }
    {
      auto &c = consumer;
      Image imported(c, FORMAT, gn_packed_extent(W), gn_packed_extent(H), usage, false, true, ahb);
      {
        auto cmd = c.begin();
        barrier(c, cmd, imported.image, VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                (VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT),
                VK_QUEUE_FAMILY_FOREIGN_EXT, c.family);
        c.finish(cmd);
      }
      for (bool edge : {false, true})
        for (bool aa : {false, true})
          for (bool sgsr : {false, true})
            for (bool flip : {false, true})
              for (bool crop : {false, true}) {
                if (W != 512 && (edge || aa != sgsr)) continue;
                std::vector<unsigned char> reference;
                for (bool central : {false,true}) {
              Image target(c, FORMAT, W, H,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
                Draw display(c, target, nullptr, &imported, false,
                             VK_IMAGE_LAYOUT_UNDEFINED,
                             edge ? xrimmersive::presentEdge
                                  : xrimmersive::presentBasic,
                             edge ? sizeof(xrimmersive::presentEdge)
                                  : sizeof(xrimmersive::presentBasic), central);
                Readback read(c);
                struct Push {
                  float crop[4], viewport[4], bounds[4], control[4], flags[4],
                      content[4], patch[4], packing[4];
                };
                float start = crop ? .25f : 0, extent = crop ? .5f : 1;
                Push push{{start, flip ? start + extent : start, extent,
                           flip ? -extent : extent},
                          {1.f / W, 1.f / H, float(W), float(H)},
                          {start * W, start * H, (start + extent) * W - 1,
                           (start + extent) * H - 1},
                          {.7f, 1, 0, 0},
                          {sgsr ? 1.f : 0, 0, 0, 0},
                          {1, 1, aa ? 1.f : 0, 0},
                          {0, 0, 0, 0}, {}};
                gn_packing_parameters(W, H, push.packing);
                auto cmd = c.begin();
                VkRenderPassBeginInfo rp{
                    VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
                rp.renderPass = display.pass;
                rp.framebuffer = display.fb;
                rp.renderArea = {{0, 0}, {W, H}};
                V(CmdBeginRenderPass)(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
                V(CmdBindPipeline)
                (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, display.pipeline);
                V(CmdBindDescriptorSets)
                (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, display.layout, 0, 1,
                 &display.set, 0, nullptr);
                V(CmdPushConstants)
                (cmd, display.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                 sizeof(push), &push);
                V(CmdDraw)(cmd, 3, 1, 0, 0);
                V(CmdEndRenderPass)(cmd);
                read.record(cmd, target,
                            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                c.finish(cmd);
                printf("edge=%d aa=%d sgsr=%d flip=%d crop=%d ", edge, aa, sgsr,
                       flip, crop);
                good = read.check(flip, crop, srgb, !aa && !sgsr && !crop) && good;
                auto actual=read.pixels();
                if(!central) reference=actual;
                else {
                  unsigned maxDifference=0;
                  for(size_t i=0;i<actual.size();++i)
                    maxDifference=std::max(maxDifference,unsigned(std::abs(int(actual[i])-int(reference[i]))));
                  printf("central vs safe maximum difference=%u\n",maxDifference);
                  good = maxDifference<=2 && good;
                }
                cases++;
                }
              }
      for (bool flip : {false, true})
        for (bool crop : {false, true})
          for (bool warp : {false, true}) {
            good = captureCheck(c, imported, flip, crop, warp, srgb) && good;
            cases++;
          }
    }
    AHardwareBuffer_release(ahb);
  }
  printf("%s %u packed presentation cases\n", good ? "PASS" : "FAIL", cases);
  return good ? 0 : 1;
}
