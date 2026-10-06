#include <arcore/arcore.h>
#include <arcore/geometry_validation.h>
#include <depth/frame_capture.h>
#include <mutex>
#include <memory>
#include <chrono>

namespace oc {

    namespace {
        using ScopedImage = std::unique_ptr<ArImage, decltype(&ArImage_release)>;

        int64_t NowMs() {
            return std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
        }

        void ReportStatus(const char* operation, ArStatus result, ArStatus& previous) {
            if (result != AR_SUCCESS && result != previous && !arcore_state::DepthUnavailable(result)) {
                LOGE("ARCore %s failed (%d)", operation, result);
            }
            previous = result;
        }

        ArStatus DisableDepthSensor(ArSession* session) {
            ArCameraConfigFilter* filter = nullptr;
            ArCameraConfigList* list = nullptr;
            ArCameraConfig* config = nullptr;
            ArCameraConfigFilter_create(session, &filter);
            ArCameraConfigList_create(session, &list);
            ArCameraConfig_create(session, &config);
            ArStatus result = AR_ERROR_FATAL;
            if (filter && list && config) {
                ArSession_getCameraConfig(session, config);
                uint32_t usage = 0;
                ArCameraConfig_getDepthSensorUsage(session, config, &usage);
                if (usage == AR_CAMERA_CONFIG_DEPTH_SENSOR_USAGE_DO_NOT_USE) {
                    result = AR_SUCCESS; // retain the default FPS/resolution on Pixels
                } else {
                    ArCameraConfigFilter_setFacingDirection(session, filter, AR_CAMERA_CONFIG_FACING_DIRECTION_BACK);
                    ArCameraConfigFilter_setDepthSensorUsage(session, filter, AR_CAMERA_CONFIG_DEPTH_SENSOR_USAGE_DO_NOT_USE);
                    ArSession_getSupportedCameraConfigsWithFilter(session, filter, list);
                    int32_t count = 0;
                    ArCameraConfigList_getSize(session, list, &count);
                    result = AR_ERROR_UNSUPPORTED_CONFIGURATION;
                    if (count > 0) {
                        ArCameraConfigList_getItem(session, list, 0, config);
                        result = ArSession_setCameraConfig(session, config);
                    }
                }
            }
            if (config) ArCameraConfig_destroy(config);
            if (list) ArCameraConfigList_destroy(list);
            if (filter) ArCameraConfigFilter_destroy(filter);
            return result;
        }

        bool UsesDepthSensor(ArSession* session) {
            ArCameraConfig* config = nullptr;
            ArCameraConfig_create(session, &config);
            if (!config) return false;
            ArSession_getCameraConfig(session, config);
            uint32_t usage = 0;
            ArCameraConfig_getDepthSensorUsage(session, config, &usage);
            ArCameraConfig_destroy(config);
            return usage == AR_CAMERA_CONFIG_DEPTH_SENSOR_USAGE_REQUIRE_AND_USE;
        }

        geometry::Plane ReadPlane(ArSession* session, ArImage* image) {
            geometry::Plane p;
            if (!image) return p;
            ArImage_getWidth(session, image, &p.width);
            ArImage_getHeight(session, image, &p.height);
            ArImage_getPlaneRowStride(session, image, 0, &p.rowStride);
            ArImage_getPlanePixelStride(session, image, 0, &p.pixelStride);
            ArImage_getPlaneData(session, image, 0, &p.data, &p.length);
            return p;
        }
    }

    ARCore::ARCore(void *env, void *context, bool faceMode, bool depthCamera)
            : face_mode_(faceMode), has_coordinate_system_(false), has_depth_sensor(depthCamera),
              offset(0), resolution(0), useDepthRaw(false), viewportWidth(0), viewportHeight(0),
              lastDepthTimestamp(0) {
        ar_zero_.second = nullptr;
#ifndef ARCORE_BACKPORT
        if (env && context) {
            ArStatus status;
            if (faceMode) {
                ArSessionFeature features[2] = {AR_SESSION_FEATURE_FRONT_CAMERA, AR_SESSION_FEATURE_END_OF_LIST};
                status = ArSession_createWithFeatures(env, context, features, &ar_session_);
            } else {
                status = ArSession_create(env, context, &ar_session_);
            }
            ReportStatus("create", status, last_session_error_);
            if (status != AR_SUCCESS || !ar_session_) return;
            if (!faceMode && !depthCamera) {
                // Select a documented camera config once, before any anchors
                // exist, rather than relying on a private session-feature enum.
                status = DisableDepthSensor(ar_session_);
                ReportStatus("camera config", status, last_session_error_);
                if (status != AR_SUCCESS) {
                    ArSession_destroy(ar_session_);
                    ar_session_ = nullptr;
                    return;
                }
            }

            ArConfig *ar_config = nullptr;
            ArConfig_create(ar_session_, &ar_config);
            if (!ar_config) {
                ReportStatus("config allocation", AR_ERROR_RESOURCE_EXHAUSTED, last_session_error_);
                return;
            }
            has_depth_sensor = !faceMode && UsesDepthSensor(ar_session_);
            ArConfig_setFocusMode(ar_session_, ar_config, AR_FOCUS_MODE_FIXED);
            ArConfig_setPlaneFindingMode(ar_session_, ar_config, AR_PLANE_FINDING_MODE_DISABLED);
            int32_t automaticSupported = 0, rawSupported = 0;
            ArDepthMode depthMode = AR_DEPTH_MODE_DISABLED;
            if (faceMode)
                ArConfig_setAugmentedFaceMode(ar_session_, ar_config, AR_AUGMENTED_FACE_MODE_MESH3D);
            else {
                ArSession_isDepthModeSupported(ar_session_, AR_DEPTH_MODE_AUTOMATIC, &automaticSupported);
                ArSession_isDepthModeSupported(ar_session_, AR_DEPTH_MODE_RAW_DEPTH_ONLY, &rawSupported);
                depthMode = arcore_state::SelectDepthMode(depthCamera, automaticSupported != 0, rawSupported != 0);
                ArConfig_setDepthMode(ar_session_, ar_config, depthMode);
            }
            ArConfig_setUpdateMode(ar_session_, ar_config, AR_UPDATE_MODE_BLOCKING);
            status = ArSession_configure(ar_session_, ar_config);
            // A supported raw-only mode is a useful fallback if automatic
            // depth becomes unavailable for the selected runtime camera config.
            if (status == AR_ERROR_UNSUPPORTED_CONFIGURATION &&
                depthMode == AR_DEPTH_MODE_AUTOMATIC && rawSupported) {
                depthMode = AR_DEPTH_MODE_RAW_DEPTH_ONLY;
                ArConfig_setDepthMode(ar_session_, ar_config, depthMode);
                status = ArSession_configure(ar_session_, ar_config);
            }
            ArConfig_destroy(ar_config);
            ReportStatus("configure", status, last_session_error_);
            if (status != AR_SUCCESS) {
                ArSession_destroy(ar_session_);
                ar_session_ = nullptr;
                return;
            }
            ArFrame_create(ar_session_, &ar_frame_);
            useDepth = depthMode != AR_DEPTH_MODE_DISABLED;
            useDepthRaw = useDepth && rawSupported != 0;
            useSmoothedDepth = depthMode == AR_DEPTH_MODE_AUTOMATIC;
        } else
#endif
        {
            useDepth = depthCamera;
            useDepthRaw = depthCamera;
        }
    }

