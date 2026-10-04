// Isolated SYNC_FD probe: one empty signal submission and independent retirement fence.
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

#include <poll.h>
#include <unistd.h>
int main(int argc, char **argv) {
 setbuf(stdout,nullptr);
 if(argc!=2) return 2;
 void *lib=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL);
 if(!lib) { puts(dlerror()); return 3; }
 auto get=(PFN_vkGetInstanceProcAddr)dlsym(lib,"vkGetInstanceProcAddr");
 if(!get) get=(PFN_vkGetInstanceProcAddr)dlsym(lib,"vk_icdGetInstanceProcAddr");
 if(!get) {
  auto hal=(ProbeHalModule *)dlsym(lib,"HMI"); ProbeHalDevice *dev=nullptr;
  if(hal && hal->methods && !hal->methods->open(hal,"vk0",&dev)) get=dev->get;
 }
 if(!get) return 4;
 auto create=(PFN_vkCreateInstance)get(nullptr,"vkCreateInstance");
 VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_1;
 VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ic.pApplicationInfo=&app;
 VkInstance inst{}; VkResult result=create(&ic,nullptr,&inst); printf("instance=%d\n",result); if(result) return 5;
#define I(name) auto name=(PFN_vk##name)get(inst,"vk" #name)
 I(EnumeratePhysicalDevices); I(GetPhysicalDeviceExternalSemaphoreProperties); I(CreateDevice); I(GetDeviceProcAddr); I(DestroyInstance);
 uint32_t count=1; VkPhysicalDevice physical{}; if(EnumeratePhysicalDevices(inst,&count,&physical)!=VK_SUCCESS) return 6;
 VkPhysicalDeviceExternalSemaphoreInfo q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO}; q.handleType=VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT;
 VkExternalSemaphoreProperties props{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
 GetPhysicalDeviceExternalSemaphoreProperties(physical,&q,&props);
 printf("SYNC_FD semaphore features=0x%x compatible=0x%x\n",props.externalSemaphoreFeatures,props.compatibleHandleTypes);
 if(!(props.externalSemaphoreFeatures&VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT)) return 7;
 float priority=1;
 VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qc.queueCount=1; qc.pQueuePriorities=&priority;
 const char *ext[]={"VK_KHR_external_semaphore_fd"};
 VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dc.queueCreateInfoCount=1; dc.pQueueCreateInfos=&qc; dc.enabledExtensionCount=1; dc.ppEnabledExtensionNames=ext;
 VkDevice device{}; result=CreateDevice(physical,&dc,nullptr,&device); printf("device=%d\n",result); if(result) return 8;
#define D(name) auto name=(PFN_vk##name)GetDeviceProcAddr(device,"vk" #name)
 D(CreateSemaphore); D(GetSemaphoreFdKHR); D(DestroySemaphore); D(CreateFence); D(DestroyFence); D(WaitForFences); D(GetDeviceQueue); D(QueueSubmit); D(DestroyDevice);
 using Make=VkResult (*)(VkDevice,VkSemaphore *);
 using Export=VkResult (*)(VkDevice,VkSemaphore,int *);
 using Destroy=void (*)(VkDevice,VkSemaphore);
 auto make=(Make)dlsym(lib,"gamenative_xr_create_completion_semaphore_v1");
 auto exportFd=(Export)dlsym(lib,"gamenative_xr_export_completion_semaphore_v1");
 auto destroy=(Destroy)dlsym(lib,"gamenative_xr_destroy_completion_semaphore_v1");
 if(make && (!exportFd || !destroy)) return 9;
 VkSemaphore semaphore{};
 VkExportSemaphoreCreateInfo ex{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO}; ex.handleTypes=q.handleType;
 VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; sc.pNext=&ex;
 result=make?make(device,&semaphore):CreateSemaphore(device,&sc,nullptr,&semaphore);
 printf("create exportable semaphore=%d nativeABI=%d\n",result,make!=nullptr); if(result) return 10;
 VkFenceCreateInfo fc{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{};
 if(CreateFence(device,&fc,nullptr,&fence)!=VK_SUCCESS) return 11;
 VkQueue queue{}; GetDeviceQueue(device,0,0,&queue);
 VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.signalSemaphoreCount=1; submit.pSignalSemaphores=&semaphore;
 result=QueueSubmit(queue,1,&submit,fence); printf("submit=%d\n",result); if(result) return 12;
 VkSemaphoreGetFdInfoKHR gi{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR}; gi.semaphore=semaphore; gi.handleType=q.handleType;
 int fd=-1; result=exportFd?exportFd(device,semaphore,&fd):GetSemaphoreFdKHR(device,&gi,&fd);
 printf("export=%d fd=%d\n",result,fd);
 VkResult retired=WaitForFences(device,1,&fence,VK_TRUE,1000000000ull);
 printf("independent retirement=%d\n",retired);
 int signaled=1;
 if(fd>=0) { pollfd p{fd,POLLIN,0}; signaled=poll(&p,1,1000)>0 && (p.revents&POLLIN) && !(p.revents&(POLLERR|POLLNVAL)); close(fd); }
 printf("sync-file ready=%d\n",signaled);
 if(retired!=VK_SUCCESS) return 13;
 if(destroy) destroy(device,semaphore); else DestroySemaphore(device,semaphore,nullptr);
 DestroyFence(device,fence,nullptr); DestroyDevice(device,nullptr); DestroyInstance(inst,nullptr);
 return result==VK_SUCCESS && signaled ? 0:14;
}
