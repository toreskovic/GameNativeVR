// GameNative VR resource adapters for the vendored FidelityFX optical-flow kernels.
#define FFX_GPU 1
#define FfxInt32 int
#define FfxInt32x2 ivec2
#define FfxInt32x3 ivec3
#define FfxUInt32 uint
#define FfxUInt32x2 uvec2
#define FfxUInt32x4 uvec4
#define FfxBoolean bool
#define FFX_GROUPSHARED shared
#define FFX_GROUP_MEMORY_BARRIER barrier()
#define ffxMin min
#define ffxMax max
#define ffxBroadcast4 uvec4
#define FFX_OPTICALFLOW_USE_MSAD4_INSTRUCTION 0
#define FFX_OPTICALFLOW_FIX_TOP_LEFT_BIAS 1
#define FFX_OPTICALFLOW_USE_HEURISTICS 1
#define FFX_LOCAL_SEARCH_FALLBACK 1
#define FFX_OPTICALFLOW_THREAD_GROUP_WIDTH 4
#define FFX_OPTICALFLOW_THREAD_GROUP_HEIGHT 4
#define FFX_OPTICALFLOW_THREAD_GROUP_DEPTH 4
layout(push_constant) uniform Params { ivec4 size; } p;
layout(binding=0) uniform usampler2D firstLuma;
layout(binding=1) uniform usampler2D secondLuma;
#ifdef FILTERED_FLOW
layout(binding=2, rg16f) uniform image2D outFlow;
#else
layout(binding=2, rg32i) uniform iimage2D outFlow;
#endif
layout(binding=3) uniform isampler2D previousFlow;
uint OpticalFlowPyramidLevel() { return uint(p.size.z); }
uint OpticalFlowPyramidLevelCount() { return uint(p.size.w); }
bool IsSceneChanged() { return false; } // rejection is local in the custom synthesis pass
uint packed(usampler2D tex, ivec2 pos) {
    ivec2 hi=textureSize(tex,0)-1;
    uint v=0u;
    for(int x=0;x<4;++x) v |= texelFetch(tex,clamp(pos+ivec2(x,0),ivec2(0),hi),0).r << uint(x*8);
    return v;
}
uint LoadFirstImagePackedLuma(ivec2 pos) { return packed(firstLuma,pos); }
uint LoadSecondImagePackedLuma(ivec2 pos) { return packed(secondLuma,pos); }
ivec2 LoadPreviousOpticalFlow(ivec2 pos) {
    return texelFetch(previousFlow,clamp(pos,ivec2(0),textureSize(previousFlow,0)-1),0).xy;
}
ivec2 LoadOpticalFlow(ivec2 pos) { return LoadPreviousOpticalFlow(pos); }
ivec2 LoadRwOpticalFlow(ivec2 pos) {
    if(p.size.z==p.size.w-1) return ivec2(0);
    return ivec2(imageLoad(outFlow,clamp(pos,ivec2(0),imageSize(outFlow)-1)).xy);
}
void StoreOpticalFlow(ivec2 pos, ivec2 value) {
#ifdef SEARCH_PASS
    // Upstream all lanes store the same vector. Elect one writer to avoid a race.
    if(gl_LocalInvocationIndex!=0u) return;
#endif
    if(all(greaterThanEqual(pos,ivec2(0))) && all(lessThan(pos,imageSize(outFlow))))
#ifdef FILTERED_FLOW
        imageStore(outFlow,pos,vec4(value,0,0));
#else
        imageStore(outFlow,pos,ivec4(value,0,0));
#endif
}
void StoreOpticalFlowNextLevel(ivec2 pos, ivec2 value) { StoreOpticalFlow(pos,value); }
#ifdef SEARCH_PASS
#ifdef NATIVE_SUBGROUP
shared uint subgroupPartial[64];
uint reduce64(uint v, bool sum) {
    uint partial = sum ? subgroupAdd(v) : subgroupMin(v);
    if(gl_NumSubgroups == 1u) return partial;
    if(subgroupElect()) subgroupPartial[gl_SubgroupID] = partial;
    barrier();
    uint result = sum ? 0u : 0xffffffffu;
    for(uint i=0u;i<gl_NumSubgroups;++i)
        result = sum ? result+subgroupPartial[i] : min(result,subgroupPartial[i]);
    barrier(); // All readers must finish before the next reduction overwrites it.
    return result;
}
#else
// A fixed 64-thread workgroup reduction avoids assuming a particular subgroup
// width. Native subgroup arithmetic can replace this after device measurements.
shared uint reduction[64];
uint reduce64(uint v, bool sum) {
    uint i=gl_LocalInvocationIndex;
    reduction[i]=v; barrier();
    for(uint step=32u;step>0u;step>>=1u) {
        if(i<step) reduction[i]=sum ? reduction[i]+reduction[i+step] : min(reduction[i],reduction[i+step]);
        barrier();
    }
    uint result=reduction[0]; barrier(); return result;
}
#endif
#define ffxWaveSum(v) reduce64(v,true)
#define ffxWaveMin(v) reduce64(v,false)
#define ffxWaveLaneCount() 64
#define ffxWaveIsFirstLane() (gl_LocalInvocationIndex==0u)
#endif
#include "upstream/ffx_opticalflow_common.h"
