// Included by layer.cpp; these hooks observe descriptors and forward unchanged.
#define DESCRIPTOR_NEXT(name) d->name
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice h, const VkDescriptorSetLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorSetLayout *out) {
  std::lock_guard<std::recursive_mutex> guard(lock); auto d = dev(h);
  auto result = DESCRIPTOR_NEXT(vkCreateDescriptorSetLayout)(h, ci, a, out);
  if (result == VK_SUCCESS && sampledTracking(d)) {
    auto &layout = d->descriptorLayouts[*out];
    for (unsigned j = 0; j < ci->bindingCount; ++j) {
      const auto &b = ci->pBindings[j]; layout[b.binding] = {b.descriptorType, b.descriptorCount, b.stageFlags};
    }
  }
  return result;
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice h, VkDescriptorSetLayout layout, const VkAllocationCallbacks *a) {
  std::lock_guard<std::recursive_mutex> guard(lock); auto d = dev(h);
  d->descriptorLayouts.erase(layout); DESCRIPTOR_NEXT(vkDestroyDescriptorSetLayout)(h, layout, a);
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice h, const VkPipelineLayoutCreateInfo *ci, const VkAllocationCallbacks *a, VkPipelineLayout *out) {
  std::lock_guard<std::recursive_mutex> guard(lock); auto d = dev(h);
  auto result = DESCRIPTOR_NEXT(vkCreatePipelineLayout)(h, ci, a, out);
  if (result == VK_SUCCESS && sampledTracking(d)) {
    auto &layouts = d->pipelineLayouts[*out];
    for (unsigned j = 0; j < ci->setLayoutCount; ++j) {
      auto l = d->descriptorLayouts.find(ci->pSetLayouts[j]);
      layouts.push_back(l == d->descriptorLayouts.end() ? descriptor_trace::Layout{} : l->second);
    }
  }
  return result;
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice h, VkPipelineLayout layout, const VkAllocationCallbacks *a) {
  std::lock_guard<std::recursive_mutex> guard(lock); auto d = dev(h);
  d->pipelineLayouts.erase(layout); DESCRIPTOR_NEXT(vkDestroyPipelineLayout)(h, layout, a);
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice h, const VkDescriptorSetAllocateInfo *ci, VkDescriptorSet *out) {
  std::lock_guard<std::recursive_mutex> guard(lock); auto d = dev(h);
  auto result = DESCRIPTOR_NEXT(vkAllocateDescriptorSets)(h, ci, out);
  if (result == VK_SUCCESS && sampledTracking(d)) for (unsigned j = 0; j < ci->descriptorSetCount; ++j) {
    auto state = std::make_shared<descriptor_trace::Set>(); state->pool = ci->descriptorPool;
    auto l = d->descriptorLayouts.find(ci->pSetLayouts[j]);
    if (l != d->descriptorLayouts.end()) state->layout = l->second; else state->valid = false;
    d->descriptorSets[out[j]] = std::move(state);
  }
  return result;
}
static void forgetDescriptorPool(Device *d, VkDescriptorPool pool) {
  for (auto i = d->descriptorSets.begin(); i != d->descriptorSets.end();) {
    if (i->second->pool == pool) { i->second->valid = false; i->second->images.clear(); i = d->descriptorSets.erase(i); }
    else ++i;
  }
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkFreeDescriptorSets(VkDevice h, VkDescriptorPool pool, uint32_t count, const VkDescriptorSet *sets) {
  std::lock_guard<std::recursive_mutex> guard(lock); auto d = dev(h);
  auto result = DESCRIPTOR_NEXT(vkFreeDescriptorSets)(h, pool, count, sets);
  if (result == VK_SUCCESS) for (unsigned j = 0; j < count; ++j) {
    auto i = d->descriptorSets.find(sets[j]);
    if (i != d->descriptorSets.end()) { i->second->valid = false; i->second->images.clear(); d->descriptorSets.erase(i); }
  }
  return result;
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkResetDescriptorPool(VkDevice h, VkDescriptorPool pool, VkDescriptorPoolResetFlags flags) {
  std::lock_guard<std::recursive_mutex> guard(lock); auto d = dev(h);
  auto result = DESCRIPTOR_NEXT(vkResetDescriptorPool)(h, pool, flags);
  if (result == VK_SUCCESS) forgetDescriptorPool(d, pool);
  return result;
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice h, VkDescriptorPool pool, const VkAllocationCallbacks *a) {
  std::lock_guard<std::recursive_mutex> guard(lock); auto d = dev(h);
  forgetDescriptorPool(d, pool); DESCRIPTOR_NEXT(vkDestroyDescriptorPool)(h, pool, a);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice h, uint32_t nw, const VkWriteDescriptorSet *writes, uint32_t nc, const VkCopyDescriptorSet *copies) {
  std::unique_lock<std::recursive_mutex> guard(lock); auto d = dev(h);
  if (sampledTracking(d)) {
    for (unsigned j = 0; j < nw; ++j) {
      const auto &w = writes[j]; auto i = d->descriptorSets.find(w.dstSet);
      if (i != d->descriptorSets.end()) descriptor_trace::write(*i->second, w.dstBinding, w.dstArrayElement,
          w.descriptorCount, w.descriptorType, w.pImageInfo, sizeof(VkDescriptorImageInfo));
    }
    for (unsigned j = 0; j < nc; ++j) {
      const auto &c = copies[j]; auto src = d->descriptorSets.find(c.srcSet), dst = d->descriptorSets.find(c.dstSet);
      if (dst == d->descriptorSets.end()) continue;
      if (src == d->descriptorSets.end()) { dst->second->valid = false; dst->second->images.clear(); }
      else descriptor_trace::copy(*src->second, *dst->second, c);
    }
  }
  auto forward = DESCRIPTOR_NEXT(vkUpdateDescriptorSets);
  guard.unlock();
  forward(h, nw, writes, nc, copies);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer h, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t first, uint32_t count, const VkDescriptorSet *sets, uint32_t dynamicCount, const uint32_t *offsets) {
  std::unique_lock<std::recursive_mutex> guard(lock); auto d = dev(h);
  if (sampledTracking(d) && point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
    auto &c = d->commands[h];
    // Conservatively drop previous bindings on any layout change. Missing
    // candidates are preferable to reporting incompatible/stale descriptors.
    if (c.descriptorLayout != layout) c.descriptorSets = {};
    c.descriptorLayout = layout;
    if (uint64_t(first) + count > c.descriptorSets.size()) c.descriptorOverflow = true;
    for (unsigned j = 0; j < count && uint64_t(first) + j < c.descriptorSets.size(); ++j) {
      auto i = d->descriptorSets.find(sets[j]);
      if (i == d->descriptorSets.end()) c.descriptorOverflow = true;
      c.descriptorSets[first+j] = i == d->descriptorSets.end() ? nullptr : i->second;
    }
  }
  auto forward = DESCRIPTOR_NEXT(vkCmdBindDescriptorSets);
  guard.unlock();
  forward(h, point, layout, first, count, sets, dynamicCount, offsets);
}
EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorUpdateTemplate(VkDevice h, const VkDescriptorUpdateTemplateCreateInfo *ci, const VkAllocationCallbacks *a, VkDescriptorUpdateTemplate *out) {
  std::lock_guard<std::recursive_mutex> guard(lock); auto d = dev(h);
  auto fn = DESCRIPTOR_NEXT(vkCreateDescriptorUpdateTemplate);
  auto result = fn(h, ci, a, out);
  if (result == VK_SUCCESS && sampledTracking(d)) {
    auto &t = d->descriptorTemplates[*out]; t.type = ci->templateType; t.set = ci->set; t.bindPoint = ci->pipelineBindPoint;
    t.entries.assign(ci->pDescriptorUpdateEntries, ci->pDescriptorUpdateEntries + ci->descriptorUpdateEntryCount);
    if (ci->templateType == VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET) {
      auto l = d->descriptorLayouts.find(ci->descriptorSetLayout); if (l != d->descriptorLayouts.end()) t.layout = l->second;
    } else {
      auto l = d->pipelineLayouts.find(ci->pipelineLayout);
      if (l != d->pipelineLayouts.end() && ci->set < l->second.size()) t.layout = l->second[ci->set];
    }
  }
  return result;
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorUpdateTemplate(VkDevice h, VkDescriptorUpdateTemplate t, const VkAllocationCallbacks *a) {
  std::lock_guard<std::recursive_mutex> guard(lock); auto d = dev(h); d->descriptorTemplates.erase(t);
  auto fn = DESCRIPTOR_NEXT(vkDestroyDescriptorUpdateTemplate);
  fn(h, t, a);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSetWithTemplate(VkDevice h, VkDescriptorSet set, VkDescriptorUpdateTemplate t, const void *data) {
  std::unique_lock<std::recursive_mutex> guard(lock); auto d = dev(h);
  auto i = d->descriptorSets.find(set); auto info = d->descriptorTemplates.find(t);
  if (sampledTracking(d) && i != d->descriptorSets.end()) {
    if (info != d->descriptorTemplates.end()) descriptor_trace::update(*i->second, info->second, data);
    else { i->second->valid = false; i->second->images.clear(); }
  }
  auto fn = DESCRIPTOR_NEXT(vkUpdateDescriptorSetWithTemplate);
  auto forward = fn;
  guard.unlock();
  forward(h, set, t, data);
}
static descriptor_trace::Set *pushState(Device *d, Commands &c, VkPipelineLayout layout, uint32_t set) {
  if (set >= c.descriptorSets.size()) return nullptr;
  if (c.descriptorLayout != layout) c.descriptorSets = {};
  c.descriptorLayout = layout;
  auto &state = c.descriptorSets[set];
  if (!state) {
    state = std::make_shared<descriptor_trace::Set>();
    auto l = d->pipelineLayouts.find(layout);
    if (l != d->pipelineLayouts.end() && set < l->second.size()) state->layout = l->second[set];
    else state->valid = false;
  }
  return state.get();
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetKHR(VkCommandBuffer h, VkPipelineBindPoint point, VkPipelineLayout layout, uint32_t set, uint32_t count, const VkWriteDescriptorSet *writes) {
  std::unique_lock<std::recursive_mutex> guard(lock); auto d = dev(h);
  if (sampledTracking(d) && point == VK_PIPELINE_BIND_POINT_GRAPHICS) {
    auto state = pushState(d, d->commands[h], layout, set);
    if (state) for (unsigned j = 0; j < count; ++j) {
      auto &w = writes[j]; descriptor_trace::write(*state, w.dstBinding, w.dstArrayElement, w.descriptorCount,
          w.descriptorType, w.pImageInfo, sizeof(VkDescriptorImageInfo));
    }
  }
  auto forward = DESCRIPTOR_NEXT(vkCmdPushDescriptorSetKHR);
  guard.unlock();
  forward(h, point, layout, set, count, writes);
}
EXPORT VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetWithTemplateKHR(VkCommandBuffer h, VkDescriptorUpdateTemplate t, VkPipelineLayout layout, uint32_t set, const void *data) {
  std::unique_lock<std::recursive_mutex> guard(lock); auto d = dev(h);
  if (sampledTracking(d)) {
    auto info = d->descriptorTemplates.find(t);
    auto state = info != d->descriptorTemplates.end() && info->second.bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS
        ? pushState(d, d->commands[h], layout, set) : nullptr;
    if (state) {
      if (info != d->descriptorTemplates.end()) descriptor_trace::update(*state, info->second, data);
      else { state->valid = false; state->images.clear(); }
    }
  }
  auto forward = DESCRIPTOR_NEXT(vkCmdPushDescriptorSetWithTemplateKHR);
  guard.unlock();
  forward(h, t, layout, set, data);
}
#undef DESCRIPTOR_NEXT
