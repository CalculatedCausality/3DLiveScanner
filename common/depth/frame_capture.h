// SPDX-License-Identifier: Apache-2.0
#ifndef SCANNER_DEPTH_FRAME_CAPTURE_H
#define SCANNER_DEPTH_FRAME_CAPTURE_H
#include <depth/experimental.h>
#include <arcore/geometry_validation.h>

namespace oc { namespace depth_test {
// GL-side bounded copies only. No NNAPI, file I/O or borrowed plane pointers.
inline std::shared_ptr<Frame> Capture(const geometry::Plane& depth,
        const geometry::Plane& confidence, const geometry::Plane* secondary,
        int minimumConfidence, float filterError, uint64_t generation) {
    if (!depth.Valid(2) || !confidence.Valid(1) || !confidence.SameSize(depth) ||
        depth.width>1024 || depth.height>1024 || uint64_t(depth.width)*depth.height>262144) return {};
    try {
        std::shared_ptr<Frame> frame=std::make_shared<Frame>();
        frame->width=depth.width;frame->height=depth.height;frame->generation=generation;
        size_t count=size_t(depth.width)*depth.height;
        frame->depth.resize(count);frame->confidence.resize(count);frame->links.reserve(count);
        bool filtered=secondary&&secondary->Valid(2)&&secondary->SameSize(depth);
        for (int y=0;y<depth.height;++y) for(int x=0;x<depth.width;++x) {
            size_t i=size_t(y)*depth.width+x;
            float value=depth.Depth(x,y)*.001f;
            const int c=confidence.Byte(x,y);
            if(double(value)>.05&&c>minimumConfidence&&filtered) {
                float alternative=secondary->Depth(x,y)*.001f;
                if(double(alternative)>.05&&std::fabs(double(value)-alternative)<filterError) value=alternative;
            }
            frame->depth[i]=value;
            frame->confidence[i]=value>0?c/255.f:0;
        }
        return frame;
    } catch(...) { return {}; }
}

inline void AddRayLink(std::shared_ptr<Frame>& frame,uint32_t pixel,uint32_t index,
        const glm::vec4& original,float depth,float minimumDepth,const glm::dvec3& ray) {
    if(!frame)return;
    try {
        for(int k=0;k<3;++k)if(!std::isfinite(ray[k]))return;
        Link link;link.pixel=pixel;link.point=index;link.original=original;
        link.depth=depth;link.minimumDepth=minimumDepth;link.worldPerMetre=ray;
        frame->links.push_back(link);
    } catch(...) {frame.reset();}
}

inline void AddLink(std::shared_ptr<Frame>& frame,uint32_t pixel,uint32_t index,
        const glm::vec4& original,float depth,float minimumDepth,
        const glm::dmat4& screenToWorld,const glm::dvec2& uv,double length) {
    if(!frame) return;
    try {
        glm::dvec4 a=screenToWorld*glm::dvec4(uv,0,1),b=screenToWorld*glm::dvec4(uv,1,1);
        if(!std::isfinite(a.w)||!std::isfinite(b.w)||std::fabs(a.w)<1e-12||std::fabs(b.w)<1e-12||length<=0) return;
        a/=std::fabs(a.w);b/=std::fabs(b.w);
        Link link;link.pixel=pixel;link.point=index;link.original=original;
        link.depth=depth;link.minimumDepth=minimumDepth;link.worldPerMetre=glm::dvec3(b-a)/length;
        for(int k=0;k<3;++k) if(!std::isfinite(link.worldPerMetre[k])) return;
        frame->links.push_back(link);
    } catch(...) { frame.reset(); }
}
} }
#endif
