// Read-only capability check. No logical device, allocations, or GPU submissions.
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <vector>

int main() {
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "GameNative VR direct import probe";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ci.pApplicationInfo = &app;
    VkInstance instance{};
    VkResult status = vkCreateInstance(&ci, nullptr, &instance);
    if (status != VK_SUCCESS) {
        std::printf("vkCreateInstance failed: %d\n", status);
        return 1;
    }
    uint32_t count = 0;
    status = vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (status != VK_SUCCESS || !count) {
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    std::vector<VkPhysicalDevice> physical(count);
    status = vkEnumeratePhysicalDevices(instance, &count, physical.data());
    if (status != VK_SUCCESS) {
        vkDestroyInstance(instance, nullptr);
        return 1;
    }
    bool failed = false;
    for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physical[i], &properties);
        std::printf("GPU=%s Vulkan=%u.%u.%u driver=0x%x\n", properties.deviceName,
            VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion),
            VK_VERSION_PATCH(properties.apiVersion), properties.driverVersion);
        uint32_t n = 0;
        status = vkEnumerateDeviceExtensionProperties(physical[i], nullptr, &n, nullptr);
        if (status != VK_SUCCESS) { failed = true; continue; }
        std::vector<VkExtensionProperties> extensions(n);
        status = vkEnumerateDeviceExtensionProperties(physical[i], nullptr, &n, extensions.data());
        if (status != VK_SUCCESS) { failed = true; continue; }
        auto has = [&](const char *name) {
            for (uint32_t j = 0; j < n; ++j)
                if (!std::strcmp(extensions[j].extensionName, name)) return true;
            return false;
        };
        const bool dma = has("VK_EXT_external_memory_dma_buf");
        const bool fd = has("VK_KHR_external_memory_fd");
        const bool ahb = has("VK_ANDROID_external_memory_android_hardware_buffer");
        std::printf("DMA_BUF=%d external_memory_fd=%d AHardwareBuffer=%d drm_modifiers=%d foreign_queue=%d\n",
            dma, fd, ahb, has("VK_EXT_image_drm_format_modifier"), has("VK_EXT_queue_family_foreign"));
        if (!dma || !fd)
            std::puts("Direct DMA-BUF import is unavailable through this driver's advertised Vulkan API.");
        struct Handle { const char *name; VkExternalMemoryHandleTypeFlagBits type; bool supported; };
        const Handle handles[] = {
            {"DMA_BUF", VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dma && fd},
            {"AHardwareBuffer", VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID, ahb}
        };
        for (auto format : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM}) {
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(physical[i], format, &fp);
            std::printf("format=%d linearFeatures=0x%x optimalFeatures=0x%x\n",
                format, fp.linearTilingFeatures, fp.optimalTilingFeatures);
            for (const auto &handle : handles) {
                if (!handle.supported) continue;
                VkPhysicalDeviceExternalImageFormatInfo external{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
                external.handleType = handle.type;
                VkPhysicalDeviceImageFormatInfo2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
                query.pNext = &external;
                query.format = format;
                query.type = VK_IMAGE_TYPE_2D;
                query.tiling = handle.type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
                    ? VK_IMAGE_TILING_LINEAR : VK_IMAGE_TILING_OPTIMAL;
                query.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
                VkExternalImageFormatProperties externalResult{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
                VkImageFormatProperties2 result{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
                result.pNext = &externalResult;
                status = vkGetPhysicalDeviceImageFormatProperties2(physical[i], &query, &result);
                std::printf("  %s sampled result=%d importable=%d extent=%ux%u layers=%u\n",
                    handle.name, status,
                    status == VK_SUCCESS && (externalResult.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT),
                    result.imageFormatProperties.maxExtent.width, result.imageFormatProperties.maxExtent.height,
                    result.imageFormatProperties.maxArrayLayers);
            }
        }
    }
    vkDestroyInstance(instance, nullptr);
    return failed ? 1 : 0;
}
