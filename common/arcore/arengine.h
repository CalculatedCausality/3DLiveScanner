#ifndef ARCORE_ARENGINE_H
#define ARCORE_ARENGINE_H

#include <map>
#include <jni.h>
#include <arcore/camera.h>
#include <data/image.h>
#include <data/mesh.h>
#include <gl/renderer.h>
#include <arcore/backend.h>
#include <media/NdkImageReader.h>

namespace oc {

    class AREngine : public ARBackend {
    public:
        AREngine(void *env, void *context, bool depthCamera = true, bool faceMode = false, bool flashlight = false);

        ~AREngine();

        void Clear(bool detach) override;

        void OnPause() override;

        void OnResume() override;

        void OnDisplayGeometryChanged(int display_rotation, int width, int height) override;

        void Configure(void* session, void* frame) override;

        float CountFrameError() override;

        bool Process(bool update = true) override;

        std::vector<glm::vec3> GetActiveAnchors() override;

        std::vector<float> GetDistortion() override;

        glm::vec3 HitTest(int x, int y) override;

        Mesh GetFace(glm::mat4 matrix) override { UpdateFace(matrix); return face_mesh; };

        std::vector<glm::vec4> GetPointCloud() override { UpdateFeaturePoints(); return points; }

        glm::mat4 GetProjection() override { return projection_mat; }

        glm::mat4 GetView() override { return view_mat; }

        bool HasCoordinateSystem() override { return has_coordinate_system_; }

        void RemoveFaceDetails() override;

        void RenderCamera(ARCoreCamera::Effect effect = ARCoreCamera::GRAYSCALE, int scale = 1) override;

        void SetNVScheme(ARCoreCamera::NightVisionScheme s) override { camera.SetNVScheme(s); }

        void SetOffset(float value) override { offset = value; }

        void SetResolution(float res) override { resolution = res; }

        Image* GetDepthMap(bool confidence, bool increasing, int s = 1) override;
    private:
        bool UpdateAnchor();

        void UpdateFace(glm::mat4 matrix);

        void UpdateFeaturePoints();

        std::map<id3d, HwArAnchor*> ar_anchor_list;
        HwArSession *ar_session_ = nullptr;
        HwArFrame *ar_frame_ = nullptr;
        Mesh face_mesh;

        ARCoreCamera camera;
        glm::mat4 view_mat = glm::mat4(1);
        glm::mat4 projection_mat = glm::mat4(1);
        bool frame_valid_ = false;

        bool face_mode_;
        bool has_coordinate_system_;
        std::vector<glm::vec4> points;
        bool texture_initialized_ = false;
        float offset;
        float resolution;
        bool useDepth;
        int viewportWidth;
        int viewportHeight;
    };
}

#endif
