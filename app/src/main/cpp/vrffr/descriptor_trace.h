#pragma once
#include <map>
#include <memory>
#include <vector>
#include <cstring>
#include <vulkan/vulkan.h>

namespace descriptor_trace {
struct Binding { VkDescriptorType type; uint32_t count; VkShaderStageFlags stages; };
using Layout = std::map<uint32_t, Binding>;
struct Set {
  Layout layout;
  VkDescriptorPool pool{};
  bool valid = true;
  uint64_t revision = 0;
  std::map<std::pair<uint32_t, uint32_t>, VkImageView> images;
};
struct Template {
  VkDescriptorUpdateTemplateType type{};
  uint32_t set = 0;
  VkPipelineBindPoint bindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  Layout layout;
  std::vector<VkDescriptorUpdateTemplateEntry> entries;
};
inline bool sampled(VkDescriptorType t) {
  return t == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE || t == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
}
// Resolve descriptor-array spill across consecutive compatible bindings.
inline bool slot(const Layout &layout, uint32_t &binding, uint32_t &element, VkDescriptorType type) {
  auto i = layout.find(binding);
  if (i == layout.end()) return false;
  const auto stages = i->second.stages;
  while (i != layout.end()) {
    if (i->second.type != type || i->second.stages != stages) return false;
    if (element < i->second.count) { binding = i->first; return true; }
    element -= i->second.count;
    ++i;
  }
  return false;
}
inline void write(Set &set, uint32_t binding, uint32_t element, uint32_t count,
                  VkDescriptorType type, const void *data, size_t stride) {
  if (!set.valid || !sampled(type)) return;
  ++set.revision;
  if (count > 4096) { set.valid = false; set.images.clear(); return; }
  for (uint32_t n = 0; n < count; ++n, ++element) {
    if (!slot(set.layout, binding, element, type)) { set.valid = false; set.images.clear(); return; }
    VkDescriptorImageInfo info{};
    if (data) std::memcpy(&info, static_cast<const char*>(data) + size_t(n) * stride, sizeof(info));
    auto key = std::make_pair(binding, element);
    if (info.imageView && (set.layout.at(binding).stages & VK_SHADER_STAGE_FRAGMENT_BIT)) {
      if (set.images.size() >= 4096 && !set.images.count(key)) { set.valid = false; set.images.clear(); return; }
      set.images[key] = info.imageView;
    } else set.images.erase(key);
  }
}
inline void update(Set &set, const Template &t, const void *data) {
  for (const auto &e : t.entries)
    if (sampled(e.descriptorType)) write(set, e.dstBinding, e.dstArrayElement, e.descriptorCount,
        e.descriptorType, static_cast<const char*>(data) + e.offset, e.stride);
}
inline void copy(const Set &src, Set &dst, const VkCopyDescriptorSet &c) {
  auto b = src.layout.find(c.srcBinding);
  if (!src.valid || b == src.layout.end() || c.descriptorCount > 4096) { dst.valid = false; dst.images.clear(); return; }
  if (!sampled(b->second.type)) return;
  const auto type = b->second.type;
  uint32_t sb = c.srcBinding, se = c.srcArrayElement;
  std::vector<VkDescriptorImageInfo> values(c.descriptorCount);
  for (auto &v : values) {
    if (!slot(src.layout, sb, se, type)) { dst.valid = false; dst.images.clear(); return; }
    auto i = src.images.find({sb, se++});
    if (i != src.images.end()) v.imageView = i->second;
  }
  write(dst, c.dstBinding, c.dstArrayElement, c.descriptorCount, type, values.data(), sizeof(VkDescriptorImageInfo));
}
}
