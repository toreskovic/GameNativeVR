// Isolated driver probe: creates small images/views, but submits no GPU work.
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <cstdio>
#include <cstring>
#include <initializer_list>
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
 if (argc != 2) return 2;
 for (unsigned layers : {1u, 2u}) {
  AHardwareBuffer_Desc desc{}; desc.width=64; desc.height=64; desc.layers=layers;
  desc.format=AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
  desc.usage=AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE|AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT;
  AHardwareBuffer *buffer=nullptr; int result=AHardwareBuffer_allocate(&desc,&buffer);
  printf("native AHB layers=%u allocate=%d\n",layers,result);
  if(buffer) AHardwareBuffer_release(buffer);
 }
 auto lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
 if (!lib) { puts(dlerror()); return 3; }
 auto get = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
 if (!get) get = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
 if (!get) {
  auto hal = (ProbeHalModule *)dlsym(lib, "HMI"); ProbeHalDevice *dev = nullptr;
  if (hal && hal->methods && !hal->methods->open(hal, "vk0", &dev)) get = dev->get;
 }
 if (!get) return 4;
 auto create = (PFN_vkCreateInstance)get(nullptr,"vkCreateInstance");
 VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_1;
 VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ic.pApplicationInfo=&app;
 VkInstance inst{}; VkResult r=create(&ic,nullptr,&inst); printf("instance=%d\n",r); if(r) return 5;
#define I(name) auto name=(PFN_vk##name)get(inst,"vk" #name)
 I(EnumeratePhysicalDevices); I(GetPhysicalDeviceImageFormatProperties2); I(CreateDevice); I(GetDeviceProcAddr); I(DestroyInstance);
 uint32_t n=1; VkPhysicalDevice phys{}; if(EnumeratePhysicalDevices(inst,&n,&phys)!=VK_SUCCESS) return 6;
 float priority=1; VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qc.queueFamilyIndex=0; qc.queueCount=1; qc.pQueuePriorities=&priority;
 const char *ext[]={"VK_ANDROID_external_memory_android_hardware_buffer", "VK_EXT_queue_family_foreign", "VK_KHR_external_memory", "VK_KHR_dedicated_allocation", "VK_KHR_get_memory_requirements2", "VK_KHR_sampler_ycbcr_conversion", "VK_KHR_bind_memory2", "VK_KHR_maintenance1"};
 VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dc.queueCreateInfoCount=1; dc.pQueueCreateInfos=&qc; dc.enabledExtensionCount=sizeof(ext)/sizeof(ext[0]); dc.ppEnabledExtensionNames=ext;
 VkDevice dev{}; r=CreateDevice(phys,&dc,nullptr,&dev); printf("device=%d\n",r); if(r) return 7;
#define D(name) auto name=(PFN_vk##name)GetDeviceProcAddr(dev,"vk" #name)
 D(CreateImage); D(DestroyImage); D(AllocateMemory); D(FreeMemory); D(BindImageMemory); D(GetMemoryAndroidHardwareBufferANDROID); D(CreateImageView); D(DestroyImageView); D(DestroyDevice);
 for(auto fmt:{VK_FORMAT_B8G8R8A8_SRGB,VK_FORMAT_B8G8R8A8_UNORM,VK_FORMAT_R8G8B8A8_SRGB,VK_FORMAT_R8G8B8A8_UNORM}) for(unsigned mutableFormat:{0u,1u}) {
  VkPhysicalDeviceExternalImageFormatInfo eq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO}; eq.handleType=VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID;
  VkPhysicalDeviceImageFormatInfo2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2}; q.pNext=&eq; q.format=fmt; q.type=VK_IMAGE_TYPE_2D; q.tiling=VK_IMAGE_TILING_OPTIMAL; q.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT; q.flags=mutableFormat?VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT:0;
  VkExternalImageFormatProperties ep{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES}; VkImageFormatProperties2 ip{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2}; ip.pNext=&ep;
  r=GetPhysicalDeviceImageFormatProperties2(phys,&q,&ip); printf("format=%d mutable=%u query=%d external=%x maxLayers=%u\n",fmt,mutableFormat,r,ep.externalMemoryProperties.externalMemoryFeatures,ip.imageFormatProperties.maxArrayLayers);
  if(r || !(ep.externalMemoryProperties.externalMemoryFeatures&VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) continue;
  for(unsigned layers:{1u,2u}) {
   VkExternalMemoryImageCreateInfo ei{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO}; ei.handleTypes=eq.handleType;
   VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ci.pNext=&ei; ci.flags=q.flags; ci.imageType=q.type; ci.format=fmt; ci.extent={64,64,1}; ci.mipLevels=1; ci.arrayLayers=layers; ci.samples=VK_SAMPLE_COUNT_1_BIT; ci.tiling=q.tiling; ci.usage=q.usage;
   VkImage image{}; r=CreateImage(dev,&ci,nullptr,&image); printf(" layers=%u create=%d",layers,r); if(r){ puts(""); continue; }
   VkExportMemoryAllocateInfo ex{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO}; ex.handleTypes=eq.handleType;
   VkMemoryDedicatedAllocateInfo dd{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO}; dd.pNext=&ex; dd.image=image;
   VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.pNext=&dd;
   VkDeviceMemory mem{}; r=AllocateMemory(dev,&ai,nullptr,&mem); printf(" alloc=%d",r);
   if(r==VK_SUCCESS) {
    r=BindImageMemory(dev,image,mem,0); printf(" bind=%d",r);
    if(r==VK_SUCCESS) {
     VkMemoryGetAndroidHardwareBufferInfoANDROID gi{VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID}; gi.memory=mem; AHardwareBuffer *ahb=nullptr;
     r=GetMemoryAndroidHardwareBufferANDROID(dev,&gi,&ahb); printf(" export=%d",r);
     if(r==VK_SUCCESS && ahb) { AHardwareBuffer_Desc d{}; AHardwareBuffer_describe(ahb,&d); printf(" ahbFormat=%u ahbLayers=%u",d.format,d.layers); AHardwareBuffer_release(ahb); }
     VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image=image; vi.viewType=VK_IMAGE_VIEW_TYPE_2D_ARRAY; vi.format=mutableFormat&&fmt==VK_FORMAT_R8G8B8A8_UNORM?VK_FORMAT_R8G8B8A8_SRGB:fmt; vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,layers}; VkImageView view{};
     r=CreateImageView(dev,&vi,nullptr,&view); printf(" viewFormat=%d view=%d",vi.format,r); if(!r) DestroyImageView(dev,view,nullptr);
    }
   }
   puts(""); DestroyImage(dev,image,nullptr); if(mem) FreeMemory(dev,mem,nullptr);
  }
 }
 DestroyDevice(dev,nullptr); DestroyInstance(inst,nullptr); return 0;
}
