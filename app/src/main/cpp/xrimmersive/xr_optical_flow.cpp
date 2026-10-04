#include "xr_optical_flow.h"
#include "opticalflow/shaders_spv.h"
#include <algorithm>
#include <array>
#include <cmath>

#include "xr_vulkan_dispatch.h"

namespace xrimmersive::windowsvr {
struct OpticalFlowBackend::Impl {
    static constexpr int Levels=3, Sets=15;
    VkDevice device;
    VkExtent2D extent, flowExtent;
    VkDescriptorSetLayout setLayout{};
    VkPipelineLayout layout{};
    VkDescriptorPool pool{};
    VkDescriptorSet sets[Sets]{};
    VkSampler nearest{}, linear{};
    VkPipeline pipelines[7]{};
    bool filteredFlow=false, presentationSynthesis=false;
    lsfg::LsfgImage luma[2][Levels], vectors[2][Levels][2], filtered[2];
    int nextSet=0;
    bool ready=false, initialized=false;
    std::array<ColorTransform,2> colorTransforms{};
    Impl(const lsfg::Device& d,VkExtent2D e,float scale,bool deferred): device(d.Handle()),extent(e),presentationSynthesis(deferred) {
        flowExtent={std::max(32u,uint32_t(e.width*scale)),std::max(32u,uint32_t(e.height*scale))};
        auto supports=[&](VkFormat format,VkFormatFeatureFlags extra=0) {
            VkFormatProperties properties{};
            vkd.GetPhysicalDeviceFormatProperties(d.PhysicalHandle(),format,&properties);
            const auto required=VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|extra;
            return (properties.optimalTilingFeatures&required)==required;
        };
        const bool compactLuma=supports(VK_FORMAT_R8_UINT);
        filteredFlow=supports(VK_FORMAT_R16G16_SFLOAT,VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
        VkPhysicalDeviceSubgroupProperties subgroup{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
        VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        properties.pNext=&subgroup;
        vkd.GetPhysicalDeviceProperties2(d.PhysicalHandle(),&properties);
        const auto subgroupOps=VK_SUBGROUP_FEATURE_BASIC_BIT|VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
        const bool nativeSubgroup=(subgroup.supportedStages&VK_SHADER_STAGE_COMPUTE_BIT) &&
            (subgroup.supportedOperations&subgroupOps)==subgroupOps;
        if(filteredFlow) for(auto& image:filtered) {
            image=lsfg::LsfgImage(d,{(flowExtent.width+7)/8,(flowExtent.height+7)/8},VK_FORMAT_R16G16_SFLOAT);
            if(!image.Valid()) return;
        }
        for(int l=0;l<Levels;++l) {
            VkExtent2D le{std::max(1u,flowExtent.width>>l),std::max(1u,flowExtent.height>>l)};
            VkExtent2D ve{(le.width+7)/8,(le.height+7)/8};
            for(int i=0;i<2;++i) {
                luma[i][l]=lsfg::LsfgImage(d,le,compactLuma?VK_FORMAT_R8_UINT:VK_FORMAT_R32_UINT);
                if(!luma[i][l].Valid()) return;
                for(auto& v:vectors[i][l]) {
                    v=lsfg::LsfgImage(d,ve,VK_FORMAT_R32G32_SINT);
                    if(!v.Valid()) return;
                }
            }
        }
        VkDescriptorSetLayoutBinding bindings[7]{};
        for(int i=0;i<7;++i) bindings[i]={uint32_t(i),(i==2||i>=5)?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo si{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        si.bindingCount=7; si.pBindings=bindings;
        if(vkCreateDescriptorSetLayout(device,&si,nullptr,&setLayout)!=VK_SUCCESS) return;
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,112};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount=1;li.pSetLayouts=&setLayout;li.pushConstantRangeCount=1;li.pPushConstantRanges=&push;
        if(vkCreatePipelineLayout(device,&li,nullptr,&layout)!=VK_SUCCESS) return;
        VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,Sets*3},{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,Sets*4}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets=Sets;pi.poolSizeCount=2;pi.pPoolSizes=sizes;
        if(vkCreateDescriptorPool(device,&pi,nullptr,&pool)!=VK_SUCCESS) return;
        std::array<VkDescriptorSetLayout,Sets> layouts;layouts.fill(setLayout);
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool=pool;ai.descriptorSetCount=Sets;ai.pSetLayouts=layouts.data();
        if(vkAllocateDescriptorSets(device,&ai,sets)!=VK_SUCCESS) return;
        VkSamplerCreateInfo sm{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sm.addressModeU=sm.addressModeV=sm.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sm.minFilter=sm.magFilter=VK_FILTER_NEAREST;
        if(vkCreateSampler(device,&sm,nullptr,&nearest)!=VK_SUCCESS) return;
        sm.minFilter=sm.magFilter=VK_FILTER_LINEAR;
        if(vkCreateSampler(device,&sm,nullptr,&linear)!=VK_SUCCESS) return;
        using namespace opticalflow;
        const uint32_t* code[]={compactLuma?pyramidCompactSpv:pyramidSpv,nativeSubgroup?searchSubgroupSpv:searchSpv,
            filterSpv,refineSpv,filteredFlow?synthesizeFloatSpv:synthesizeSpv,filterFloatSpv,pack_flowSpv};
        const size_t bytes[]={compactLuma?sizeof(pyramidCompactSpv):sizeof(pyramidSpv),
            nativeSubgroup?sizeof(searchSubgroupSpv):sizeof(searchSpv),sizeof(filterSpv),sizeof(refineSpv),
            filteredFlow?sizeof(synthesizeFloatSpv):sizeof(synthesizeSpv),sizeof(filterFloatSpv),sizeof(pack_flowSpv)};
        for(int i=0;i<(presentationSynthesis?7:filteredFlow?6:5);++i) {
            VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};mi.codeSize=bytes[i];mi.pCode=code[i];
            VkShaderModule module{};
            if(vkCreateShaderModule(device,&mi,nullptr,&module)!=VK_SUCCESS) return;
            VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};ci.layout=layout;
            ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};
            auto result=vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&ci,nullptr,&pipelines[i]);
            vkDestroyShaderModule(device,module,nullptr);
            if(result!=VK_SUCCESS) return;
        }
        ready=true;
    }
    ~Impl() {
        for(auto p:pipelines) if(p) vkDestroyPipeline(device,p,nullptr);
        if(pool) vkDestroyDescriptorPool(device,pool,nullptr);
        if(layout) vkDestroyPipelineLayout(device,layout,nullptr);
        if(setLayout) vkDestroyDescriptorSetLayout(device,setLayout,nullptr);
        if(nearest) vkDestroySampler(device,nearest,nullptr);
        if(linear) vkDestroySampler(device,linear,nullptr);
    }
    void dependency(VkCommandBuffer cmd) {
        VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        m.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
        m.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&m,0,nullptr,0,nullptr);
    }
    void dispatch(VkCommandBuffer cmd,int pass,VkImageView a,VkImageView b,VkImageView out,VkImageView flow,VkImageView reverse,
                  int level,uint32_t x,uint32_t y,VkImageView mip1=VK_NULL_HANDLE,VkImageView mip2=VK_NULL_HANDLE) {
        // At most 15 dispatches (13 with presentation synthesis), each with its own immutable in-flight descriptor set.
        VkDescriptorSet set=sets[nextSet++];
        const VkSampler sa=(pass==0||pass==4)?linear:nearest;
        const VkSampler sb=pass==4?linear:nearest;
        const VkSampler sf=pass==4&&filteredFlow?linear:nearest;
        VkDescriptorImageInfo infos[]={{sa,a,VK_IMAGE_LAYOUT_GENERAL},{sb,b,VK_IMAGE_LAYOUT_GENERAL},
            {VK_NULL_HANDLE,out,VK_IMAGE_LAYOUT_GENERAL},{sf,flow,VK_IMAGE_LAYOUT_GENERAL},{sf,reverse,VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet writes[5]{};
        for(int i=0;i<5;++i) {
            writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};writes[i].dstSet=set;writes[i].dstBinding=i;
            writes[i].descriptorCount=1;writes[i].descriptorType=i==2?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[i].pImageInfo=&infos[i];
        }
        vkUpdateDescriptorSets(device,5,writes,0,nullptr);
        if(pass==0) {
            VkDescriptorImageInfo mipInfos[]={{VK_NULL_HANDLE,mip1,VK_IMAGE_LAYOUT_GENERAL},{VK_NULL_HANDLE,mip2,VK_IMAGE_LAYOUT_GENERAL}};
            VkWriteDescriptorSet mipWrites[2]{};
            for(int i=0;i<2;++i) { auto& w=mipWrites[i]; w={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w.dstSet=set; w.dstBinding=5+i; w.descriptorCount=1; w.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w.pImageInfo=&mipInfos[i]; }
            vkUpdateDescriptorSets(device,2,mipWrites,0,nullptr);
        }
        struct Params { int size[4]; std::array<ColorTransform,2> transforms; };
        static_assert(sizeof(Params)==112);
        Params params{{int(flowExtent.width),int(flowExtent.height),level,Levels},colorTransforms};
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines[pass]);
        vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
        vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(params),&params);
        vkCmdDispatch(cmd,x,y,1);dependency(cmd);
    }
    void record(VkCommandBuffer cmd,VkImageView oldColor,VkImageView newColor,VkImageView output,int current,bool reuse,const std::array<ColorTransform,2>& transforms) {
        colorTransforms=transforms;
        nextSet=0;
        if(!initialized) {
            lsfg::LsfgBarriers init(cmd);
            for(auto& pair:luma) for(auto& image:pair) init.ReadToWrite(image);
            for(auto& dir:vectors) for(auto& pair:dir) for(auto& image:pair) init.ReadToWrite(image);
            if(filteredFlow) for(auto& image:filtered) init.ReadToWrite(image);
            init.Build();initialized=true;
        }
        dependency(cmd);
        const auto dummy=vectors[0][0][0].View();
        for(int slot=0;slot<2;++slot) {
            if(reuse&&slot!=current) continue;
            auto size=luma[slot][0].Extent();
            dispatch(cmd,0,slot==current?newColor:oldColor,luma[slot][0].View(),luma[slot][0].View(),dummy,dummy,slot==current?1:0,
                     (size.width+15)/16,(size.height+15)/16,luma[slot][1].View(),luma[slot][2].View());
        }
        for(int dir=0;dir<2;++dir) {
            int first=dir?current:1-current,second=1-first;
            for(int l=Levels-1;l>=0;--l) {
                auto le=luma[first][l].Extent(),ve=vectors[dir][l][0].Extent();
                auto a=luma[first][l].View(),b=luma[second][l].View();
                if(l==Levels-1) {
                    dispatch(cmd,1,a,b,vectors[dir][l][0].View(),dummy,dummy,l,(le.width+15)/16,(le.height+15)/16);
                } else {
                    // Fuse coarse-vector upsampling/predictor selection with
                    // bounded, foveated refinement at this finer level.
                    dispatch(cmd,3,a,b,vectors[dir][l][0].View(),vectors[dir][l+1][1].View(),dummy,l,(ve.width+7)/8,(ve.height+7)/8);
                }
                if(l!=0 || !presentationSynthesis) dispatch(cmd,l==0&&filteredFlow?5:2,a,b,l==0&&filteredFlow?filtered[dir].View():vectors[dir][l][1].View(),vectors[dir][l][0].View(),dummy,l,(ve.width+15)/16,(ve.height+3)/4);

            }
        }
        if(presentationSynthesis) {
            auto size=vectors[0][0][0].Extent();
            dispatch(cmd,6,vectors[0][0][0].View(),vectors[1][0][0].View(),output,dummy,dummy,0,(size.width+7)/8,(size.height+7)/8);
            return;
        }
        dispatch(cmd,4,oldColor,newColor,output,filteredFlow?filtered[0].View():vectors[0][0][1].View(),filteredFlow?filtered[1].View():vectors[1][0][1].View(),0,(extent.width+7)/8,(extent.height+7)/8);
    }
};
OpticalFlowBackend::OpticalFlowBackend(const lsfg::Device& d,VkExtent2D e,float scale,bool deferred):p_(std::make_unique<Impl>(d,e,scale,deferred)){}
OpticalFlowBackend::~OpticalFlowBackend()=default;
bool OpticalFlowBackend::valid() const { return p_->ready; }
void OpticalFlowBackend::record(VkCommandBuffer cmd,VkImageView oldColor,VkImageView newColor,VkImageView output,int current,bool reuse,const std::array<ColorTransform,2>& transforms) {
    p_->record(cmd,oldColor,newColor,output,current,reuse,transforms);
}
}
