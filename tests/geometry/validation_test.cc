#include <arcore/geometry_validation.h>
#include <tango/geometry_validation.h>
#include <glm/gtc/matrix_transform.hpp>
#include <cassert>
#include <limits>
#include <iostream>

using namespace oc::geometry;

int main() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    // Rigid poses, including large translations, survive; bad transforms do not.
    glm::mat4 pose = glm::translate(glm::mat4(1), glm::vec3(1234, -500, 17));
    pose = glm::rotate(pose, 0.6f, glm::vec3(0, 1, 0));
    assert(RigidPose(pose));
    assert(!RigidPose(glm::mat4(0)));
    assert(!RigidPose(glm::scale(pose, glm::vec3(-1, 1, 1))));
    assert(!RigidPose(glm::scale(pose, glm::vec3(2))));
    glm::mat4 invalid = pose;
    invalid[3][1] = nan;
    assert(!RigidPose(invalid));
    invalid[3][1] = inf;
    assert(!RigidPose(invalid));
    glm::mat4 projection = glm::perspective(1.f, 0.5625f, 0.001f, 100.f);
    assert(Invertible(projection));
    assert(!Invertible(glm::mat4(0)));
    assert(!Invertible(invalid));
    assert(Point(glm::vec4(0, 0, 0, 0.01f))); // no arbitrary confidence threshold
    assert(Point(glm::vec4(0, 0, 500, 1))); // no new range filter
    assert(!Point(glm::vec4(nan, 0, 0, 1)));
    assert(!Point(glm::vec4(0, inf, 0, 1)));
    assert(!Point(glm::vec4(0, 0, 0, 0)));
    assert(!Point(glm::vec4(0, 0, 0, 1.1f)));
    assert(CameraPoint(glm::vec4(1, 2, 5, 0.1f), 360, 640));
    assert(CameraPoint(glm::vec4(1, 2, 5, 1.000001f), 360, 640)); // homogeneous rounding
    assert(CameraPoint(glm::vec4(5, 0, 1, 1), 360, 640)); // outside view, still representable
    assert(!CameraPoint(glm::vec4(1, 0, 0, 1), 360, 640));
    assert(!CameraPoint(glm::vec4(1, 0, -1, 1), 360, 640));
    assert(!CameraPoint(glm::vec4(1, 0, std::numeric_limits<float>::min(), 1), 360, 640));
    int pixelX, pixelY;
    assert(!MaskCoordinates(glm::vec4(nan, 0, 1, 1), 360, 640, pixelX, pixelY));
    assert(!MaskCoordinates(glm::vec4(1e30f, 0, 1, 1), 360, 640, pixelX, pixelY));
    assert(GridPosition(glm::vec3(-100, 0, 100), 0.1f));
    assert(!GridPosition(glm::vec3(nan), 0.1f));
    assert(!GridPosition(glm::vec3(std::numeric_limits<float>::max()), 0.1f));
    assert(GridDistanceSquared(glm::ivec3(100000, 0, 0), glm::ivec3(0)) == 1e10);
    assert(GridDistanceSquared(glm::ivec3(INT_MAX, 0, 0), glm::ivec3(INT_MIN, 0, 0)) > 1e19);

    // Unaligned depth with row and pixel padding; last row has no tail padding.
    uint8_t buffer[] = {99, 0x34, 0x12, 99, 0x78, 0x56, 99, 99, 99, 0xff, 0xff, 99, 0, 0};
    Plane depth;
    depth.data = buffer + 1;
    depth.length = sizeof(buffer) - 1;
    depth.width = depth.height = 2;
    depth.rowStride = 8;
    depth.pixelStride = 3;
    assert(depth.Valid(2));
    assert(depth.Depth(0, 0) == 0x1234);
    assert(depth.Depth(1, 0) == 0x5678);
    assert(depth.Depth(0, 1) == 65535); // ARCore full 16-bit millimetres
    assert((depth.Depth(0, 1) & 0x1fff) == 8191); // Huawei packed DEPTH16
    assert(depth.Depth(1, 1) == 0);
    --depth.length;
    assert(!depth.Valid(2));
    ++depth.length;
    depth.rowStride = 4;
    assert(!depth.Valid(2));
    depth.rowStride = 8;
    depth.pixelStride = 1;
    assert(!depth.Valid(2));
    depth.pixelStride = 3;
    depth.width = INT_MAX;
    depth.height = INT_MAX;
    depth.rowStride = INT_MAX;
    assert(!depth.Valid(2)); // no overflowing length arithmetic

    uint8_t confidenceBytes[] = {255, 0, 77, 128, 200};
    Plane confidence;
    confidence.data = confidenceBytes;
    confidence.length = sizeof(confidenceBytes);
    confidence.width = confidence.height = 2;
    confidence.pixelStride = 1;
    confidence.rowStride = 3;
    assert(confidence.Valid(1));
    assert(confidence.Byte(0, 1) == 128);
    assert(confidence.Byte(1, 1) == 200);
    assert(!confidence.SameSize(depth));
    confidence.data = nullptr;
    assert(!confidence.Valid(1));

    // Vertical hole filling must interpolate Y (the X coordinates are equal).
    double value = 0;
    assert(InterpolatedDepth(3, 1, 5, 1, 3, value) && value == 2);
    assert(!InterpolatedDepth(2, 2, 2, 1, 3, value));
    assert(!InterpolatedDepth(3, 1, 5, 0, 3, value));
    assert(!InterpolatedDepth(3, 1, 5, nan, 3, value));
    const glm::dmat4 inverse = glm::inverse(glm::dmat4(projection));
    assert(Point(DepthPoint(inverse, glm::dvec2(0.2, -0.2), 99.999, 2)));
    assert(!Point(DepthPoint(inverse, glm::dvec2(0), 99.999, -0.1)));
    assert(!Point(DepthPoint(inverse, glm::dvec2(0), 99.999, nan)));
    assert(!Point(DepthPoint(glm::dmat4(0), glm::dvec2(0), 99.999, 2)));

    Tango3DR_Pose sdkPose = {};
    sdkPose.orientation[3] = 1;
    assert(Pose(&sdkPose));
    sdkPose.orientation[3] = 0;
    assert(!Pose(&sdkPose));
    sdkPose.orientation[3] = 1;
    sdkPose.translation[0] = nan;
    assert(!Pose(&sdkPose));
    assert(!Pose(nullptr));
    Tango3DR_Vector4 points[] = {{0, 0, 1, 1}, {1, 2, 5, 0.1f}};
    Tango3DR_PointCloud cloud = {};
    cloud.num_points = 2;
    cloud.points = points;
    assert(Cloud(&cloud));
    points[1][2] = 0;
    assert(!Cloud(&cloud));
    points[1][2] = inf;
    assert(!Cloud(&cloud));
    points[1][2] = 5;
    cloud.timestamp = nan;
    assert(!Cloud(&cloud));
    cloud.num_points = 0;
    assert(!Cloud(&cloud));

    Tango3DR_Mesh mesh = {};
    assert(Mesh(&mesh)); // legitimate empty/deletion segment
    Tango3DR_Vector3 vertices[] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
    Tango3DR_Face faces[] = {{0, 1, 2}};
    Tango3DR_Color colors[3] = {};
    mesh.num_vertices = 3;
    mesh.num_faces = 1;
    mesh.vertices = vertices;
    mesh.faces = faces;
    mesh.colors = colors;
    assert(Mesh(&mesh));
    faces[0][2] = 3;
    assert(!Mesh(&mesh));
    faces[0][2] = 2;
    vertices[0][0] = nan;
    assert(!Mesh(&mesh));
    vertices[0][0] = 0;
    mesh.colors = nullptr;
    assert(!Mesh(&mesh));
    std::cout << "geometry validation regressions passed\n";
}
