// SPDX-License-Identifier: Apache-2.0
// Generated native planes only. No camera, app, files, or scan dataset access.
#include "../../common/depth/experimental.h"
#include <cmath>
#include <cstdio>

int main() {
    using namespace oc::depth_test;
    Runtime runtime; runtime.SetEnabled(true);
    Frame frame; frame.width=160; frame.height=120; frame.generation=runtime.Generation();
    frame.worldToCamera[1][1]=-1; frame.worldToCamera[2][2]=-1;
    frame.depth.resize(frame.width*frame.height); frame.confidence.assign(frame.depth.size(),.85f);
    std::vector<glm::vec4> original;
    for(int y=0;y<frame.height;++y) for(int x=0;x<frame.width;++x) {
        size_t p=y*frame.width+x;
        frame.depth[p]=2.f+.008f*std::sin(float(x*17+y*13));
        Link link; link.pixel=p; link.point=p; link.depth=frame.depth[p];
        link.worldPerMetre=glm::dvec3((x-80)/144.,(y-60)/144.,-1.);
        link.original=glm::vec4(glm::vec3(link.worldPerMetre*double(link.depth)),.85f);
        frame.links.push_back(link); original.push_back(link.original);
    }
    frame.pointCount=original.size();
    for(int repeat=0;repeat<5;++repeat) {
        auto points=original; Stats stats;
        bool applied=runtime.Apply(frame,points,stats);
        std::printf("EXPERIMENTAL generated-only repeat=%d applied=%d inferred=%d eligible=%u changed=%u ms=%.3f %s\n",
            repeat,int(applied),int(stats.inferred),stats.eligible,stats.changed,stats.milliseconds,runtime.Status().c_str());
        if(!applied || !stats.inferred || !stats.changed) return 2;
        for(size_t i=0;i<points.size();++i) {
            if(points[i].w!=original[i].w || !std::isfinite(points[i].z) ||
               std::fabs(points[i].z-original[i].z)>.020001) return 3;
        }
    }
    return 0;
}
