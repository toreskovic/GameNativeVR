#include "../../app/src/main/cpp/xrimmersive/xr_visibility_mask.h"
#include <cassert>
#include <iostream>
using namespace xrimmersive;
static bool bad=false;
static XrResult XRAPI_CALL query(XrSession,XrViewConfigurationType,uint32_t eye,XrVisibilityMaskTypeKHR type,XrVisibilityMaskKHR *out) {
    assert(eye<2 && type>=1 && type<=3);
    out->vertexCountOutput=4;out->indexCountOutput=6;
    if(!out->vertexCapacityInput||!out->indexCapacityInput) return XR_SUCCESS;
    out->vertices[0]={-1,1};out->vertices[1]={1,1};out->vertices[2]={1,-1};out->vertices[3]={-1,-1};
    const uint32_t indices[]={0,1,2,0,2,3};std::copy(indices,indices+6,out->indices);
    if(bad) out->indices[0]=4;
    return XR_SUCCESS;
}
int main() {
    VisibilityMasks cache;cache.refresh(XR_NULL_HANDLE,nullptr);
    assert(cache.visible()[0].indices.empty());
    cache.refresh(XR_NULL_HANDLE,query);
    assert(cache.revision()==2 && cache.visible()[1].indices.size()==6);
    assert(cache.wire(0,1,0).find("vertices=4 indices=6 v0=-1000000 v1=1000000")!=std::string::npos);
    assert(cache.wire(1,3,8).find("v0=0 v1=1 v2=2")!=std::string::npos);
    assert(cache.wire(2,1,0)=="ERROR malformed");
    auto mesh=cache.visible()[0];std::vector<XrVector2f> uv;
    assert(projectVisibility(mesh,{-atanf(1),atanf(1),atanf(1),-atanf(1)},uv));
    assert(fabsf(uv[0].x)<1e-6 && fabsf(uv[0].y)<1e-6);
    assert(fabsf(uv[2].x-1)<1e-6 && fabsf(uv[2].y-1)<1e-6);
    uv.clear();assert(projectVisibility(mesh,{-atanf(2),atanf(1),atanf(2),-atanf(1)},uv));
    assert(fabsf(uv[0].x-1.f/3)<1e-6 && fabsf(uv[0].y-1.f/3)<1e-6);
    uv.clear();assert(projectVisibility(mesh,{-atanf(.7),atanf(.7),atanf(.7),-atanf(.7)},uv));
    assert(uv[0].x<0 && uv[2].x>1); // clipped by rasterizer, never clamped/distorted
    uv.clear();assert(!projectVisibility(mesh,{},uv)&&uv.empty());
    bad=true;cache.refresh(XR_NULL_HANDLE,query);assert(cache.visible()[0].vertices.empty());
    const XrFovf full{-atanf(1.2f),atanf(.9f),atanf(1.1f),-atanf(1.3f)};
    auto area=[](const VisibilityMesh &m) {
        double sum=0;
        for(size_t i=0;i<m.indices.size();i+=3) {
            auto a=m.vertices[m.indices[i]],b=m.vertices[m.indices[i+1]],c=m.vertices[m.indices[i+2]];
            double cross=(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
            assert(cross>0);sum+=cross*.5;
        }
        return sum;
    };
    const double fullArea=2.1*2.4;
    for(unsigned eye=0;eye<2;++eye) {
        auto masks=picoQuest3Mask(eye,full);
        double hidden=area(masks[0]),visible=area(masks[1]);
        assert(fabs((hidden+visible)/fullArea-1)<1e-5);
        assert(hidden/fullArea>.10 && hidden/fullArea<.115);
        std::cout<<"eye="<<eye<<" hidden="<<hidden/fullArea*100<<"%\n";
        // Every pixel center is covered once: no overlap or hole in the partition.
        for(int y=0;y<100;++y) for(int x=0;x<100;++x) {
            XrVector2f p{-1.2f+2.1f*(x+.371f)/100, -1.3f+2.4f*(y+.293f)/100};
            int hits=0;
            for(int type=0;type<2;++type) {
                auto &m=masks[type];
                for(size_t i=0;i<m.indices.size();i+=3) {
                    bool inside=true;
                    for(int edge=0;edge<3;++edge) {
                        auto a=m.vertices[m.indices[i+edge]],b=m.vertices[m.indices[i+(edge+1)%3]];
                        inside &= (b.x-a.x)*(p.y-a.y)-(b.y-a.y)*(p.x-a.x)>=0;
                    }
                    hits+=inside;
                }
            }
            assert(hits==1);
        }
    }
    VisibilityMasks fallback;fallback.refresh(XR_NULL_HANDLE,nullptr);
    assert(!fallback.updatePicoFallback(std::array<XrFovf,2>{}));
    assert(fallback.updatePicoFallback({full,full}));
    auto revision=fallback.revision();assert(!fallback.updatePicoFallback({full,full}));
    assert(fallback.revision()==revision);
    bad=false;fallback.refresh(XR_NULL_HANDLE,query);
    assert(!fallback.updatePicoFallback({full,full})); // native mesh takes priority
    assert(fallback.visible()[0].indices.size()==6);
    std::cout<<"Visibility cache/projection/protocol checks passed\n";
}
