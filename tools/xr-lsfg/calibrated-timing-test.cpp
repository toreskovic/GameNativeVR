// Exercise calibrated device/monotonic clocks and three GPU markers on host Vulkan.
#include "../../app/src/main/cpp/xrimmersive/xr_lsfg_timing.h"
#include <vulkan/vulkan.h>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iostream>
#include <vector>
using namespace xrimmersive::windowsvr;
#define CHECK(call) assert((call)==VK_SUCCESS)
static int64_t now() { return std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count(); }
int main() {
    // Exercise the OS bridge even when the host Vulkan driver advertises only
    // MONOTONIC. The real RAW Vulkan path still needs an advertising device.
    auto osBridge=sampleRawClockBridge();
    const auto before=now();
    timespec rawTime{}; assert(clock_gettime(CLOCK_MONOTONIC_RAW,&rawTime)==0);
    const auto after=now(); osBridge.include(sampleRawClockBridge());
    const auto converted=osBridge.convert(uint64_t(rawTime.tv_sec)*1000000000+rawTime.tv_nsec);
    assert(converted>0 && converted>=before-int64_t(osBridge.uncertainty())
           && converted<=after+int64_t(osBridge.uncertainty()));
    std::cout<<"OS RAW clock bridge passed\n";
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pApplicationInfo=&app;
    VkInstance instance{}; CHECK(vkCreateInstance(&ici,nullptr,&instance));
    uint32_t n=0; CHECK(vkEnumeratePhysicalDevices(instance,&n,nullptr)); assert(n);
    std::vector<VkPhysicalDevice> physical(n); CHECK(vkEnumeratePhysicalDevices(instance,&n,physical.data()));
    auto gpu=physical[0]; CHECK(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&n,nullptr));
    std::vector<VkExtensionProperties> extensions(n); CHECK(vkEnumerateDeviceExtensionProperties(gpu,nullptr,&n,extensions.data()));
    const char* ext=nullptr;
    for(const auto& e:extensions) if(!strcmp(e.extensionName,VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME)) ext=VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME;
    for(const auto& e:extensions) if(!strcmp(e.extensionName,"VK_KHR_calibrated_timestamps")) ext="VK_KHR_calibrated_timestamps";
    const bool khr=ext && !strcmp(ext,"VK_KHR_calibrated_timestamps");
    vkGetPhysicalDeviceQueueFamilyProperties(gpu,&n,nullptr);
    std::vector<VkQueueFamilyProperties> families(n); vkGetPhysicalDeviceQueueFamilyProperties(gpu,&n,families.data());
    uint32_t family=0; while(family<n && (!(families[family].queueFlags&VK_QUEUE_COMPUTE_BIT)||!families[family].timestampValidBits)) ++family; assert(family<n);
    VkPhysicalDeviceProperties props{}; vkGetPhysicalDeviceProperties(gpu,&props);
    const float priority=1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueFamilyIndex=family; qi.queueCount=1; qi.pQueuePriorities=&priority;
    VkDeviceCreateInfo di{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.queueCreateInfoCount=1; di.pQueueCreateInfos=&qi; di.enabledExtensionCount=ext?1:0; di.ppEnabledExtensionNames=&ext;
    VkDevice device{}; CHECK(vkCreateDevice(gpu,&di,nullptr,&device)); VkQueue queue{}; vkGetDeviceQueue(device,family,0,&queue);
    auto domains=reinterpret_cast<PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT>(vkGetInstanceProcAddr(instance,khr?"vkGetPhysicalDeviceCalibrateableTimeDomainsKHR":"vkGetPhysicalDeviceCalibrateableTimeDomainsEXT"));
    auto calibrate=reinterpret_cast<PFN_vkGetCalibratedTimestampsEXT>(vkGetDeviceProcAddr(device,khr?"vkGetCalibratedTimestampsKHR":"vkGetCalibratedTimestampsEXT"));
    bool monotonic=false, raw=false;
    if(ext&&domains&&calibrate) { CHECK(domains(gpu,&n,nullptr)); std::vector<VkTimeDomainEXT> ts(n); CHECK(domains(gpu,&n,ts.data())); for(auto t:ts) { monotonic|=t==VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT; raw|=t==VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT; } }
    VkCommandPoolCreateInfo ci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; ci.queueFamilyIndex=family;
    VkCommandPool pool{}; CHECK(vkCreateCommandPool(device,&ci,nullptr,&pool));
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool=pool; ai.commandBufferCount=1; ai.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkCommandBuffer cmd{}; CHECK(vkAllocateCommandBuffers(device,&ai,&cmd));
    VkQueryPoolCreateInfo pi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO}; pi.queryType=VK_QUERY_TYPE_TIMESTAMP; pi.queryCount=3;
    VkQueryPool queries{}; CHECK(vkCreateQueryPool(device,&pi,nullptr,&queries));
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence fence{}; CHECK(vkCreateFence(device,&fi,nullptr,&fence));
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(vkBeginCommandBuffer(cmd,&bi));
    vkCmdResetQueryPool(cmd,queries,0,3);
    vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,0);
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&barrier,0,nullptr,0,nullptr);
    vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,queries,1);
    vkCmdWriteTimestamp(cmd,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,queries,2); CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO}; submit.commandBufferCount=1; submit.pCommandBuffers=&cmd;
    const auto bridgeAtSubmit=sampleRawClockBridge();
    const auto submitted=now(); CHECK(vkQueueSubmit(queue,1,&submit,fence));
    CHECK(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX)); // Test only; production polls completion.
    const auto completed=now(); uint64_t ticks[3]{};
    CHECK(vkGetQueryPoolResults(device,queries,0,3,sizeof(ticks),ticks,sizeof(uint64_t),VK_QUERY_RESULT_64_BIT));
    const auto bits=families[family].timestampValidBits; const auto period=props.limits.timestampPeriod;
    for(int i=0;i<2;++i) { const auto elapsed=gpuTimestampDuration(ticks[i],ticks[i+1],bits,period); assert(elapsed>=0 && elapsed<completed-submitted); }
    for (bool useRaw : {false, true}) {
        if (!(useRaw ? raw : monotonic)) continue;
        auto bridge=bridgeAtSubmit;
        bridge.include(sampleRawClockBridge());
        VkCalibratedTimestampInfoEXT ts[2]={{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT},{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT}};
        ts[0].timeDomain=VK_TIME_DOMAIN_DEVICE_EXT; ts[1].timeDomain=useRaw ? VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_EXT : VK_TIME_DOMAIN_CLOCK_MONOTONIC_EXT;
        uint64_t clocks[2],deviation; CHECK(calibrate(device,2,ts,clocks,&deviation)); assert(deviation<1000000);
        bridge.include(sampleRawClockBridge());
        const auto host=useRaw ? bridge.convert(clocks[1]) : int64_t(clocks[1]);
        if(useRaw) { assert(host>0); deviation+=bridge.uncertainty(); }
        for(auto tick:ticks) { const auto mapped=calibratedGpuTime(tick,clocks[0],host,bits,period); assert(mapped>=submitted-int64_t(deviation)&&mapped<=completed+int64_t(deviation)); }
        std::cout<<(useRaw ? "RAW bridged" : "MONOTONIC")<<" GPU markers passed; max deviation ns="<<deviation<<'\n';
    }
    if(!raw&&!monotonic) std::cout<<"Host calibration unavailable\n";
    vkDestroyFence(device,fence,nullptr); vkDestroyQueryPool(device,queries,nullptr); vkDestroyCommandPool(device,pool,nullptr); vkDestroyDevice(device,nullptr); vkDestroyInstance(instance,nullptr);
}
