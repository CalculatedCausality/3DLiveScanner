#ifndef ARCORE_BACKEND_H
#define ARCORE_BACKEND_H

#include <arcore/camera.h>
#include <data/image.h>
#include <data/mesh.h>
#include <depth/experimental.h>

namespace oc {

class ARBackend {
public:
    virtual ~ARBackend() = default;

    virtual void Clear(bool detach) = 0;
    virtual void OnPause() = 0;
    virtual void OnResume() = 0;
    virtual void OnDisplayGeometryChanged(int display_rotation, int width, int height) = 0;
    virtual void Configure(void* session, void* frame) = 0;
    virtual float CountFrameError() = 0;
    virtual bool Process(bool update) = 0;
    virtual std::vector<glm::vec3> GetActiveAnchors() = 0;
    virtual std::vector<float> GetDistortion() = 0;
    virtual Mesh GetFace(glm::mat4 matrix) = 0;
    virtual Image* GetDepthMap(bool confidence, bool increasing, int scale) = 0;
    virtual std::vector<glm::vec4> GetPointCloud() = 0;
    virtual glm::mat4 GetProjection() = 0;
    virtual glm::mat4 GetView() = 0;
    virtual bool HasCoordinateSystem() = 0;
    virtual glm::vec3 HitTest(int x, int y) = 0;
    virtual void RemoveFaceDetails() = 0;
    virtual void RenderCamera(ARCoreCamera::Effect effect, int scale) = 0;
    virtual void SetNVScheme(ARCoreCamera::NightVisionScheme scheme) = 0;
    virtual void SetOffset(float offset) = 0;
    virtual void SetResolution(float resolution) = 0;
    virtual void ConfigureDepthTest(bool, uint64_t) {}
    virtual std::shared_ptr<depth_test::Frame> TakeDepthTestFrame() { return {}; }
    virtual std::string GetCaptureDiagnostics() { return {}; }
};

}

#endif
