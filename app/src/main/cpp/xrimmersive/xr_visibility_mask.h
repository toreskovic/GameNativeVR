#pragma once
#include <openxr/openxr.h>
#include <array>
#include <vector>
#include <mutex>
#include <cmath>
#include <string>
#include <algorithm>
#include "xr_quest3_mask_data.h"

namespace xrimmersive {
struct VisibilityMesh {
    std::vector<XrVector2f> vertices;
    std::vector<uint32_t> indices;
};
// Horizontal strips partition the rectangle into visible and hidden triangles.
// This preserves a non-circular outline without overlapping triangles or holes.
inline std::array<VisibilityMesh,3> picoQuest3Mask(unsigned eye,const XrFovf &fov) {
    std::array<VisibilityMesh,3> result;
    auto outline=quest3Outline(eye);
    std::vector<float> rows{0,1};
    for(auto v:outline) rows.push_back(v.y);
    std::sort(rows.begin(),rows.end());rows.erase(std::unique(rows.begin(),rows.end()),rows.end());
    auto triangle=[](VisibilityMesh &mesh,XrVector2f a,XrVector2f b,XrVector2f c) {
        float cross=(b.x-a.x)*(c.y-a.y)-(b.y-a.y)*(c.x-a.x);
        if(fabsf(cross)<1e-10f) return;
        if(cross<0) std::swap(b,c);
        uint32_t base=mesh.vertices.size();
        mesh.vertices.insert(mesh.vertices.end(),{a,b,c});
        mesh.indices.insert(mesh.indices.end(),{base,base+1,base+2});
    };
    auto quad=[&](VisibilityMesh &mesh,float l0,float r0,float l1,float r1,float y0,float y1) {
        triangle(mesh,{l0,y0},{r0,y0},{r1,y1});
        triangle(mesh,{l0,y0},{r1,y1},{l1,y1});
    };
    for(size_t row=1;row<rows.size();++row) {
        float y0=rows[row-1],y1=rows[row],mid=(y0+y1)*.5f;
        std::vector<std::array<float,2>> spans;
        for(size_t i=0;i<outline.size();++i) {
            auto a=outline[i],b=outline[(i+1)%outline.size()];
            if(mid<=std::min(a.y,b.y)||mid>=std::max(a.y,b.y)) continue;
            auto x=[&](float y){return a.x+(b.x-a.x)*(y-a.y)/(b.y-a.y);};
            spans.push_back({x(y0),x(y1)});
        }
        if(spans.size()!=2) return {}; // malformed outline: leave full-screen fallback
        if(spans[0][0]+spans[0][1]>spans[1][0]+spans[1][1]) std::swap(spans[0],spans[1]);
        auto l=spans[0],r=spans[1];
        quad(result[1],l[0],r[0],l[1],r[1],y0,y1);
        quad(result[0],0,l[0],0,l[1],y0,y1);
        quad(result[0],r[0],1,r[1],1,y0,y1);
    }
    result[2].vertices=outline;
    for(uint32_t i=0;i<outline.size();++i) result[2].indices.push_back(i);
    const float l=tanf(fov.angleLeft),r=tanf(fov.angleRight),d=tanf(fov.angleDown),u=tanf(fov.angleUp);
    if(!(r>l&&u>d)||!std::isfinite(r-l)||!std::isfinite(u-d)) return {};
    for(auto &mesh:result) for(auto &v:mesh.vertices) v={l+v.x*(r-l),d+v.y*(u-d)};
    return result;
}
// The XR thread queries the runtime. Control-server threads only read this cache.
class VisibilityMasks {
    mutable std::mutex mutex_;
    std::array<std::array<VisibilityMesh,3>,2> masks_;
    uint32_t revision_ = 0;
    std::array<bool,2> fallback_{};
    std::array<XrFovf,2> fallbackFov_{};
public:
    void refresh(XrSession session, PFN_xrGetVisibilityMaskKHR get) {
        std::array<std::array<VisibilityMesh,3>,2> next;
        if (get) for (unsigned eye=0;eye<2;++eye) for (unsigned type=1;type<=3;++type) {
            XrVisibilityMaskKHR mask{XR_TYPE_VISIBILITY_MASK_KHR};
            if (XR_FAILED(get(session,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,eye,
                XrVisibilityMaskTypeKHR(type),&mask)) || !mask.vertexCountOutput || !mask.indexCountOutput ||
                mask.vertexCountOutput>65536 || mask.indexCountOutput>196608) continue;
            auto &mesh=next[eye][type-1];
            mesh.vertices.resize(mask.vertexCountOutput); mesh.indices.resize(mask.indexCountOutput);
            mask.vertexCapacityInput=mesh.vertices.size(); mask.vertices=mesh.vertices.data();
            mask.indexCapacityInput=mesh.indices.size(); mask.indices=mesh.indices.data();
            if (XR_FAILED(get(session,XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,eye,
                XrVisibilityMaskTypeKHR(type),&mask)) || mask.vertexCountOutput>mesh.vertices.size() ||
                mask.indexCountOutput>mesh.indices.size()) {mesh={};continue;}
            mesh.vertices.resize(mask.vertexCountOutput); mesh.indices.resize(mask.indexCountOutput);
            bool valid=type==3 || mesh.indices.size()%3==0;
            for(auto v:mesh.vertices) valid &= std::isfinite(v.x)&&std::isfinite(v.y)&&fabsf(v.x)<100&&fabsf(v.y)<100;
            for(auto i:mesh.indices) valid &= i<mesh.vertices.size();
            if(!valid) mesh={};
        }
        std::lock_guard<std::mutex> lock(mutex_);
        masks_=std::move(next); fallback_={}; ++revision_;
    }
    // Called only for Pico 4, with the runtime's full, unscaled per-eye FOV.
    bool updatePicoFallback(const std::array<XrFovf,2> &fovs) {
        std::lock_guard<std::mutex> lock(mutex_);
        bool changed=false;
        for(unsigned eye=0;eye<2;++eye) {
            bool native=false;
            for(auto &mesh:masks_[eye]) native |= !mesh.indices.empty();
            if(native && !fallback_[eye]) continue;
            const auto &a=fovs[eye], &b=fallbackFov_[eye];
            if(fallback_[eye] && fabsf(a.angleLeft-b.angleLeft)<1e-6f && fabsf(a.angleRight-b.angleRight)<1e-6f &&
                fabsf(a.angleUp-b.angleUp)<1e-6f && fabsf(a.angleDown-b.angleDown)<1e-6f) continue;
            auto mask=picoQuest3Mask(eye,a);
            if(mask[1].indices.empty()) continue;
            masks_[eye]=std::move(mask);fallback_[eye]=true;fallbackFov_[eye]=a;changed=true;
        }
        if(changed) ++revision_;
        return changed;
    }
    uint32_t revision() const {std::lock_guard<std::mutex> lock(mutex_);return revision_;}
    std::array<VisibilityMesh,2> visible() const {
        std::lock_guard<std::mutex> lock(mutex_);return {masks_[0][1],masks_[1][1]};
    }
    // Bounded chunks, revision checked by the caller before accepting any data.
    std::string wire(int eye,int type,int offset) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if(eye<0||eye>1||type<1||type>3||offset<0) return "ERROR malformed";
        const auto &m=masks_[eye][type-1];
        std::string out="OK revision="+std::to_string(revision_)+" vertices="+std::to_string(m.vertices.size())+
            " indices="+std::to_string(m.indices.size());
        const size_t total=m.vertices.size()*2+m.indices.size();
        for(size_t i=offset;i<total&&i<size_t(offset)+64;++i) {
            long long value=i<m.vertices.size()*2 ? llround(double(i%2?m.vertices[i/2].y:m.vertices[i/2].x)*1000000.) :
                m.indices[i-m.vertices.size()*2];
            out+=" v"+std::to_string(i-offset)+"="+std::to_string(value);
        }
        return out;
    }
};
// Masks live on z=-1, in metres. Project using the actual submitted layer FOV,
// including cropped FOV / stretched-border output, and Vulkan's downward Y.
inline bool projectVisibility(const VisibilityMesh &mesh,const XrFovf &fov,std::vector<XrVector2f> &uv) {
    float l=tanf(fov.angleLeft),r=tanf(fov.angleRight),u=tanf(fov.angleUp),d=tanf(fov.angleDown);
    if(!(r>l&&u>d)||!std::isfinite(r-l)||!std::isfinite(u-d)||mesh.indices.empty()) return false;
    for(auto i:mesh.indices) {auto v=mesh.vertices[i];uv.push_back({(v.x-l)/(r-l),(u-v.y)/(u-d)});}
    return true;
}
}
