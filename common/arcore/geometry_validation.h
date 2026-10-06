#ifndef ARCORE_GEOMETRY_VALIDATION_H
#define ARCORE_GEOMETRY_VALIDATION_H

#include <cmath>
#include <cstdint>
#include <climits>
#include <glm/glm.hpp>

namespace oc {
namespace geometry {

inline bool Finite(const glm::vec4& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::isfinite(v.w);
}

inline bool Finite(const glm::mat4& m) {
    for (int i = 0; i < 4; ++i) if (!Finite(m[i])) return false;
    return true;
}

inline bool Invertible(const glm::mat4& m) {
    if (!Finite(m)) return false;
    const double determinant = glm::determinant(glm::dmat4(m));
    return std::isfinite(determinant) && determinant != 0 && Finite(glm::inverse(m));
}

// Camera poses are rigid transforms, not projection matrices. Allow SDK rounding,
// but reject singular/scaled/reflected poses before decomposition or integration.
inline bool RigidPose(const glm::mat4& m) {
    if (!Finite(m)) return false;
    const float tolerance = 0.01f;
    if (std::fabs(m[0][3]) > tolerance || std::fabs(m[1][3]) > tolerance ||
        std::fabs(m[2][3]) > tolerance || std::fabs(m[3][3] - 1) > tolerance) return false;
    for (int i = 0; i < 3; ++i) {
        for (int j = i; j < 3; ++j) {
            const float dot = glm::dot(glm::vec3(m[i]), glm::vec3(m[j]));
            if (std::fabs(dot - (i == j ? 1.f : 0.f)) > tolerance) return false;
        }
    }
    return std::fabs(glm::determinant(glm::mat3(m)) - 1.f) < tolerance;
}

inline bool Point(const glm::vec4& p) {
    return Finite(p) && p.w > 0 && p.w <= 1;
}

inline bool MaskCoordinates(const glm::vec4& projected, int width, int height, int& x, int& y) {
    if (!Finite(projected) || width <= 0 || height <= 0) return false;
    const double px = (0.5 + 0.5 * projected.x) * (width - 1);
    const double py = (0.5 + 0.5 * projected.y) * (height - 1);
    // Leave headroom for differences and Bresenham's doubled error terms.
    if (px <= INT_MIN / 8 || px >= INT_MAX / 8 || py <= INT_MIN / 8 || py >= INT_MAX / 8) return false;
    x = static_cast<int>(px);
    y = static_cast<int>(py);
    return true;
}

inline bool CameraPoint(const glm::vec4& point, int width, int height) {
    // Here w may be a homogeneous transform result rather than confidence.
    if (!Finite(point) || point.z <= 0) return false;
    glm::vec4 projected(point.x / point.z, point.y / point.z, point.z, 1);
    int x, y;
    return MaskCoordinates(projected, width, height, x, y);
}

// Integer grid keys must not receive NaNs or values outside their representation.
inline bool GridPosition(const glm::vec3& p, float density) {
    for (int i = 0; i < 3; ++i) {
        const double cell = static_cast<double>(p[i]) / density;
        if (!std::isfinite(cell) || cell <= INT_MIN / 2 || cell >= INT_MAX / 2) return false;
    }
    return density > 0;
}

template <typename Position>
inline double GridDistanceSquared(const Position& a, const Position& b) {
    const double dx = double(a.x) - double(b.x);
    const double dy = double(a.y) - double(b.y);
    const double dz = double(a.z) - double(b.z);
    return dx * dx + dy * dy + dz * dz;
}

// Byte-addressed plane: rows may be padded and the last row need not include
// trailing padding. Validate once, then read without alignment assumptions.
struct Plane {
    const uint8_t* data = nullptr;
    int32_t length = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t rowStride = 0;
    int32_t pixelStride = 0;

    bool Valid(int bytes) const {
        if (!data || length <= 0 || width <= 0 || height <= 0 ||
            rowStride <= 0 || pixelStride < bytes || bytes <= 0) return false;
        const int64_t rowBytes = int64_t(width - 1) * pixelStride + bytes;
        const int64_t required = int64_t(height - 1) * rowStride + rowBytes;
        return rowBytes <= rowStride && required <= length;
    }
    uint8_t Byte(int x, int y) const {
        return data[int64_t(y) * rowStride + int64_t(x) * pixelStride];
    }
    uint16_t Depth(int x, int y) const {
        const uint8_t* p = data + int64_t(y) * rowStride + int64_t(x) * pixelStride;
        return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
    }
    bool SameSize(const Plane& other) const {
        return width == other.width && height == other.height;
    }
};

// Preserve the existing ray mapping; reject undefined homogeneous divisions.
inline glm::vec4 DepthPoint(const glm::dmat4& screen2world, const glm::dvec2& uv,
                           double rayLength, double depth) {
    if (!std::isfinite(depth) || depth <= 0 || !std::isfinite(rayLength) || rayLength <= 0)
        return glm::vec4(0);
    glm::dvec4 a = screen2world * glm::dvec4(uv, 0, 1);
    glm::dvec4 b = screen2world * glm::dvec4(uv, 1, 1);
    if (!std::isfinite(a.w) || !std::isfinite(b.w) || std::fabs(a.w) < 1e-12 || std::fabs(b.w) < 1e-12)
        return glm::vec4(0);
    a /= std::fabs(a.w);
    b /= std::fabs(b.w);
    a.w = b.w = 1;
    const glm::vec4 p(a + (b - a) / rayLength * depth);
    return Point(p) ? p : glm::vec4(0);
}

inline bool InterpolatedDepth(double coordinate, double start, double end,
                              double startDepth, double endDepth, double& depth) {
    if (end <= start || startDepth <= 0 || endDepth <= 0) return false;
    const double t = (coordinate - start) / (end - start);
    depth = startDepth + t * (endDepth - startDepth);
    return std::isfinite(depth) && t >= 0 && t <= 1 && depth > 0;
}

} // namespace geometry
} // namespace oc
#endif