    ARCore::~ARCore() {
        Clear(true);
        if (ar_frame_) ArFrame_destroy(ar_frame_);
        if (ar_session_) ArSession_destroy(ar_session_);
    }

    void ARCore::Clear(bool detach) {
        frame_valid_ = false;
        points.clear();
        lastDepthTimestamp = 0;
#if SCANNER_MODERN
        for(size_t i=0;i<reference_count_;++i)if(reference_history_[i].anchor) {
            if(detach&&ar_session_)ArAnchor_detach(ar_session_,reference_history_[i].anchor);
            ArAnchor_release(reference_history_[i].anchor);reference_history_[i].anchor=nullptr;
        }
        reference_count_=active_reference_=0;reference_revisits_=0;
        ar_zero_.second=nullptr; // non-owning alias of the active history entry
#endif
        if (detach && ar_session_) {
            for (auto& anchor : ar_anchor_list) {
                ArAnchor_detach(ar_session_, ar_anchor_list[anchor.first]);
                ArAnchor_release(ar_anchor_list[anchor.first]);
            }
            if (ar_zero_.second) {
                ArAnchor_detach(ar_session_, ar_zero_.second);
                ArAnchor_release(ar_zero_.second);
            }
        }
        ar_anchor_list.clear();
        ar_zero_.second = 0;
        zero_transform_ = glm::mat4(1);
        reference_valid_ = false;
        last_reference_attempt_ms_ = 0;
        texture_calibration_logged_=false;
        capture_diagnostics_.clear();
        has_coordinate_system_ = false;
    }

    void ARCore::OnPause() {
        frame_valid_ = false;
        reference_valid_ = false;
        points.clear();
        session_state_.Paused();
        lastDepthTimestamp = 0;
        if (ar_session_) ArSession_pause(ar_session_);
    }

    void ARCore::OnResume() {
        if (!ar_session_) return;
        reference_valid_ = false;
        ArStatus status = ArSession_resume(ar_session_);
        session_state_.Resumed(status, NowMs());
        ReportStatus("resume", status, last_session_error_);
        lastDepthTimestamp = 0;
        camera.InitializeGlContent();
        texture_initialized_ = false;
    }

    void ARCore::OnDisplayGeometryChanged(int display_rotation, int width, int height) {
        viewportWidth = width;
        viewportHeight = height;
        if (ar_session_) ArSession_setDisplayGeometry(ar_session_, display_rotation, width, height);
    }

    void ARCore::Configure(void *session, void *frame) {
        ar_session_ = static_cast<ArSession *>(session);
        ar_frame_ = static_cast<ArFrame *>(frame);

        frame_valid_ = false;
        points.clear();
        lastDepthTimestamp = 0;
        if (!ar_session_ || !ar_frame_) return;
        ArConfig* config = nullptr;
        ArConfig_create(ar_session_, &config);
        if (!config) return;
        ArSession_getConfig(ar_session_, config);
        ArDepthMode mode = AR_DEPTH_MODE_DISABLED;
        ArConfig_getDepthMode(ar_session_, config, &mode);
        ArConfig_destroy(config);
        int32_t rawSupported = 0;
        ArSession_isDepthModeSupported(ar_session_, AR_DEPTH_MODE_RAW_DEPTH_ONLY, &rawSupported);
        useDepth = mode == AR_DEPTH_MODE_AUTOMATIC || mode == AR_DEPTH_MODE_RAW_DEPTH_ONLY;
        useDepthRaw = useDepth && rawSupported != 0;
        useSmoothedDepth = mode == AR_DEPTH_MODE_AUTOMATIC;
        has_depth_sensor = !face_mode_ && UsesDepthSensor(ar_session_);
        session_state_.Resumed(AR_SUCCESS, NowMs());
    }

    float ARCore::CountFrameError() {
        if (!ar_session_ || !ar_frame_) return 10000;
        int size = 0;
        float error = 10000;
        float data[7] = {0, 0, 0, 1, 0, 0, 0};
        ArPose *ar_pose;
        ArPose_create(ar_session_, data, &ar_pose);
        glm::mat4 matrix = projection_mat * view_mat;

        ArHitResult* hit = 0;
        ArHitResultList* hits = 0;
        ArHitResult_create(ar_session_, &hit);
        ArHitResultList_create(ar_session_, &hits);
        for (glm::vec3& v : GetActiveAnchors()) {
            glm::vec4 point = matrix * glm::vec4(v, 1.0);
            point /= fabs(point.z * point.w);
            point = 0.5f * point + 0.5f;

            ArFrame_hitTest(ar_session_, ar_frame_, viewportWidth * point.x, viewportHeight * point.y, hits);
            ArHitResultList_getSize(ar_session_, hits, &size);
            for (int i = 0; i < size; i++) {
                ArHitResultList_getItem(ar_session_, hits, i, hit);
                ArHitResult_getHitPose(ar_session_, hit, ar_pose);
                ArPose_getPoseRaw(ar_session_, ar_pose, data);
                glm::vec3 position = glm::vec3(data[4], data[5], data[6]);
                float dst = glm::distance(v, position);
                if (dst > 0) {
                    error = glm::min(error, dst);
                }
            }
        }

        ArHitResultList_destroy(hits);
        ArHitResult_destroy(hit);
        ArPose_destroy(ar_pose);
        return error;
    }

    bool ARCore::Process(bool update) {
        frame_valid_ = false;
        points.clear();
        if (!ar_session_ || !ar_frame_) return false;
        if (session_state_.RetryDue(NowMs())) {
            ArStatus status = ArSession_resume(ar_session_);
            session_state_.Resumed(status, NowMs());
            ReportStatus("resume retry", status, last_session_error_);
            texture_initialized_ = false;
        }
        if (!session_state_.Running()) return false;
        if (update) {
            if (!texture_initialized_) {
                ArSession_setCameraTextureName(ar_session_, camera.GetTextureName());
                texture_initialized_ = true;
            }
            ArStatus status = ArSession_update(ar_session_, ar_frame_);
            ReportStatus("update", status, last_session_error_);
            session_state_.Updated(status, NowMs());
            if (status != AR_SUCCESS) {
                lastDepthTimestamp = 0;
                if (status == AR_ERROR_CAMERA_NOT_AVAILABLE) ArSession_pause(ar_session_);
                if (status == AR_ERROR_TEXTURE_NOT_SET) texture_initialized_ = false;
                return false;
            }
        }
        int64_t frameTimestamp = 0;
        ArFrame_getTimestamp(ar_session_, ar_frame_, &frameTimestamp);
        if (frameTimestamp <= 0) return false; // normal camera warm-up

        ArCamera *ar_camera = nullptr;
        ArFrame_acquireCamera(ar_session_, ar_frame_, &ar_camera);
        if (!ar_camera) return false;
        // A paused camera can still return a stale pose. Missing tracking is
        // normal and must not be mistaken for a usable reconstruction frame.
        if (!face_mode_) {
            ArTrackingState state = AR_TRACKING_STATE_STOPPED;
            ArCamera_getTrackingState(ar_session_, ar_camera, &state);
            if (state != AR_TRACKING_STATE_TRACKING) {
                lastDepthTimestamp = 0;
                ArCamera_release(ar_camera);
                return false;
            }
        }
        glm::mat4 view(0), projection(0);
        ArCamera_getViewMatrix(ar_session_, ar_camera, glm::value_ptr(view));
        ArCamera_getProjectionMatrix(ar_session_, ar_camera, 0.001f, 100.f,
                                       glm::value_ptr(projection));
#if SCANNER_MODERN
        ReadTextureCalibration(ar_camera);
        if (!face_mode_ && !UpdateReferenceFrame(view)) {
            ArCamera_release(ar_camera);
            return false;
        }
#endif
        view = view * GetZeroTransform();
        ArCamera_release(ar_camera);
        if (!geometry::RigidPose(view) || !geometry::Invertible(projection)) {
            lastDepthTimestamp = 0;
            LOGE("ARCore: rejected invalid camera geometry");
            return false;
        }
        view_mat = view;
        projection_mat = projection;
#if SCANNER_MODERN
        if (!face_mode_ && useDepth && !texture_calibration_logged_ && reference_valid_) {
            ArImage* diagnostic=nullptr;
            const ArStatus status=useDepthRaw?ArFrame_acquireRawDepthImage16Bits(ar_session_,ar_frame_,&diagnostic):
                                                 ArFrame_acquireDepthImage16Bits(ar_session_,ar_frame_,&diagnostic);
            ScopedImage owner(diagnostic,ArImage_release);
            if(status==AR_SUCCESS&&diagnostic) {
                auto plane=ReadPlane(ar_session_,diagnostic);
                if(plane.Valid(2)&&depth_projection_.Configure(projection_mat,view_mat)) {
                    camera.InitARCore(ar_session_,ar_frame_);
                    LogDepthCalibration(plane.width,plane.height);
                }
            }
        }
#endif

        if (face_mode_) {
            frame_valid_ = true;
        } else {
            // Replenish auxiliary anchors after they stop tracking. The zero
            // anchor/origin is deliberately retained so accepted geometry stays
            // in the same coordinate system.
#if SCANNER_MODERN
            frame_valid_ = reference_valid_;
#else
            frame_valid_ = !GetActiveAnchors().empty() || UpdateAnchor();
#endif
        }
        return frame_valid_;
    }

