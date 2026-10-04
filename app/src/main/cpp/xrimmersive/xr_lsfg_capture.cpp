#include "xr_lsfg_capture.h"
#include "../../windows/openxr_runtime/gamenative_foveated_packing.h"
#include "xr_lsfg_capture_spv.h"
#include "xr_frame_interpolation.h"
#include <algorithm>

#include "xr_vulkan_dispatch.h"

namespace xrimmersive::windowsvr {
struct VulkanEyeCapture::Impl {
    VkDevice device;
    PFN_vkGetAndroidHardwareBufferPropertiesANDROID properties;
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout layout{};
    VkPipeline pipelines[2]{};
    VkDescriptorPool pool{};
    VkDescriptorSet sets[4]{};
    VkSampler sampler{};
    struct Source {
        AHardwareBuffer* buffer{};
        VkImage image{};
        VkImageView view{};
        VkDeviceMemory memory{};
        uint64_t registration{};
    };
    std::array<std::array<Source, WindowsFrameTransport::kMaxImages>, 2> sources{};
    Source* selected[2]{};
    Impl(VkDevice d, PFN_vkGetAndroidHardwareBufferPropertiesANDROID props) : device(d), properties(props) {}
    void destroy(Source& s) {
        if (s.view) vkDestroyImageView(device, s.view, nullptr);
        if (s.image) vkDestroyImage(device, s.image, nullptr);
        if (s.memory) vkFreeMemory(device, s.memory, nullptr);
        if (s.buffer) AHardwareBuffer_release(s.buffer);
        s = {};
    }
    ~Impl() {
        for (auto& eye : sources) for (auto& s : eye) destroy(s);
        for (auto pipeline : pipelines) if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
        if (layout) vkDestroyPipelineLayout(device, layout, nullptr);
        if (pool) vkDestroyDescriptorPool(device, pool, nullptr);
        if (setLayout) vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
        if (sampler) vkDestroySampler(device, sampler, nullptr);
    }
    bool init() {
        VkDescriptorSetLayoutBinding bindings[] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
            {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        si.bindingCount = 3; si.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(device, &si, nullptr, &setLayout) != VK_SUCCESS) return false;
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1; li.pSetLayouts = &setLayout;
        li.pushConstantRangeCount = 1; li.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(device, &li, nullptr, &layout) != VK_SUCCESS) return false;
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 4; pi.poolSizeCount = 2; pi.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(device, &pi, nullptr, &pool) != VK_SUCCESS) return false;
        VkDescriptorSetLayout layouts[] = {setLayout, setLayout, setLayout, setLayout};
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = pool; ai.descriptorSetCount = 4; ai.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(device, &ai, sets) != VK_SUCCESS) return false;
        VkSamplerCreateInfo sm{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sm.magFilter = sm.minFilter = VK_FILTER_LINEAR;
        sm.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sm.addressModeU = sm.addressModeV = sm.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(device, &sm, nullptr, &sampler) != VK_SUCCESS) return false;
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = sizeof(kCaptureSpv); mi.pCode = kCaptureSpv;
        VkShaderModule module{};
        if (vkCreateShaderModule(device, &mi, nullptr, &module) != VK_SUCCESS) return false;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.layout = layout;
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                    VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
        VkBool32 packed = VK_FALSE;
        const VkSpecializationMapEntry entry{1, 0, sizeof(packed)};
        const VkSpecializationInfo specialization{1, &entry, sizeof(packed), &packed};
        ci.stage.pSpecializationInfo = &specialization;
        VkResult result = VK_SUCCESS;
        for (packed = 0; packed < 2; ++packed) {
            result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipelines[packed]);
            if (result != VK_SUCCESS) break;
        }
        vkDestroyShaderModule(device, module, nullptr);
        return result == VK_SUCCESS;
    }
    bool importSource(Source& s, const EyeFrame& f) {
        if (s.buffer == f.buffer && s.registration == f.registrationSerial) return s.view != VK_NULL_HANDLE;
        destroy(s);
        // Cache unsupported imports too, until transport registration changes.
        // This avoids repeating driver queries/allocations every game frame.
        s.buffer = f.buffer; AHardwareBuffer_acquire(s.buffer);
        s.registration = f.registrationSerial;
        AHardwareBuffer_Desc desc{};
        AHardwareBuffer_describe(f.buffer, &desc);
        if (!desc.layers || f.bufferLayer >= desc.layers || desc.width != uint32_t(f.foveatedPacked ? gn_packed_extent(f.width) : f.width) || desc.height != uint32_t(f.foveatedPacked ? gn_packed_extent(f.height) : f.height) ||
            !(desc.usage & AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE)) return false;
        VkAndroidHardwareBufferFormatPropertiesANDROID fmt{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID};
        VkAndroidHardwareBufferPropertiesANDROID props{VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID};
        props.pNext = &fmt;
        if (properties(device, f.buffer, &props) != VK_SUCCESS || !props.memoryTypeBits ||
            (fmt.format != VK_FORMAT_R8G8B8A8_UNORM && fmt.format != VK_FORMAT_B8G8R8A8_UNORM) ||
            !(fmt.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
            !(fmt.formatFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) return false;
        VkExternalMemoryImageCreateInfo ext{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
        VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ii.pNext = &ext; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = fmt.format;
        ii.extent = {desc.width, desc.height, 1}; ii.mipLevels = 1; ii.arrayLayers = desc.layers;
        ii.samples = VK_SAMPLE_COUNT_1_BIT; ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        if (vkCreateImage(device, &ii, nullptr, &s.image) != VK_SUCCESS) return false;
        VkImportAndroidHardwareBufferInfoANDROID imp{VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID};
        imp.buffer = s.buffer;
        VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated.image = s.image; dedicated.pNext = &imp;
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.pNext = &dedicated; ai.allocationSize = props.allocationSize;
        ai.memoryTypeIndex = __builtin_ctz(props.memoryTypeBits);
        if (vkAllocateMemory(device, &ai, nullptr, &s.memory) != VK_SUCCESS ||
            vkBindImageMemory(device, s.image, s.memory, 0) != VK_SUCCESS) return false;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = s.image; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = fmt.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, f.bufferLayer, 1};
        return vkCreateImageView(device, &vi, nullptr, &s.view) == VK_SUCCESS;
    }
};
VulkanEyeCapture::VulkanEyeCapture(VkDevice d, PFN_vkGetAndroidHardwareBufferPropertiesANDROID p, bool capturePipeline)
    : p_(std::make_unique<Impl>(d, p)) { if (capturePipeline && !p_->init()) p_.reset(); }
VulkanEyeCapture::~VulkanEyeCapture() = default;
bool VulkanEyeCapture::valid() const { return bool(p_); }
bool VulkanEyeCapture::probe(VkDevice device, PFN_vkGetAndroidHardwareBufferPropertiesANDROID properties, const EyeFrame &frame) {
    if (!device || !properties || !frame.buffer) return false;
    Impl importer(device, properties);
    Impl::Source source{};
    bool ok = importer.importSource(source, frame);
    importer.destroy(source);
    return ok;
}
bool VulkanEyeCapture::import(const std::array<EyeFrame, 2>& frames) {
    if (!p_) return false;
    // Preflight both eyes before touching any cached import.
    for (const auto& f : frames)
        if (f.kind != BufferKind::HardwareBuffer || !f.buffer || f.width <= 0 || f.height <= 0 ||
            f.imageIndex < 0 || f.imageIndex >= WindowsFrameTransport::kMaxImages) return false;
    for (int e = 0; e < 2; ++e) {
        auto& s = p_->sources[e][frames[e].imageIndex];
        if (!p_->importSource(s, frames[e])) return false;
        p_->selected[e] = &s;
    }
    return true;
}
VkImage VulkanEyeCapture::sourceImage(int e) const { return p_->selected[e]->image; }
VkImageView VulkanEyeCapture::sourceView(int e) const { return p_->selected[e]->view; }
void VulkanEyeCapture::record(VkCommandBuffer cmd, unsigned slot, VkImageView src, VkImageView aligned,
                             VkImageView real, const EyeFrame& f, const EyeFrame& target,
                             int width, int height, bool captureReal, bool align) {
    struct Params { float crop[4], rotation[3][4], sourceFov[4], targetFov[4]; int flags[4]; float packing[4]; } push{};
    static_assert(sizeof(push) == 128);
    float w = f.sourceWidth > 0 ? f.sourceWidth : f.width;
    float h = f.sourceHeight > 0 ? f.sourceHeight : f.height;
    push.crop[0] = float(f.sourceX) / f.width;
    push.crop[1] = (f.sourceY + (f.flipY ? h : 0)) / f.height;
    push.crop[2] = w / f.width; push.crop[3] = (f.flipY ? -h : h) / f.height;
    auto rotation = targetToSourceRotation(f.projectionOrientation, target.projectionOrientation);
    for (int c = 0; c < 3; ++c) for (int r = 0; r < 3; ++r) push.rotation[c][r] = rotation[c*3+r];
    for (int i = 0; i < 4; ++i) {
        push.sourceFov[i] = std::tan(f.projectionFov[i]); push.targetFov[i] = std::tan(target.projectionFov[i]);
    }
    push.rotation[0][3] = float(f.width); push.rotation[1][3] = float(f.height);
    push.flags[0] = align; push.flags[1] = captureReal; push.flags[2] = f.swapRedBlue; push.flags[3] = 0;
    gn_packing_parameters(f.width, f.height, push.packing);
    VkDescriptorImageInfo infos[] = {{p_->sampler, src, VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE, aligned, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, real, VK_IMAGE_LAYOUT_GENERAL}};
    VkWriteDescriptorSet writes[3]{};
    for (int i = 0; i < 3; ++i) {
        writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        writes[i].dstSet = p_->sets[slot]; writes[i].dstBinding = i; writes[i].descriptorCount = 1;
        writes[i].descriptorType = (i > 0) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(p_->device, 3, writes, 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p_->pipelines[f.foveatedPacked ? 1 : 0]);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p_->layout, 0, 1, &p_->sets[slot], 0, nullptr);
    vkCmdPushConstants(cmd, p_->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
    vkCmdDispatch(cmd, (width+7)/8, (height+7)/8, 1);
}


}
