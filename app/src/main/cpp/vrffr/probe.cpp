// Separate feature-query process: no guest handles, device creation or draws.
// GN_VK_PROBE_LIBRARY optionally selects a wrapper or Android HAL library.
#include "capabilities.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
// Android Vulkan HAL ABI (64-bit). Some Android drivers export only HMI,
// rather than the desktop ICD entry points. Keep this fallback probe-only.
struct ProbeHalDevice;
struct ProbeHalModule;
struct ProbeHalMethods { int (*open)(const ProbeHalModule *, const char *, ProbeHalDevice **); };
struct ProbeHalModule {
  uint32_t tag; uint16_t moduleVersion, halVersion;
  const char *id, *name, *author; ProbeHalMethods *methods; void *dso;
  uint64_t reserved[25];
};
struct ProbeHalDevice {
  uint32_t tag, version; ProbeHalModule *module; uint64_t reserved[12];
  int (*close)(ProbeHalDevice *);
  PFN_vkEnumerateInstanceExtensionProperties enumerate;
  PFN_vkCreateInstance create;
  PFN_vkGetInstanceProcAddr get;
};
int main(int argc, char **argv) {
  setbuf(stdout, nullptr);
  if (argc != 6) return 2;
  // Adrenotools expects the shared Android C++ runtime in the global scope.
  // The probe itself uses a static runtime and otherwise has no such dependency.
  void *runtime = dlopen(argv[5], RTLD_NOW | RTLD_GLOBAL);
  if (!runtime) { printf("direct probe: C++ runtime load failed: %s\n", dlerror()); return 12; }
  printf("direct probe: C++ runtime loaded: %s\n", argv[5]);
  const char *library = getenv("GN_VK_PROBE_LIBRARY");
  void *driver = nullptr;
  if (library) {
    driver = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    if (!driver) { printf("library probe: load failed: %s\n", dlerror()); return 5; }
  } else {
    const std::string toolsPath = std::string(argv[1]) + "/libadrenotools.so";
    void *tools = dlopen(toolsPath.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!tools) { printf("direct probe: adrenotools load failed: %s\n", dlerror()); return 3; }
    using Open = void *(*)(int, int, const char *, const char *, const char *, const char *, const char *, void **);
    auto open = reinterpret_cast<Open>(dlsym(tools, "adrenotools_open_libvulkan"));
    if (!open) { puts("direct probe: adrenotools export missing"); return 4; }
    // ADRENOTOOLS_DRIVER_CUSTOM = 1; same ABI as the application's existing loader.
    driver = open(RTLD_NOW | RTLD_LOCAL, 1, argv[4], argv[1], argv[2], argv[3], nullptr, nullptr);
    if (!driver) { puts("direct probe: custom driver load failed"); return 5; }
  }
  auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(driver, "vkGetInstanceProcAddr"));
  if (!get) get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(driver, "vk_icdGetInstanceProcAddr"));
  if (!get && library) {
    auto *hal = static_cast<ProbeHalModule *>(dlsym(driver, "HMI"));
    ProbeHalDevice *device = nullptr;
    if (hal && hal->tag == 0x48574d54 && hal->id && !strcmp(hal->id, "vulkan") &&
        hal->methods && hal->methods->open &&
        hal->methods->open(hal, "vk0", &device) == 0 && device) get = device->get;
  }
  if (!get) { puts("direct probe: vkGetInstanceProcAddr missing"); return 6; }
  auto create = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
  if (!create) return 7;
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "GameNative FFR direct driver probe";
  app.apiVersion = VK_API_VERSION_1_2;
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;
  VkInstance instance{};
  auto result = create(&ci, nullptr, &instance);
  printf("direct probe: driver=%s/%s createInstance=%d\n", argv[2], argv[3], result);
  if (result != VK_SUCCESS) return 8;
  auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(get(instance, "vkEnumeratePhysicalDevices"));
  auto destroy = reinterpret_cast<PFN_vkDestroyInstance>(get(instance, "vkDestroyInstance"));
  auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(get(instance, "vkGetPhysicalDeviceProperties"));
  auto features = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(get(instance, "vkGetPhysicalDeviceFeatures2"));
  auto extensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(get(instance, "vkEnumerateDeviceExtensionProperties"));
  if (!enumerate || !destroy || !props || !features || !extensions) return 9;
  uint32_t count = 0;
  result = enumerate(instance, &count, nullptr);
  if (result != VK_SUCCESS || !count || count > 32) { destroy(instance, nullptr); return 10; }
  std::vector<VkPhysicalDevice> physical(count);
  result = enumerate(instance, &count, physical.data());
  if (result != VK_SUCCESS) { destroy(instance, nullptr); return 11; }
  for (uint32_t i = 0; i < count; ++i) {
    VkPhysicalDeviceProperties p{};
    props(physical[i], &p);
    // Separate queries: Vulkan12 and the promoted extension structure must not
    // occur together in the same pNext chain.
    VkPhysicalDeviceVulkan12Features v12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    VkPhysicalDeviceFeatures2 core{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    core.pNext = &v12;
    features(physical[i], &core);
    VkPhysicalDevice8BitStorageFeatures bytes{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES};
    VkPhysicalDeviceFeatures2 promoted{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    promoted.pNext = &bytes;
    features(physical[i], &promoted);
    printf("storage probe: path=%s GPU=%s api=%u.%u.%u pushConstants=%u "
           "core12.storageBuffer8BitAccess=%u extension.storageBuffer8BitAccess=%u "
           "core12.uniformAndStorageBuffer8BitAccess=%u extension.uniformAndStorageBuffer8BitAccess=%u "
           "core12.storagePushConstant8=%u extension.storagePushConstant8=%u "
           "shaderInt8=%u shaderInt16=%u shaderInt64=%u scalarBlockLayout=%u descriptorIndexing=%u\n",
           library ? library : "adrenotools-direct", p.deviceName,
           VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion),
           p.limits.maxPushConstantsSize, v12.storageBuffer8BitAccess, bytes.storageBuffer8BitAccess,
           v12.uniformAndStorageBuffer8BitAccess, bytes.uniformAndStorageBuffer8BitAccess,
           v12.storagePushConstant8, bytes.storagePushConstant8, v12.shaderInt8,
           core.features.shaderInt16, core.features.shaderInt64, v12.scalarBlockLayout, v12.descriptorIndexing);
    uint32_t n = 0;
    if (extensions(physical[i], nullptr, &n, nullptr) != VK_SUCCESS || n > 4096) continue;
    std::vector<VkExtensionProperties> ext(n);
    if (extensions(physical[i], nullptr, &n, ext.data()) != VK_SUCCESS) continue;
    bool fdm = false, wide = p.apiVersion >= VK_API_VERSION_1_3;
    for (const auto &e : ext) {
      fdm |= !strcmp(e.extensionName, VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME);
      wide |= !strcmp(e.extensionName, VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME);
    }
    VkPhysicalDeviceFragmentDensityMapFeaturesEXT f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    if (fdm) { f2.pNext = &f; features(physical[i], &f2); }
    const auto q = ffr::queryFormat(get, instance, physical[i], wide);
    printf("format queries: path=direct GPU=%s api=%u.%u.%u extension=%d feature=%u regularImages=%u "
           "legacy=0x%x properties2=%d optimal2=0x%x properties3=%d optimal3=0x%llx "
           "imageQuery=%d imageResult=%d maxExtent=%ux%u maxLayers=%u samples=0x%x source=%s\n",
           p.deviceName, VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion), VK_VERSION_PATCH(p.apiVersion),
           fdm, f.fragmentDensityMap, f.fragmentDensityMapNonSubsampledImages,
           q.legacy.optimalTilingFeatures, q.hasModern, q.modern.formatProperties.optimalTilingFeatures,
           q.hasWide, (unsigned long long)q.wide.optimalTilingFeatures, q.hasImage, q.imageResult,
           q.image.imageFormatProperties.maxExtent.width, q.image.imageFormatProperties.maxExtent.height,
           q.image.imageFormatProperties.maxArrayLayers, q.image.imageFormatProperties.sampleCounts, q.source);
  }
  destroy(instance, nullptr);
  // Driver/HAL and adrenotools namespaces are reclaimed at process exit.
  return 0;
}
