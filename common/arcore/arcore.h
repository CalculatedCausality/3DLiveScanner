#ifndef ARCORE_ARCORE_H
#define ARCORE_ARCORE_H

#include <map>
#include <array>
#include <jni.h>
#include <arcore/camera.h>
#include <data/image.h>
#include <data/mesh.h>
#include <gl/renderer.h>
#include <arcore/backend.h>
#include <arcore/session_state.h>
#include <arcore/depth_measurement.h>

namespace oc {

    class ARCore : public ARBackend {
    public:
        ARCore(void *env, void *context, bool faceMode, bool depthCamera = false);

        ~ARCore();

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

        void RemoveFaceDetails() override { face_not_all = true; }

        void RenderCamera(ARCoreCamera::Effect effect = ARCoreCamera::GRAYSCALE, int scale = 1) override;

        void SetNVScheme(ARCoreCamera::NightVisionScheme s) override { camera.SetNVScheme(s); }

        void SetOffset(float value) override { offset = value; }

        void SetResolution(float res) override { resolution = res; }

        Image* GetDepthMap(bool confidence, bool increasing, int s = 1) override;
        void ConfigureDepthTest(bool enabled,uint64_t generation) override {
            depth_test_enabled_=enabled;depth_test_generation_=generation;
            if(!enabled) depth_test_frame_.reset();
        }
        std::shared_ptr<depth_test::Frame> TakeDepthTestFrame() override {
            auto result=std::move(depth_test_frame_);return result;
        }
        std::string GetCaptureDiagnostics() override {
            return capture_diagnostics_+"; references="+std::to_string(reference_count_)
                    +"; active_reference="+std::to_string(active_reference_)
                    +"; reference_revisits="+std::to_string(reference_revisits_)
                    +"; reference_valid="+(reference_valid_?"true":"false");
        }

    private:
        glm::mat4 GetMatrix(ArPose* ar_pose);

        glm::mat4 GetZeroTransform();
        bool UpdateReferenceFrame(const glm::mat4& rawView);
        void ReadTextureCalibration(ArCamera* camera);
        void LogDepthCalibration(int width,int height);

        glm::vec4 ToPoint(glm::dmat4& screen2world, double& len,
                int32_t& depthWidth, int32_t& depthHeight, int& x, int& y, double& depth);

        bool UpdateAnchor();

        void UpdateFace(glm::mat4 matrix);

        void UpdateFeaturePoints();

        std::map<id3d, ArAnchor*> ar_anchor_list;
        ArSession *ar_session_ = nullptr;
        ArFrame *ar_frame_ = nullptr;
        std::pair<id3d, ArAnchor*> ar_zero_;
        Mesh face_mesh;

        ARCoreCamera camera;

        glm::mat4 view_mat = glm::mat4(1);
        glm::mat4 projection_mat = glm::mat4(1);
        glm::mat4 zero_transform_ = glm::mat4(1);
        bool reference_valid_ = false;
        int64_t last_reference_attempt_ms_ = 0;
        struct Reference { ArAnchor* anchor=nullptr; glm::mat4 model_pose{1}; };
        std::array<Reference,32> reference_history_;
        size_t reference_count_=0,active_reference_=0;
        uint64_t reference_revisits_=0;
        glm::mat4 texture_pose_world_{1};
        float texture_focal_[2]{},texture_center_[2]{};
        int32_t texture_dimensions_[2]{};
        bool texture_calibration_valid_=false,texture_calibration_logged_=false;
        std::string capture_diagnostics_;
        geometry::DepthProjection depth_projection_;
        uint64_t depth_source_frames_=0,depth_source_points_=0,depth_source_repeated_=0,depth_source_estimated_=0;
        bool frame_valid_ = false;

        bool face_mode_;
        bool has_coordinate_system_;
        bool has_depth_sensor;
        std::vector<glm::vec4> points;
        float offset;
        float resolution;
        bool face_not_all = false;
        bool texture_initialized_ = false;
        bool useDepth = false;
        bool useDepthRaw;
        bool useSmoothedDepth = false;
        arcore_state::SessionState session_state_;
        ArStatus last_session_error_ = AR_SUCCESS;
        ArStatus last_depth_error_ = AR_SUCCESS;
        int viewportWidth;
        int viewportHeight;
        int64_t lastDepthTimestamp;
        bool depth_test_enabled_=false;
        uint64_t depth_test_generation_=0;
        std::shared_ptr<depth_test::Frame> depth_test_frame_;
    };
}

#endif