    void ARCore::RenderCamera(ARCoreCamera::Effect effect, int scale) {
        if (!ar_session_ || !ar_frame_ || !session_state_.Running()) return;
        if (effect >= ARCoreCamera::DEPTH) {
            Image* img = 0;
            if (effect == ARCoreCamera::NIGHTVISION)
                img = GetDepthMap(true, false, scale);
            else if (effect == ARCoreCamera::DEPTH_INV)
                img = GetDepthMap(false, false, scale);
            else
                img = GetDepthMap(false, true, scale);
            if (img) {
                GLuint texture = GLSL::Image2GLTexture(img, false);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, texture);
                camera.GetShader()->Bind();
                camera.GetShader()->UniformInt("depth", 0);
                camera.DrawARCore(ar_session_, ar_frame_, effect, viewportWidth, viewportHeight);
                glDeleteTextures(1, &texture);
                delete img;
            }
        } else {
            camera.DrawARCore(ar_session_, ar_frame_, effect, viewportWidth, viewportHeight);
        }
    }

    std::vector<glm::vec3> ARCore::GetActiveAnchors() {
#if SCANNER_MODERN
        if (!reference_valid_ || !ar_zero_.second) return {};
        return {glm::vec3(ar_zero_.first.matrix[3])};
#else
        if (!ar_session_) return {};
        float data[7] = {0, 0, 0, 1, 0, 0, 0};
        ArPose *ar_pose;
        ArPose_create(ar_session_, data, &ar_pose);
        std::vector<glm::vec3> output;
        output.reserve(ar_anchor_list.size());

        glm::mat4 zero = glm::inverse(GetZeroTransform());
        for (auto it = ar_anchor_list.begin(); it != ar_anchor_list.end();) {
            auto& anchor = *it;
            ArTrackingState state = AR_TRACKING_STATE_STOPPED;
            ArAnchor_getTrackingState(ar_session_, anchor.second, &state);
            if (state == AR_TRACKING_STATE_STOPPED) {
                ArAnchor_release(anchor.second);
                it = ar_anchor_list.erase(it);
                continue;
            }
            if (state == AR_TRACKING_STATE_TRACKING) {
                ArAnchor_getPose(ar_session_, anchor.second, ar_pose);
                ArPose_getPoseRaw(ar_session_, ar_pose, data);
                glm::vec3 v = glm::vec3(data[4], data[5], data[6]);
                glm::vec4 p = zero * glm::vec4(v, 1.0f);
                v = p / glm::abs(p.w);
                if (geometry::GridPosition(v, ANCHOR_DENSITY_BASE)) output.push_back(v);
            }
            ++it;
        }
        ArPose_destroy(ar_pose);
        return output;
#endif
    }

    std::vector<float> ARCore::GetDistortion() {
        std::vector<float> output;
        output.reserve(3);
        for (int i = 0; i < 3; i++) {
            output.push_back(0);
        }
        return output;
    }

    glm::vec3 ARCore::HitTest(int x, int y) {
        if (!ar_session_ || !ar_frame_ || !frame_valid_) return glm::vec3(INT_MAX);
        float data[7] = {0, 0, 0, 1, 0, 0, 0};
        ArPose *ar_pose;
        ArPose_create(ar_session_, data, &ar_pose);

        int size = 0;
        ArHitResult* hit = 0;
        ArHitResultList* hits = 0;
        ArHitResult_create(ar_session_, &hit);
        ArHitResultList_create(ar_session_, &hits);
        ArFrame_hitTest(ar_session_, ar_frame_, x, y, hits);
        ArHitResultList_getSize(ar_session_, hits, &size);
        if (size > 0) {
            ArHitResultList_getItem(ar_session_, hits, 0, hit);
            ArHitResult_getHitPose(ar_session_, hit, ar_pose);
            ArPose_getPoseRaw(ar_session_, ar_pose, data);
            ArHitResultList_destroy(hits);
            ArHitResult_destroy(hit);
            ArPose_destroy(ar_pose);
            const glm::vec3 point(data[4], data[5], data[6]);
            return geometry::GridPosition(point, ANCHOR_DENSITY_BASE) ? point : glm::vec3(INT_MAX);
        }
        ArHitResultList_destroy(hits);
        ArHitResult_destroy(hit);
        ArPose_destroy(ar_pose);
        return glm::vec3(INT_MAX);
    }

    Image* ARCore::GetDepthMap(bool confidence, bool increasing, int s) {
        if (s <= 0 || !ar_session_ || !ar_frame_ || !session_state_.Running()) return nullptr;
        if (useDepth) {
            ArImage* image = 0;
            ArStatus result = AR_SUCCESS;
            if (useDepthRaw) {
                result = ArFrame_acquireRawDepthImage16Bits(ar_session_, ar_frame_, &image);
            } else {
                result = ArFrame_acquireDepthImage16Bits(ar_session_, ar_frame_, &image);
            }
            ScopedImage depthImage(image, ArImage_release);
            if (result == AR_SUCCESS) {

                ArImage* confidenceImage = 0;
                geometry::Plane confidencePlane;
                bool hasConfidence = false;
                if (confidence && useDepthRaw) {
                    result = ArFrame_acquireRawDepthConfidenceImage(ar_session_, ar_frame_, &confidenceImage);
                    if (result == AR_SUCCESS) {
                        confidencePlane = ReadPlane(ar_session_, confidenceImage);
                        hasConfidence = confidencePlane.Valid(1);
                    }
                }
                ScopedImage confidenceOwner(confidenceImage, ArImage_release);

                //get depth data
                const geometry::Plane depthPlane = ReadPlane(ar_session_, image);
                int32_t depthWidth = depthPlane.width, depthHeight = depthPlane.height;
                if (!depthPlane.Valid(2) || s > depthWidth || s > depthHeight) {
                    return nullptr;
                }
                hasConfidence = hasConfidence && confidencePlane.SameSize(depthPlane);

                if (depthWidth > 240) {
                    const int factor = depthWidth / 240;
                    if (s > glm::min(depthWidth, depthHeight) / factor) {
                        return nullptr;
                    }
                    s *= factor;
                }
                depthWidth /= s;
                depthHeight /= s;
                Image* output = new Image(depthWidth, depthHeight);
                for (int y = 0; y < depthHeight; y++) {
                    for (int x = 0; x < depthWidth; x++) {
                        int depth = static_cast<int>(depthPlane.Depth(s * x, s * y) * 0.001 * 255);
                        if (!increasing && depth > 0) depth = 768 - depth;
                        output->GetData()[(y * depthWidth + x) * 4 + 0] = camera.Convert(depth, 0);
                        output->GetData()[(y * depthWidth + x) * 4 + 1] = camera.Convert(depth, 1);
                        output->GetData()[(y * depthWidth + x) * 4 + 2] = camera.Convert(depth, 2);
                        output->GetData()[(y * depthWidth + x) * 4 + 3] = 255;
                        if (confidence && hasConfidence) {
                            output->GetData()[(y * depthWidth + x) * 4 + 3] = 128 + confidencePlane.Byte(s * x, s * y) / 2;
                        }
                    }
                }

                return output;
            }
        }
        return 0;
    }

    glm::mat4 ARCore::GetMatrix(ArPose* ar_pose) {
        float matrix[16];
        ArPose_getMatrix(ar_session_, ar_pose, matrix);

        glm::mat4 output(1);
        output[0][0] = matrix[0];
        output[0][1] = matrix[1];
        output[0][2] = matrix[2];
        output[0][3] = matrix[3];
        output[1][0] = matrix[4];
        output[1][1] = matrix[5];
        output[1][2] = matrix[6];
        output[1][3] = matrix[7];
        output[2][0] = matrix[8];
        output[2][1] = matrix[9];
        output[2][2] = matrix[10];
        output[2][3] = matrix[11];
        output[3][0] = matrix[12];
        output[3][1] = matrix[13];
        output[3][2] = matrix[14];
        output[3][3] = matrix[15];
        return output;
    }

    void ARCore::ReadTextureCalibration(ArCamera* camera) {
#if SCANNER_MODERN
        if(texture_calibration_logged_)return;
        texture_calibration_valid_=false;
        ArCameraIntrinsics* intrinsics=nullptr;
        ArCameraIntrinsics_create(ar_session_,&intrinsics);
        if(!intrinsics)return;
        ArCamera_getTextureIntrinsics(ar_session_,camera,intrinsics);
        ArCameraIntrinsics_getFocalLength(ar_session_,intrinsics,&texture_focal_[0],&texture_focal_[1]);
        ArCameraIntrinsics_getPrincipalPoint(ar_session_,intrinsics,&texture_center_[0],&texture_center_[1]);
        ArCameraIntrinsics_getImageDimensions(ar_session_,intrinsics,&texture_dimensions_[0],&texture_dimensions_[1]);
        ArCameraIntrinsics_destroy(intrinsics);
        const float identity[7]={0,0,0,1,0,0,0};ArPose* pose=nullptr;
        ArPose_create(ar_session_,identity,&pose);if(!pose)return;
        ArCamera_getPose(ar_session_,camera,pose);texture_pose_world_=GetMatrix(pose);ArPose_destroy(pose);
        texture_calibration_valid_=geometry::RigidPose(texture_pose_world_)&&texture_dimensions_[0]>0&&texture_dimensions_[1]>0;
        for(int i=0;i<2;++i)texture_calibration_valid_&=std::isfinite(texture_focal_[i])&&texture_focal_[i]>0&&std::isfinite(texture_center_[i]);
#else
        (void)camera;
#endif
    }

    void ARCore::LogDepthCalibration(int width,int height) {
#if SCANNER_MODERN
        if(!texture_calibration_valid_||texture_calibration_logged_)return;
        const double fx=double(texture_focal_[0])*width/texture_dimensions_[0],fy=double(texture_focal_[1])*height/texture_dimensions_[1];
        const double cx=double(texture_center_[0])*width/texture_dimensions_[0],cy=double(texture_center_[1])*height/texture_dimensions_[1];
        const auto modelFromTexture=glm::inverse(glm::dmat4(zero_transform_))*glm::dmat4(texture_pose_world_);
        double maximum=0;
        for(int iy=1;iy<=3;++iy)for(int ix=1;ix<=3;++ix) {
            int x=ix*width/4,y=iy*height/4;
            auto direct=modelFromTexture*glm::dvec4(2*(x-cx)/fx,2*(cy-y)/fy,-2,1);
            auto current=depth_projection_.Point(camera.Transform(x,y,width,height),2);
            maximum=std::max(maximum,glm::length(glm::dvec3(direct)-glm::dvec3(current)));
        }
        char message[384];
        std::snprintf(message,sizeof(message),"DEPTH_CALIB size=%dx%d texture=%dx%d fx=%.4f fy=%.4f cx=%.4f cy=%.4f display_ray_difference_at_2m=%.6f",width,height,texture_dimensions_[0],texture_dimensions_[1],fx,fy,cx,cy,maximum);
        capture_diagnostics_=message;LOGI("%s",message);
        texture_calibration_logged_=true;
#else
        (void)width;(void)height;
#endif
    }

    bool ARCore::UpdateReferenceFrame(const glm::mat4& rawView) {
#if SCANNER_MODERN
        reference_valid_ = false;
        if (!ar_session_ || !geometry::RigidPose(rawView)) return false;
        const glm::vec3 cameraPosition = glm::inverse(rawView)[3];
        auto readAnchor = [&](ArAnchor* anchor, glm::mat4& matrix) {
            ArTrackingState state = AR_TRACKING_STATE_STOPPED;
            ArAnchor_getTrackingState(ar_session_, anchor, &state);
            if (state != AR_TRACKING_STATE_TRACKING) return false;
            const float identity[7] = {0,0,0,1,0,0,0};
            ArPose* pose = nullptr;
            ArPose_create(ar_session_, identity, &pose);
            if (!pose) return false;
            ArAnchor_getPose(ar_session_, anchor, pose);
            matrix = GetMatrix(pose);
            ArPose_destroy(pose);
            return geometry::RigidPose(matrix);
        };
        auto createAnchor = [&](ArAnchor*& anchor, glm::mat4& matrix, bool allowPending) {
            // A session anchor supplies a world reference without attaching the
            // whole scan to a depth-hit surface whose fitted pose can change.
            const float data[7] = {0,0,0,1,cameraPosition.x,cameraPosition.y,cameraPosition.z};
            ArPose* pose = nullptr;
            ArPose_create(ar_session_, data, &pose);
            if (!pose) return false;
            matrix=GetMatrix(pose);
            const ArStatus status = ArSession_acquireNewAnchor(ar_session_, pose, &anchor);
            ArPose_destroy(pose);
            if (status == AR_SUCCESS && anchor) {
                glm::mat4 tracked;
                if (readAnchor(anchor,tracked)) { matrix=tracked; return true; }
                if (allowPending && geometry::RigidPose(matrix)) return true;
            }
            if (anchor) { ArAnchor_detach(ar_session_,anchor); ArAnchor_release(anchor); anchor=nullptr; }
            return false;
        };
        if (!ar_zero_.second) {
            ArAnchor* anchor = nullptr; glm::mat4 matrix;
            if (!createAnchor(anchor,matrix,true)) return false;
            ar_zero_.second=anchor; ar_zero_.first.matrix=matrix;
            reference_history_[0].anchor=anchor;reference_history_[0].model_pose=matrix;
            reference_count_=1;active_reference_=0;
            zero_transform_=glm::mat4(1);
        }
        glm::mat4 current;
        if (!readAnchor(ar_zero_.second,current)) return false;
        glm::mat4 correction=current*glm::inverse(ar_zero_.first.matrix);
        if (!geometry::RigidPose(correction)) return false;
        // Select in the scan's fixed model coordinates. On a revisit, retain
        // the original local reference rather than creating a new one from the
        // latest region's correction. A missing local reference blocks capture.
        const glm::vec3 modelCamera=glm::vec3(glm::inverse(correction)*glm::vec4(cameraPosition,1));
        float distance=glm::distance(modelCamera,glm::vec3(reference_history_[active_reference_].model_pose[3]));
        size_t nearest=active_reference_;float nearestDistance=distance;
        for(size_t i=0;i<reference_count_;++i) {
            const float d=glm::distance(modelCamera,glm::vec3(reference_history_[i].model_pose[3]));
            if(d<nearestDistance){nearest=i;nearestDistance=d;}
        }
        if(nearest!=active_reference_&&nearestDistance+.75f<distance) {
            glm::mat4 nearbyWorld;
            if(!readAnchor(reference_history_[nearest].anchor,nearbyWorld))return false;
            const glm::mat4 nearbyCorrection=nearbyWorld*glm::inverse(reference_history_[nearest].model_pose);
            if(!geometry::RigidPose(nearbyCorrection))return false;
            active_reference_=nearest;ar_zero_.second=reference_history_[nearest].anchor;
            ar_zero_.first.matrix=reference_history_[nearest].model_pose;
            correction=nearbyCorrection;current=nearbyWorld;
            distance=glm::distance(cameraPosition,glm::vec3(current[3]));
            ++reference_revisits_;
            LOGI("SCAN_REFERENCE reused=%zu count=%zu",active_reference_,reference_count_);
        }
        zero_transform_=correction;
        const int64_t now=NowMs();
        if (distance>4 && reference_count_<reference_history_.size() &&
            (!last_reference_attempt_ms_ || now-last_reference_attempt_ms_>=1000)) {
            last_reference_attempt_ms_=now;
            ArAnchor* replacement=nullptr; glm::mat4 replacementWorld;
            if (createAnchor(replacement,replacementWorld,false)) {
                const glm::mat4 replacementReference=glm::inverse(correction)*replacementWorld;
                if (geometry::RigidPose(replacementReference)) {
                    active_reference_=reference_count_++;
                    reference_history_[active_reference_].anchor=replacement;
                    reference_history_[active_reference_].model_pose=replacementReference;
                    ar_zero_.second=replacement; ar_zero_.first.matrix=replacementReference;
                    reference_valid_=true;
                    LOGI("SCAN_REFERENCE added=%zu distance=%.3f count=%zu; prior regions retained",active_reference_,distance,reference_count_);
                    return true;
                }
                ArAnchor_detach(ar_session_,replacement);ArAnchor_release(replacement);
            }
        }
        // Do not accumulate beyond the documented anchor-distance range if a
        // nearby tracked reference cannot be established.
        reference_valid_=distance<=8;
        return reference_valid_;
#else
        (void)rawView;return true;
#endif
    }

    glm::mat4 ARCore::GetZeroTransform() {
#if SCANNER_MODERN
        return zero_transform_;
#else
#ifndef ARCORE_BACKPORT
        if (ar_zero_.second) {
            ArTrackingState state = AR_TRACKING_STATE_STOPPED;
            ArAnchor_getTrackingState(ar_session_, ar_zero_.second, &state);
            // Never replace the scan's origin with an untracked anchor pose.
            // Keep the last tracked correction while the same session recovers.
            if (state != AR_TRACKING_STATE_TRACKING) return zero_transform_;
            float data[7] = {0, 0, 0, 1, 0, 0, 0};

            ArPose *ar_pose;
            ArPose_create(ar_session_, data, &ar_pose);
            ArAnchor_getPose(ar_session_, ar_zero_.second, ar_pose);
            glm::mat4 matrix = GetMatrix(ar_pose);
            ArPose_destroy(ar_pose);

            const glm::mat4 transform = matrix * glm::inverse(ar_zero_.first.matrix);
            if (geometry::RigidPose(transform)) zero_transform_ = transform;
            return zero_transform_;
        }
#endif
        return glm::mat4(1);
#endif
    }

    glm::vec4 ARCore::ToPoint(glm::dmat4& screen2world, double& len,
                      int32_t& depthWidth, int32_t& depthHeight, int& x, int& y, double& depth) {

        //convert sensor coordinates to screen coordinates
        glm::dvec2 T = camera.Transform(x, y, depthWidth, depthHeight);

#if SCANNER_MODERN
        (void)screen2world;(void)len;
        return depth_projection_.Point(T,depth);
#else
        return geometry::DepthPoint(screen2world, T, len, depth);
#endif
    }

    bool ARCore::UpdateAnchor() {
        float data[7] = {0, 0, 0, 1, 0, 0, 0};
        ArPose *ar_pose;
        ArPose_create(ar_session_, data, &ar_pose);

        bool valid = false;
        ArAnchor* ar_anchor_ = 0;
        int size = 0;
        ArHitResult* hit = 0;
        ArHitResultList* hits = 0;
        ArHitResult_create(ar_session_, &hit);
        ArHitResultList_create(ar_session_, &hits);
        for (float x = 0.25f; x <= 0.75f; x += 0.5f) {
            for (float y = 0.25f; y <= 0.75f; y += 0.5f) {
                ArFrame_hitTest(ar_session_, ar_frame_, viewportWidth * x, viewportHeight * y, hits);
                ArHitResultList_getSize(ar_session_, hits, &size);
                for (int i = 0; i < size; i++) {
                    ArTrackable* trackable = 0;
                    ArHitResultList_getItem(ar_session_, hits, i, hit);
                    ArHitResult_acquireTrackable(ar_session_, hit, &trackable);
                    ArTrackableType type = AR_TRACKABLE_NOT_VALID;
                    ArTrackable_getType(ar_session_, trackable, &type);
                    if (type == AR_TRACKABLE_POINT || type == AR_TRACKABLE_DEPTH_POINT) {

                        // AUTOMATIC depth also returns ArDepthPoint hits. The
                        // generic hit pose/anchor APIs work for both types.
                        ArHitResult_getHitPose(ar_session_, hit, ar_pose);
                        ArPose_getPoseRaw(ar_session_, ar_pose, data);
                        glm::vec3 position(data[4], data[5], data[6]);

                        id3d pos;
                        pos.matrix = GetMatrix(ar_pose);
                        if (!geometry::GridPosition(position, ANCHOR_DENSITY_BASE) ||
                            !geometry::RigidPose(pos.matrix)) {
                            ArTrackable_release(trackable);
                            continue;
                        }
                        float density = ANCHOR_DENSITY_BASE;
                        for (pos.layer = 0; pos.layer < ANCHOR_LAYERS; pos.layer++) {
                            pos.x = static_cast<int>(position.x / density);
                            pos.y = static_cast<int>(position.y / density);
                            pos.z = static_cast<int>(position.z / density);
                            auto existing = ar_anchor_list.find(pos);
                            if (existing != ar_anchor_list.end()) {
                                ArTrackingState state = AR_TRACKING_STATE_STOPPED;
                                ArAnchor_getTrackingState(ar_session_, existing->second, &state);
                                if (state != AR_TRACKING_STATE_TRACKING) {
                                    ArAnchor_detach(ar_session_, existing->second);
                                    ArAnchor_release(existing->second);
                                    ar_anchor_list.erase(existing);
                                } else valid = true;
                            }
                            if (ar_anchor_list.find(pos) == ar_anchor_list.end()) {
                                ArStatus ret = ArHitResult_acquireNewAnchor(ar_session_, hit, &ar_anchor_);
                                if (ret == AR_SUCCESS) {
                                    valid = true;
                                    if (ar_zero_.second == 0) {
                                        ar_zero_.first = pos;
                                        ar_zero_.second = ar_anchor_;
                                        break;
                                    }

                                    ar_anchor_list[pos] = ar_anchor_;
                                    while (true) {
                                        int count = 0;
                                        id3d far = pos;
                                        for (auto& anchor : ar_anchor_list) {
                                            if (anchor.first.layer == pos.layer) {
                                                if (geometry::GridDistanceSquared(anchor.first, pos) > geometry::GridDistanceSquared(far, pos)) {
                                                    far = anchor.first;
                                                }
                                                count++;
                                            }
                                        }
                                        if (count > ANCHOR_CACHE) {
                                            ArAnchor_detach(ar_session_, ar_anchor_list[far]);
                                            ArAnchor_release(ar_anchor_list[far]);
                                            ar_anchor_list.erase(far);
                                        } else {
                                            break;
                                        }
                                    }
                                }
                            }
                            density *= ANCHOR_DENSITY_SCALE;
                        }
                    }
                    ArTrackable_release(trackable);
                }
            }
        }
        ArHitResultList_destroy(hits);
        ArHitResult_destroy(hit);
        ArPose_destroy(ar_pose);

        if (!valid && !GetActiveAnchors().empty()) {
            valid = true;
        }
        return valid;
    }

    void ARCore::UpdateFace(glm::mat4 matrix) {
#ifndef ARCORE_BACKPORT
        face_mesh.vertices.clear();
        face_mesh.normals.clear();
        face_mesh.uv.clear();
        face_mesh.indices.clear();
        points.clear();
        if (!ar_session_ || !ar_frame_ || !frame_valid_) return;
        int32_t size = 0;
        ArTrackableList* faces = 0;
        ArTrackableList_create(ar_session_, &faces);
        ArSession_getAllTrackables(ar_session_, AR_TRACKABLE_FACE, faces);
        ArTrackableList_getSize(ar_session_, faces, &size);
        for (int32_t i = 0; i < size; i++) {
            int32_t count = 0;
            const float* vertices = 0;
            const float* normals = 0;
            ArTrackable* face = 0;
            ArTrackableList_acquireItem(ar_session_, faces, i, &face);
            ArAugmentedFace_getMeshVertices(ar_session_, ArAsFace(face), &vertices, &count);
            ArAugmentedFace_getMeshNormals(ar_session_, ArAsFace(face), &normals, &count);

            float data[7] = {0, 0, 0, 1, 0, 0, 0};
            ArPose *ar_pose;
            ArPose_create(ar_session_, data, &ar_pose);
            ArAugmentedFace_getCenterPose(ar_session_, ArAsFace(face), ar_pose);
            ArPose_getPoseRaw(ar_session_, ar_pose, data);

            GLCamera pose;
            pose.position = glm::vec3(data[4], data[5], data[6]);
            pose.rotation = glm::quat(data[3], data[0], data[1], data[2]);
            pose.scale = glm::vec3(1);
            glm::mat4 transform = pose.GetTransformation();
            for (int j = 0; j < count; j++) {
                glm::vec4 point = glm::vec4(vertices[j * 3 + 0],
                                            vertices[j * 3 + 1],
                                            vertices[j * 3 + 2],
                                            1.0f);
                point = transform * point;
                point /= fabs(point.w);
                point.w = 1.0f;
                face_mesh.vertices.push_back(point);
                face_mesh.normals.push_back(glm::vec3(normals[j * 3 + 0],
                                                      normals[j * 3 + 1],
                                                      normals[j * 3 + 2]));

                point = matrix * point;
                point /= fabs(point.w);
                face_mesh.uv.push_back(0.5f * glm::vec2(point.x, point.y) + 0.5f);
            }

            const uint16_t* indices = 0;
            int32_t triangles = 0;
            ArAugmentedFace_getMeshTriangleIndices(ar_session_, ArAsFace(face), &indices, &triangles);
            for (int j = 0; j < triangles; j++) {
                if (face_not_all) {
                    bool ok = true;
                    for (int l = j * 3 + 0; l < j * 3 + 3; l++)
                    {
                        if ((indices[l] == 13) || (indices[l] == 14))
                            ok = false;
                        if ((indices[l] == 78) || (indices[l] == 95))
                            ok = false;
                        if ((indices[l] >= 80) && (indices[l] <= 82))
                            ok = false;
                        if ((indices[l] == 87) || (indices[l] == 88))
                            ok = false;
                        if ((indices[l] == 178) || (indices[l] == 191))
                            ok = false;
                        if ((indices[l] == 308) || (indices[l] == 324))
                            ok = false;
                        if ((indices[l] >= 310) && (indices[l] <= 312))
                            ok = false;
                        if ((indices[l] == 317) || (indices[l] == 318))
                            ok = false;
                        if ((indices[l] == 402) || (indices[l] == 415))
                            ok = false;
                    }
                    if (!ok)
                        continue;
                }
                face_mesh.indices.push_back(indices[j * 3 + 0]);
                face_mesh.indices.push_back(indices[j * 3 + 1]);
                face_mesh.indices.push_back(indices[j * 3 + 2]);
                points.push_back(glm::vec4(face_mesh.vertices[indices[j * 3 + 0]], 1.0f));
                points.push_back(glm::vec4(face_mesh.vertices[indices[j * 3 + 1]], 1.0f));
                points.push_back(glm::vec4(face_mesh.vertices[indices[j * 3 + 1]], 1.0f));
                points.push_back(glm::vec4(face_mesh.vertices[indices[j * 3 + 2]], 1.0f));
                points.push_back(glm::vec4(face_mesh.vertices[indices[j * 3 + 2]], 1.0f));
                points.push_back(glm::vec4(face_mesh.vertices[indices[j * 3 + 0]], 1.0f));
            }

            ArPose_destroy(ar_pose);
            ArTrackable_release(face);
        }
        ArTrackableList_destroy(faces);
#endif
    }

    void ARCore::UpdateFeaturePoints() {
        depth_test_frame_.reset();
        points.clear();
#if SCANNER_MODERN
        if (!frame_valid_ || (!face_mode_ && !reference_valid_)) return;
#else
        if (!frame_valid_ || !UpdateAnchor()) return;
#endif

        if (!useDepth) {
            ArPointCloud *ar_point_cloud = nullptr;
            ArStatus point_cloud_status = ArFrame_acquirePointCloud(ar_session_, ar_frame_, &ar_point_cloud);
            int32_t number_of_points = 0;
            if (point_cloud_status == AR_SUCCESS) {
                ArPointCloud_getNumberOfPoints(ar_session_, ar_point_cloud, &number_of_points);
                const float *point_cloud_data = nullptr;
                ArPointCloud_getData(ar_session_, ar_point_cloud, &point_cloud_data);

                const glm::mat4 zero = glm::inverse(GetZeroTransform());
                for (int i = 0; point_cloud_data && i < number_of_points; ++i) {
                    const float* data = point_cloud_data + size_t(i) * 4;
                    glm::vec4 p = zero * glm::vec4(data[0], data[1], data[2], 1);
                    p.w = data[3];
                    if (geometry::Point(p)) points.push_back(p);
                }
                ArPointCloud_release(ar_point_cloud);
            }
        }

        if (useDepth) {
            points.clear();
            camera.InitARCore(ar_session_, ar_frame_);
            // Tracking and depth availability are independent of the number of
            // sparse feature points (e.g. while looking at a plain wall).
            {

                ArImage* image = 0;
                ArStatus result = AR_SUCCESS;
                if (useDepthRaw) {
                    result = ArFrame_acquireRawDepthImage16Bits(ar_session_, ar_frame_, &image);
                } else {
                    result = ArFrame_acquireDepthImage16Bits(ar_session_, ar_frame_, &image);
                }
                ScopedImage depthImage(image, ArImage_release);
                if (result != AR_SUCCESS) ReportStatus("depth image", result, last_depth_error_);
                if (result == AR_SUCCESS) {

                    ArImage* confidenceImage = 0;
                    geometry::Plane confidencePlane;
                    bool hasConfidence = false;
                    if (useDepthRaw) {
                        result = ArFrame_acquireRawDepthConfidenceImage(ar_session_, ar_frame_, &confidenceImage);
                        if (result != AR_SUCCESS) ReportStatus("depth confidence", result, last_depth_error_);
                        if (result == AR_SUCCESS) {
                            confidencePlane = ReadPlane(ar_session_, confidenceImage);
                            hasConfidence = confidencePlane.Valid(1);
                        }
                    }
                    ScopedImage confidenceOwner(confidenceImage, ArImage_release);

                    //get depth data
                    const geometry::Plane depthPlane = ReadPlane(ar_session_, image);
                    int32_t depthWidth = depthPlane.width, depthHeight = depthPlane.height;
                    int64_t timestamp = 0;
                    ArImage_getTimestamp(ar_session_, image, &timestamp);
                    if (!depthPlane.Valid(2) || (confidenceImage &&
                        (!hasConfidence || !confidencePlane.SameSize(depthPlane)))) {
                        LOGE("ARCore: rejected invalid depth/confidence plane");
                        return;
                    }
                    // Raw depth is only usable with its matching confidence map.
                    // NOT_YET_AVAILABLE is a normal missing frame, not bad geometry.
                    if (useDepthRaw && !hasConfidence) {
                        return;
                    }
                    last_depth_error_ = AR_SUCCESS;

                    ArImage* image2 = 0;
                    geometry::Plane secondaryPlane;
                    bool hasSecondary = false;
                    if (useDepthRaw && useSmoothedDepth && !has_depth_sensor) {
                        result = ArFrame_acquireDepthImage16Bits(ar_session_, ar_frame_, &image2);
                        if (result == AR_SUCCESS) {
                            secondaryPlane = ReadPlane(ar_session_, image2);
                            hasSecondary = secondaryPlane.Valid(2) && secondaryPlane.SameSize(depthPlane);
                        }
                    }
                    ScopedImage secondaryOwner(image2, ArImage_release);

                    //convert depthmap to pointcloud
                    int minConfidence = has_depth_sensor ? 32 : 128;
                    float maxErrorFilter = resolution * 3.0f;
                    float maxErrorHoles = resolution * 3.0f;
                    float maxErrorWalls = resolution * 3.0f;
                    double len = 100 - 0.001f; //far - near
                    std::vector<glm::vec3> refused;
                    std::map<std::pair<int, int>, double> edges2d;
                    glm::vec3 cam = glm::inverse(view_mat)[3];
                    glm::dmat4 screen2world = glm::inverse(glm::dmat4(projection_mat) * glm::dmat4(view_mat));
#if SCANNER_MODERN
                    if(!depth_projection_.Configure(projection_mat,view_mat)) {
                        LOGE("ARCore: invalid calibrated depth projection");return;
                    }
                    LogDepthCalibration(depthWidth,depthHeight);
#endif
                    // Software raw depth can be reprojected for a new camera
                    // pose with the same timestamp. That is usable data, not a
                    // tracking failure or reason to disable depth on Pixels.
                    if (!has_depth_sensor || (lastDepthTimestamp != timestamp)) {

#if SCANNER_MODERN
                        if(depth_test_enabled_&&useDepthRaw&&hasConfidence) {
                            depth_test_frame_=depth_test::Capture(depthPlane,confidencePlane,
                                nullptr,minConfidence,maxErrorFilter,depth_test_generation_);
                            if(depth_test_frame_) {
                                depth_test_frame_->depthTimestamp=timestamp;
                                ArFrame_getTimestamp(ar_session_,ar_frame_,&depth_test_frame_->cameraTimestamp);
                            }
                        }
#endif

                        float maxY = INT_MIN;
                        std::map<int, float> distances;
                        std::map<std::pair<int, int>, float> distancesLocal;
                        float s = 1;
                        int m = has_depth_sensor ? glm::max(0, (depthWidth - depthHeight) / 2) : 0;
                        if (depthWidth > 240) s = depthWidth / 240.0f;
                        for (float fy = 0; fy < depthHeight; fy += s) {
                            for (float fx = m; fx < depthWidth - m; fx += s) {
                                int x = (int)fx;
                                int y = (int)fy;
#if !SCANNER_MODERN
                                if ((x < 4) && (y == 0))
                                    continue;
#endif

                                //check point validity
#if SCANNER_MODERN
                                double depth=0;float sampleConfidence=0;bool measured=false;
                                const uint16_t estimate=hasSecondary?secondaryPlane.Depth(x,y):!useDepthRaw?depthPlane.Depth(x,y):0;
                                if(!geometry::SelectDepth(hasConfidence?depthPlane.Depth(x,y):0,
                                                         hasConfidence?confidencePlane.Byte(x,y):0,
                                                         estimate,minConfidence,depth,sampleConfidence,measured))continue;
#else
                                double depth = depthPlane.Depth(x, y) * 0.001f;
                                if (hasConfidence) {
                                    if (confidencePlane.Byte(x, y) <= minConfidence) {
                                        if (hasSecondary) {

                                            //get nearest point with high confidence in 4 directions
                                            bool left = false, right = false, up = false, down = false;
                                            glm::vec3 c, l, r, u, d;
                                            for (int tx = x; tx >= 0; tx--) {
                                                if (confidencePlane.Byte(tx, y) > minConfidence && depthPlane.Depth(tx, y) > 0) {
                                                    l = glm::vec3(tx, y, depthPlane.Depth(tx, y) * 0.001f);
                                                    left = true;
                                                    break;
                                                }
                                            }
                                            for (int tx = x; tx < depthWidth; tx++) {
                                                if (confidencePlane.Byte(tx, y) > minConfidence && depthPlane.Depth(tx, y) > 0) {
                                                    r = glm::vec3(tx, y, depthPlane.Depth(tx, y) * 0.001f);
                                                    right = true;
                                                    break;
                                                }
                                            }
                                            for (int ty = y; ty >= 0; ty--) {
                                                if (confidencePlane.Byte(x, ty) > minConfidence && depthPlane.Depth(x, ty) > 0) {
                                                    u = glm::vec3(x, ty, depthPlane.Depth(x, ty) * 0.001f);
                                                    up = true;
                                                    break;
                                                }
                                            }
                                            for (int ty = y; ty < depthHeight; ty++) {
                                                if (confidencePlane.Byte(x, ty) > minConfidence && depthPlane.Depth(x, ty) > 0) {
                                                    d = glm::vec3(x, ty, depthPlane.Depth(x, ty) * 0.001f);
                                                    down = true;
                                                    break;
                                                }
                                            }
                                            c = glm::vec3(x, y, secondaryPlane.Depth(x, y) * 0.001f);
                                            if (c.z <= 0.05f) continue;

                                            //"closed" holes filling
                                            bool horizontal = false, vertical = false;
                                            double interpolated = 0;
                                            if (left && right) {
                                                if (geometry::InterpolatedDepth(c.x, l.x, r.x, l.z, r.z, interpolated) &&
                                                    fabs(interpolated - c.z) < maxErrorHoles) {
                                                    horizontal = true;
                                                }
                                            }
                                            if (up && down) {
                                                if (geometry::InterpolatedDepth(c.y, u.y, d.y, u.z, d.z, interpolated) &&
                                                    fabs(interpolated - c.z) < maxErrorHoles) {
                                                    vertical = true;
                                                }
                                            }
                                            if (horizontal || vertical) {
                                                depth = c.z;
                                            } else {

                                                //store the refused point to process later
                                                depth = c.z - offset;
                                                const glm::vec4 p = ToPoint(screen2world, len, depthWidth, depthHeight, x, y, depth);
                                                if (!geometry::Point(p) || !geometry::GridPosition(glm::vec3(p), 0.1f)) continue;
                                                refused.emplace_back(p);

                                                //mark edge points for wall validation
                                                if (up && !down) edges2d[std::pair<int, int>(u.x, u.y)] = u.z;
                                                if (!up && down) edges2d[std::pair<int, int>(d.x, d.y)] = d.z;
                                                if (left && !right) edges2d[std::pair<int, int>(l.x, l.y)] = l.z;
                                                if (!left && right) edges2d[std::pair<int, int>(r.x, r.y)] = r.z;
                                                continue;
                                            }
                                        } else {
                                            continue;
                                        }
                                    }
                                }

#endif
#if !SCANNER_MODERN
                                if (hasSecondary) {
                                    double filtered = secondaryPlane.Depth(x, y) * 0.001f;
                                    if (filtered > 0.05 && fabs(depth - filtered) < maxErrorFilter) {
                                        depth = filtered;
                                    }
                                }
#endif

                                //add point into output
                                if (depth > 0.05) {
                                    const float measuredDepth=float(depth);
                                    depth -= offset;
                                    glm::vec4 p = ToPoint(screen2world, len, depthWidth, depthHeight, x, y, depth);
                                    if (!geometry::Point(p) || !geometry::GridPosition(glm::vec3(p), 0.1f)) continue;
#if SCANNER_MODERN
                                    p.w=sampleConfidence;
#endif
                                    if (maxY < p.y) maxY = p.y;
                                    points.emplace_back(p);
#if SCANNER_MODERN
                                    depth_source_estimated_+=!measured;
                                    if(depth_test_frame_&&measured) {
                                        depth_test::AddRayLink(depth_test_frame_,uint32_t(y*depthWidth+x),uint32_t(points.size()-1),
                                            p,measuredDepth,std::max(.05f,offset),
                                            depth_projection_.Ray(camera.Transform(x,y,depthWidth,depthHeight)));
                                    }
#endif

#if !SCANNER_MODERN
                                    if (!has_depth_sensor && hasSecondary) {
                                        int dir = (int)glm::degrees(atan2(p.y - cam.y, p.x - cam.x));
                                        float dst = glm::distance(cam, glm::vec3(p));
                                        if (distances.find(dir) == distances.end()) {
                                            distances[dir] = dst;
                                        } else if (distances[dir] > dst) {
                                            distances[dir] = dst;
                                        }
                                        std::pair<int, int> key;
                                        key.first = dir;
                                        key.second = (int)(p.y * 10);
                                        if (distancesLocal.find(key) == distancesLocal.end()) {
                                            distancesLocal[key] = dst;
                                        } else if (distancesLocal[key] > dst) {
                                            distancesLocal[key] = dst;
                                        }
                                    }
#endif
                                }
                            }
                        }


                        //convert edge points into 3D space
                        std::vector<glm::vec3> edges3d;
                        for (std::pair<const std::pair<int, int>, double>& e : edges2d) {
                            int x = e.first.first;
                            int y = e.first.second;
                            double depth = e.second - offset;
                            const glm::vec4 p = ToPoint(screen2world, len, depthWidth, depthHeight, x, y, depth);
                            if (geometry::Point(p)) edges3d.emplace_back(p);
                        }

                        //add wall points
                        for (glm::vec3& r : refused) {
                            bool ok = false;
                            glm::vec3 v(INT_MIN);
                            for (glm::vec3& p : edges3d) {
                                if ((r.y > p.y) && (r.y < maxY)) {
                                    float x = fabs(p.x - r.x);
                                    float z = fabs(p.z - r.z);
                                    if (x * x + z * z < maxErrorWalls * maxErrorWalls) {
                                        if (v.y < p.y) {
                                            ok = true;
                                            v = p;
                                        }
                                    }
                                }
                            }
                            if (ok) {
                                int dir = (int)glm::degrees(atan2(v.y - cam.y, v.x - cam.x));
                                if (distances.find(dir) != distances.end()) {
                                    float dst = glm::distance(cam, v);
                                    if ((distances[dir] - maxErrorWalls < dst)) {
                                        std::pair<int, int> key;
                                        key.first = dir;
                                        key.second = (int)(r.y * 10);
                                        if (distancesLocal.find(key) == distancesLocal.end()) {
                                            points.emplace_back(r, 1.0f);
                                        } else if (distancesLocal[key] + maxErrorWalls > dst) {
                                            points.emplace_back(r, 1.0f);
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Do not consume an empty/rejected depth frame: a valid
                    // reprojection with the same raw timestamp can follow it.
                    if (!points.empty()) {
#if SCANNER_MODERN
                        ++depth_source_frames_;depth_source_points_+=points.size();
                        depth_source_repeated_+=lastDepthTimestamp==timestamp;
                        if(depth_source_frames_==30) {
                            LOGI("DEPTH_SOURCE frames=30 size=%dx%d raw=%d weighted_confidence=%d secondary=%d offset=%.4f mean_points=%.1f repeated=%llu estimated=%llu world=session_anchor",
                                 depthWidth,depthHeight,int(useDepthRaw),int(hasConfidence),int(hasSecondary),offset,depth_source_points_/30.,
                                 static_cast<unsigned long long>(depth_source_repeated_),static_cast<unsigned long long>(depth_source_estimated_));
                            depth_source_frames_=depth_source_points_=depth_source_repeated_=depth_source_estimated_=0;
                        }
#endif
                        lastDepthTimestamp = timestamp;
                    }
                }
            }
        }
        if(depth_test_frame_) depth_test_frame_->pointCount=points.size();
        if (!points.empty()) has_coordinate_system_ = true;
    }
}
