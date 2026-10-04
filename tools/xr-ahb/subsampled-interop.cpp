#include "interop-test.h"
int main(int argc, char **argv) {
  setbuf(stdout, nullptr);
  if (argc != 5 && argc != 6) {
    fprintf(stderr,
            "Usage: %s producer.so consumer.so ordinary|subsampled output.rgba "
            "[--consumer-owned] (consumer may be local)\n",
            argv[0]);
    return 2;
  }
  bool sub = !strcmp(argv[3], "subsampled");
  if (!sub && strcmp(argv[3], "ordinary"))
    return 2;
  bool consumerOwned = argc == 6 && !strcmp(argv[5], "--consumer-owned");
  if (argc == 6 && !consumerOwned)
    return 2;
  if (consumerOwned && !strcmp(argv[2], "local"))
    return 2;
  Context producer(argv[1]);
  bool local = !strcmp(argv[2], "local");
  std::unique_ptr<Context> other;
  if (!local)
    other = std::make_unique<Context>(argv[2]);
  Context &consumer = local ? producer : *other;
  auto usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  AHardwareBuffer *ahb{};
  auto exportBuffer = [&](Image &image) {
    auto &c = image.c;
    VkMemoryGetAndroidHardwareBufferInfoANDROID gi{
        VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
    gi.memory = image.memory;
    CHECK(V(GetMemoryAndroidHardwareBufferANDROID)(c.device, &gi, &ahb));
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(ahb, &desc);
    printf("Export owner=%s subsampled=%d %ux%u layers=%u stride=%u format=%u "
           "usage=%llx\n",
           consumerOwned ? "consumer" : "producer", sub, desc.width,
           desc.height, desc.layers, desc.stride, desc.format,
           (unsigned long long)desc.usage);
  };
  std::unique_ptr<Image> consumerImage;
  if (consumerOwned) {
    consumerImage =
        std::make_unique<Image>(consumer, FORMAT, W, H, usage, sub, true);
    exportBuffer(*consumerImage);
    // Establish a defined external layout on the allocating device before
    // Turnip acquires the allocation. No input pixels need preservation.
    auto &c = consumer;
    auto cmd = c.begin();
    barrier(c, cmd, consumerImage->image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL, 0, 0, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c.family,
            VK_QUEUE_FAMILY_FOREIGN_EXT);
    c.finish(cmd);
  }
  Image source(producer, FORMAT, W, H, usage, sub, true, ahb);
  Image density(producer, VK_FORMAT_R8G8_UNORM, 32, 32,
                VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT);
  {
    auto &c = producer;
    auto cmd = c.begin();
    barrier(c, cmd, density.image, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue color{};
    color.float32[0] = color.float32[1] = 127.0f / 255.0f;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    V(CmdClearColorImage)
    (cmd, density.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1,
     &range);
    barrier(c, cmd, density.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT,
            VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_FRAGMENT_DENSITY_MAP_READ_BIT_EXT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_DENSITY_PROCESS_BIT_EXT);
    c.finish(cmd);
  }
  Draw render(producer, source, &density, nullptr, sub,
              consumerOwned ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                            : VK_IMAGE_LAYOUT_UNDEFINED);
  {
    auto &c = producer;
    auto cmd = c.begin();
    if (consumerOwned)
      barrier(c, cmd, source.image, VK_IMAGE_LAYOUT_GENERAL,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
              VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_QUEUE_FAMILY_FOREIGN_EXT, c.family);
    render.record(cmd);
    if (!local)
      barrier(c, cmd, source.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
              VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
              VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
              VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, c.family,
              VK_QUEUE_FAMILY_FOREIGN_EXT);
    c.finish(cmd);
  }
  if (!consumerOwned)
    exportBuffer(source);
  bool good = false;
  {
    std::unique_ptr<Image> imported;
    if (!local && !consumerOwned)
      imported = std::make_unique<Image>(consumer, FORMAT, W, H, usage, sub,
                                         true, ahb);
    Image &sampled = local           ? source
                     : consumerOwned ? *consumerImage
                                     : *imported;
    Image target(consumer, FORMAT, W, H,
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    Draw reconstruct(consumer, target, nullptr, &sampled, sub);
    auto &c = consumer;
    VkBufferCreateInfo bc{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bc.size = W * H * 4;
    bc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VkBuffer buffer{};
    CHECK(V(CreateBuffer)(c.device, &bc, nullptr, &buffer));
    VkMemoryRequirements mr{};
    V(GetBufferMemoryRequirements)(c.device, buffer, &mr);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex =
        c.type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory memory{};
    CHECK(V(AllocateMemory)(c.device, &ma, nullptr, &memory));
    CHECK(V(BindBufferMemory)(c.device, buffer, memory, 0));
    auto cmd = c.begin();
    barrier(c, cmd, sampled.image,
            local ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                  : VK_IMAGE_LAYOUT_GENERAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            local ? VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT : 0,
            VK_ACCESS_SHADER_READ_BIT,
            local ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                  : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            local ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_FOREIGN_EXT,
            local ? VK_QUEUE_FAMILY_IGNORED : c.family);
    reconstruct.record(cmd);
    barrier(c, cmd, target.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {W, H, 1};
    V(CmdCopyImageToBuffer)
    (cmd, target.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    V(CmdPipelineBarrier)
    (cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
     &host, 0, nullptr, 0, nullptr);
    c.finish(cmd);
    void *mapped{};
    CHECK(V(MapMemory)(c.device, memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    auto *p = (unsigned char *)mapped;
    auto *out = fopen(argv[4], "wb");
    if (!out) {
      perror("output");
      return 1;
    }
    fwrite(p, 1, W * H * 4, out);
    fclose(out);
    unsigned bad = 0, total = 0, maxError = 0, blueChanged = 0;
    for (unsigned y = 8; y < H - 8; y++)
      for (unsigned x = 8; x < W - 8; x++) {
        if (x % 64 < 8 || x % 64 >= 56 || y % 64 < 8 || y % 64 >= 56)
          continue;
        auto *pixel = p + 4 * (y * W + x);
        unsigned er = std::lround((x / 64 + 1) * 255.0 / 8),
                 eg = std::lround((y / 64 + 1) * 255.0 / 8);
        unsigned error = std::max({unsigned(std::abs(int(pixel[0]) - int(er))),
                                   unsigned(std::abs(int(pixel[1]) - int(eg))),
                                   unsigned(255 - pixel[3])});
        maxError = std::max(maxError, error);
        bad += error > 2;
        blueChanged += std::abs(int(pixel[2]) - int(x % 2 * 255)) > 2;
        total++;
      }
    printf("Pixel check: bad=%u/%u maxError=%u reducedDensityPixels=%u\n", bad,
           total, maxError, blueChanged);
    good = bad == 0 && blueChanged > total / 4;
    printf("%s\n", good ? "PASS" : "FAIL");
    V(UnmapMemory)(c.device, memory);
    V(DestroyBuffer)(c.device, buffer, nullptr);
    V(FreeMemory)(c.device, memory, nullptr);
  }
  AHardwareBuffer_release(ahb);
  return good ? 0 : 1;
}
