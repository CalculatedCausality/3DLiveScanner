#ifndef TANGO_GEOMETRY_VALIDATION_H
#define TANGO_GEOMETRY_VALIDATION_H

#include <arcore/geometry_validation.h>
#include <tango_3d_reconstruction_api.h>

namespace oc {
namespace geometry {

inline bool Pose(const Tango3DR_Pose* pose) {
    if (!pose) return false;
    double norm = 0;
    for (int i = 0; i < 3; ++i) if (!std::isfinite(pose->translation[i])) return false;
    for (int i = 0; i < 4; ++i) {
        if (!std::isfinite(pose->orientation[i])) return false;
        norm += pose->orientation[i] * pose->orientation[i];
    }
    return std::fabs(norm - 1) < 0.01;
}

inline bool Cloud(const Tango3DR_PointCloud* cloud) {
    if (!cloud || !cloud->num_points || !cloud->points || !std::isfinite(cloud->timestamp)) return false;
    for (uint32_t i = 0; i < cloud->num_points; ++i) {
        const float* p = cloud->points[i];
        if (!Point(glm::vec4(p[0], p[1], p[2], p[3])) || p[2] <= 0) return false;
    }
    return true;
}

inline bool Mesh(const Tango3DR_Mesh* mesh) {
    if (!mesh) return false;
    // Empty segments are valid deletions produced by space clearing.
    if (!mesh->num_vertices) return mesh->num_faces == 0;
    if (!mesh->vertices || !mesh->colors || (mesh->num_faces && !mesh->faces)) return false;
    for (uint32_t i = 0; i < mesh->num_vertices; ++i) {
        for (int j = 0; j < 3; ++j) if (!std::isfinite(mesh->vertices[i][j])) return false;
    }
    for (uint32_t i = 0; i < mesh->num_faces; ++i) {
        for (int j = 0; j < 3; ++j) if (mesh->faces[i][j] >= mesh->num_vertices) return false;
    }
    return true;
}

} // namespace geometry
} // namespace oc
#endif
