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

#include <vector>
// Capability gate for cross-driver subsampled transport. No submissions.
int main(int argc, char **argv) {
  setbuf(stdout, nullptr);
  if (argc != 2)
    return 2;
  auto lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
  if (!lib) {
    puts(dlerror());
    return 3;
  }
  auto get = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
  if (!get)
    get = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
  if (!get) {
    auto hal = (ProbeHalModule *)dlsym(lib, "HMI");
    ProbeHalDevice *d = nullptr;
    if (hal && hal->methods && !hal->methods->open(hal, "vk0", &d))
      get = d->get;
  }
  if (!get)
    return 4;
  auto create = (PFN_vkCreateInstance)get(nullptr, "vkCreateInstance");
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_1;
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;
  VkInstance inst{};
  auto r = create(&ci, nullptr, &inst);
  printf("instance=%d\n", r);
  if (r)
    return 5;
#define I(name) auto name = (PFN_vk##name)get(inst, "vk" #name)
  I(EnumeratePhysicalDevices);
  I(GetPhysicalDeviceProperties);
  I(GetPhysicalDeviceFeatures2);
  I(EnumerateDeviceExtensionProperties);
  I(GetPhysicalDeviceImageFormatProperties2);
  I(DestroyInstance);
  uint32_t n = 1;
  VkPhysicalDevice phys{};
  if (EnumeratePhysicalDevices(inst, &n, &phys))
    return 6;
  VkPhysicalDeviceProperties props{};
  GetPhysicalDeviceProperties(phys, &props);
  printf("GPU=%s api=%x driver=%x\n", props.deviceName, props.apiVersion,
         props.driverVersion);
  n = 0;
  EnumerateDeviceExtensionProperties(phys, nullptr, &n, nullptr);
  std::vector<VkExtensionProperties> extensions(n);
  EnumerateDeviceExtensionProperties(phys, nullptr, &n, extensions.data());
  auto has = [&](const char *s) {
    for (auto &e : extensions)
      if (!strcmp(e.extensionName, s))
        return true;
    return false;
  };
  bool density = has(VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME),
       ahb = has(
           VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME);
  printf("FDM extension=%d map2=%d AHB=%d DMA_BUF=%d modifiers=%d\n", density,
         has(VK_EXT_FRAGMENT_DENSITY_MAP_2_EXTENSION_NAME), ahb,
         has(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME),
         has(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME));
  if (density) {
    VkPhysicalDeviceFragmentDensityMapFeaturesEXT df{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f.pNext = &df;
    GetPhysicalDeviceFeatures2(phys, &f);
    printf("FDM feature=%u nonSubsampled=%u\n", df.fragmentDensityMap,
           df.fragmentDensityMapNonSubsampledImages);
  }
  for (auto fmt : {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB,
                   VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB})
    for (bool sub : {false, true})
      for (bool external : {false, true}) {
        if (sub && !density)
          continue;
        if (external && !ahb)
          continue;
        VkPhysicalDeviceExternalImageFormatInfo eq{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        eq.handleType =
            VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
        VkPhysicalDeviceImageFormatInfo2 q{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        q.pNext = external ? &eq : nullptr;
        q.format = fmt;
        q.type = VK_IMAGE_TYPE_2D;
        q.tiling = VK_IMAGE_TILING_OPTIMAL;
        q.usage =
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        q.flags = sub ? VK_IMAGE_CREATE_SUBSAMPLED_BIT_EXT : 0;
        VkExternalImageFormatProperties ep{
            VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkAndroidHardwareBufferUsageANDROID usage{
            VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID};
        ep.pNext = &usage;
        VkImageFormatProperties2 p{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        p.pNext = external ? &ep : nullptr;
        r = GetPhysicalDeviceImageFormatProperties2(phys, &q, &p);
        printf("fmt=%d subsampled=%d ahb=%d query=%d import=%d export=%d "
               "dedicated=%d ahbUsage=%llx max=%ux%u layers=%u\n",
               fmt, sub, external, r,
               !!(ep.externalMemoryProperties.externalMemoryFeatures &
                  VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT),
               !!(ep.externalMemoryProperties.externalMemoryFeatures &
                  VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT),
               !!(ep.externalMemoryProperties.externalMemoryFeatures &
                  VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT),
               (unsigned long long)usage.androidHardwareBufferUsage,
               p.imageFormatProperties.maxExtent.width,
               p.imageFormatProperties.maxExtent.height,
               p.imageFormatProperties.maxArrayLayers);
      }
  DestroyInstance(inst, nullptr);
  return 0;
}
