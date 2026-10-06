// SPDX-License-Identifier: Apache-2.0
#ifndef SCANNER_DEPTH_MEASUREMENT_H
#define SCANNER_DEPTH_MEASUREMENT_H
#include <arcore/geometry_validation.h>

namespace oc { namespace geometry {

// Raw ARCore depth is axial camera Z, not distance along a normalized ray.
// Form the metric world ray from calibration and the matching camera view;
// never derive scale from a numerically reconstructed far clipping plane.
struct DepthProjection {
    glm::dvec3 origin{0}, horizontal{0}, vertical{0}, center{0};
    bool valid=false;
    bool Configure(const glm::mat4& projection,const glm::mat4& view) {
        valid=false;
        if(!RigidPose(view)||!Invertible(projection)||
           std::fabs(projection[0][0])<1e-8||std::fabs(projection[1][1])<1e-8||
           std::fabs(projection[0][1])>1e-6||std::fabs(projection[1][0])>1e-6||
           std::fabs(projection[0][3])>1e-6||std::fabs(projection[1][3])>1e-6||
           std::fabs(projection[2][3]+1)>1e-6||std::fabs(projection[3][3])>1e-6)return false;
        const glm::dmat4 camera=glm::inverse(glm::dmat4(view));
        origin=glm::dvec3(camera[3]);
        horizontal=glm::dvec3(camera[0])/double(projection[0][0]);
        vertical=glm::dvec3(camera[1])/double(projection[1][1]);
        center=horizontal*double(projection[2][0])+vertical*double(projection[2][1])-glm::dvec3(camera[2]);
        valid=true;return true;
    }
    glm::dvec3 Ray(const glm::dvec2& uv) const {return center+uv.x*horizontal+uv.y*vertical;}
    glm::vec4 Point(const glm::dvec2& uv,double depth) const {
        if(!valid||!std::isfinite(depth)||depth<=0)return glm::vec4(0);
        const glm::vec4 point(glm::vec3(origin+Ray(uv)*depth),1);
        return geometry::Point(point)?point:glm::vec4(0);
    }
};

inline bool MeasuredDepth(uint16_t millimetres,uint8_t confidence,int minimumConfidence,
                          double& metres,float& weight) {
    if(millimetres<=50||confidence<=minimumConfidence)return false;
    // Match the owned depth packet's float conversion exactly.
    metres=millimetres*.001f;
    weight=confidence/255.f;
    return true;
}

// Preserve reliable raw samples. ARCore's dense estimate supplies lower-weight
// coverage where raw depth is absent; it never replaces a measured sample.
// The estimate weight is a fusion heuristic, not a calibrated probability.
inline bool SelectDepth(uint16_t raw,uint8_t confidence,uint16_t estimate,int minimumConfidence,
                        double& metres,float& weight,bool& measured) {
    measured=MeasuredDepth(raw,confidence,minimumConfidence,metres,weight);
    if(measured)return true;
    if(estimate<150||estimate>8000)return false;
    metres=estimate*.001f;weight=.1f;return true;
}
} }
#endif
