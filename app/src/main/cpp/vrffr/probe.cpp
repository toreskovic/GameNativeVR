// Separate Android process: no wrapper, guest handles, device creation or draws.
#include "capabilities.h"
#include <dlfcn.h>
#include <cstdio>
#include <string>
#include <vector>
int main(int argc, char **argv) {
  setbuf(stdout, nullptr);
  if (argc != 6) return 2;
  // Adrenotools expects the shared Android C++ runtime in the global scope.
  // The probe itself uses a static runtime and otherwise has no such dependency.
  void *runtime = dlopen(argv[5], RTLD_NOW | RTLD_GLOBAL);
  if (!runtime) { printf("direct probe: C++ runtime load failed: %s\n", dlerror()); return 12; }
  printf("direct probe: C++ runtime loaded: %s\n", argv[5]);
  const std::string toolsPath = std::string(argv[1]) + "/libadrenotools.so";
  void *tools = dlopen(toolsPath.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!tools) { printf("direct probe: adrenotools load failed: %s\n", dlerror()); return 3; }
  using Open = void *(*)(int, int, const char *, const char *, const char *, const char *, const char *, void **);
  auto open = reinterpret_cast<Open>(dlsym(tools, "adrenotools_open_libvulkan"));
  if (!open) { puts("direct probe: adrenotools export missing"); return 4; }
  // ADRENOTOOLS_DRIVER_CUSTOM = 1; same ABI as the application's existing loader.
  void *driver = open(RTLD_NOW | RTLD_LOCAL, 1, argv[4], argv[1], argv[2], argv[3], nullptr, nullptr);
  if (!driver) { puts("direct probe: custom driver load failed"); return 5; }
  auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(driver, "vkGetInstanceProcAddr"));
  if (!get) { puts("direct probe: vkGetInstanceProcAddr missing"); return 6; }
  auto create = reinterpret_cast<PFN_vkCreateInstance>(get(nullptr, "vkCreateInstance"));
  if (!create) return 7;
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "GameNative FFR direct driver probe";
  app.apiVersion = VK_API_VERSION_1_1;
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
  // Driver and adrenotools namespaces are reclaimed at process exit.
  return 0;
}
