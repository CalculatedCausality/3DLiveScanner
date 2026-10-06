#include <sstream>
#include <algorithm>
#include <cerrno>
#include <unistd.h>

#include <arcore/service.h>
#include <depth/record.h>
#include <thread/capture_policy.h>
#include <sys/stat.h>
#include <thread/frame_writer.h>
#include <arcore/geometry_validation.h>
#include <tango/geometry_validation.h>
#include <thread/reconstr.h>

namespace oc {

    cv::Ptr< cv::ORB > detector = cv::ORB::create();
    cv::Ptr< cv::ORB > extractor = cv::ORB::create();
    cv::BFMatcher matcher( cv::NORM_HAMMING2, true );

    Reconstruction* g_reconstruction = nullptr;

    void* ProcessDummy(void*) {
        usleep(100000);
        g_reconstruction->BinderUnlock();
        return 0;
    }

    void* ProcessPoseCorrection(void*) {

        if (!g_reconstruction->request_image || !g_reconstruction->request_image->IsValid() ||
            !g_reconstruction->rendered_image || !g_reconstruction->rendered_image->IsValid() ||
            !g_reconstruction->rendered_depth || !g_reconstruction->rendered_depth->IsValid()) {
            g_reconstruction->Start(Reconstruction::RECONSTRUCTION);
            return 0;
        }

        double best_accuracy = INT_MAX;
        glm::mat4 orig_mat = g_reconstruction->frame_viewmat;
        glm::mat4 best_mat = g_reconstruction->frame_viewmat;
        Reconstruction::CVDescription camera = g_reconstruction->DetectFeatures(g_reconstruction->request_image);
        Reconstruction::CVDescription render = g_reconstruction->DetectFeatures(g_reconstruction->rendered_image);
        g_reconstruction->request_newprojection = false;

        //validate continuity
        if (camera.keypoints.empty() || render.keypoints.empty()) {
            g_reconstruction->Start(Reconstruction::RECONSTRUCTION);
            return 0;
        }

        //convert points into 3D
        std::vector<glm::vec3> points3d;
        float w = g_reconstruction->request_image->GetWidth();
        float h = g_reconstruction->request_image->GetHeight();
        g_reconstruction->render_mutex_.lock();
        glm::mat4 projection = g_reconstruction->scene.renderer->camera.projection;
        if (g_reconstruction->scene.static_meshes_.empty()) {
            glm::mat4 screen2world = glm::inverse(g_reconstruction->frame_pose[SCREEN_CAMERA]);
            for (cv::KeyPoint& p : render.keypoints) {
                glm::vec4 depth = g_reconstruction->rendered_depth->GetColorRGBA(p.pt.x, p.pt.y);
                float x = 2.0f * p.pt.x / w - 1.0f;
                float y = 2.0f * p.pt.y / h - 1.0f;
                float z = (depth.r + depth.g + depth.b) / 255.0f * 2.0f;
                glm::vec4 v = screen2world * glm::vec4(x, y, z, 1.0f);
                points3d.emplace_back(v / fabs(v.w));
            }
        } else {
            std::vector<glm::vec2> points2d;
            for (cv::KeyPoint& p : render.keypoints) {
                points2d.emplace_back(p.pt.x, h - p.pt.y);
            }
            points3d = g_reconstruction->selector.Transform(g_reconstruction->scene.static_meshes_,
                                                            g_reconstruction->frame_pose[SCREEN_CAMERA],
                                                            points2d);
        }
        g_reconstruction->render_mutex_.unlock();

        std::vector<cv::DMatch> allMatches, matches;
        matcher.match(camera.descriptors, render.descriptors, allMatches);
        for (cv::DMatch& m : allMatches) {
            cv::Point2f p1 = camera.keypoints[m.queryIdx].pt;
            cv::Point2f p2 = render.keypoints[m.trainIdx].pt;
            float dx = p1.x - p2.x;
            float dy = p1.y - p2.y;
            float diff = sqrt(dx * dx + dy * dy);
            if (diff < 10) {
                matches.push_back(m);
            }
        }

        while (true) {
            while (true) {

                //get next matrix
                if (g_reconstruction->request_matrix.empty()) {
                    break;
                }
                glm::mat4 matrix = g_reconstruction->request_matrix[0];
                g_reconstruction->request_matrix.erase(g_reconstruction->request_matrix.begin());

                //reproject CV descriptors
                glm::mat4 m = projection * matrix;
                for (int i = 0; i < points3d.size(); i++) {
                    glm::vec4 v = m * glm::vec4(points3d[i], 1);
                    v /= fabs(v.w);
                    v = 0.5f * v + 0.5f;
                    render.keypoints[i].pt.x = v.x * w;
                    render.keypoints[i].pt.y = v.y * h;
                }

                //determine pose accuracy
                double accuracy = g_reconstruction->GetAccuracy(matches, camera, render);

                //decision on better pose
                if (best_accuracy > accuracy) {
                    best_accuracy = accuracy;
                    best_mat = matrix;
                }
            }

            //apply value
            g_reconstruction->frame_pose = ARCoreService::GetPose(g_reconstruction->frame_calibration, best_mat);
            g_reconstruction->frame_viewmat = best_mat;

            //next steps
            if ((g_reconstruction->request_distance > 0.0005f) && (best_accuracy != INT_MAX)) {
                g_reconstruction->request_distance *= 0.25f;
                g_reconstruction->AddPoses();
            } else {
                break;
            }
        }

        /*
        g_reconstruction->event_mutex_.lock();
        sprintf(g_reconstruction->pose_feedback, "%f", best_accuracy);
        g_reconstruction->event_mutex_.unlock();
        usleep(1000000);
         */


        //start reconstruction
        if (best_accuracy > 3.0f) {
            g_reconstruction->frame_pose = ARCoreService::GetPose(g_reconstruction->frame_calibration, orig_mat);
            g_reconstruction->frame_viewmat = orig_mat;
        }
        g_reconstruction->Start(Reconstruction::RECONSTRUCTION);
        return 0;
    }

    void Reconstruction::ApplyExperimentalDepth() {
#if SCANNER_MODERN
        auto packet=std::move(frame_depth_test);
        if(!depth_test_runtime.Enabled()) return;
        if(!packet) { depth_test_runtime.Fallback("no compatible raw depth frame");return; }
        try {
            packet->worldToCamera=glm::inverse(glm::dmat4(frame_pose[COLOR_CAMERA]));
            packet->minDepth=scan.MinimumDepth();packet->maxDepth=scan.MaximumDepth();
            std::vector<glm::vec4> originals=frame_points;
            struct Rollback {
                std::vector<glm::vec4>& points;
                std::vector<glm::vec4>& originals;
                bool accepted=false;
                Rollback(std::vector<glm::vec4>& p,std::vector<glm::vec4>& o):points(p),originals(o){}
                ~Rollback(){if(!accepted)points.swap(originals);}
            } rollback{frame_points,originals};
            depth_test::Stats stats;
            if(!depth_test_runtime.Apply(*packet,frame_points,stats)) return;
            if(!t3dr_is_running_.load()||packet->generation!=depth_test_runtime.Generation()) return;
            if(stats.changed) {
                int index=texturize.GetLatestIndex(dataset)+1;
                if(!depth_test::WriteRecord(dataset->GetFileName(index,".tpu"),*packet,originals,frame_points)) {
                    depth_test_runtime.Fallback("could not save original-depth backup");return;
                }
            }
            rollback.accepted=true;
        } catch(...) { depth_test_runtime.Fallback("frame preparation or backup failed"); }
#else
        frame_depth_test.reset();
#endif
    }

    void* ProcessReconstruction(void*) {
        const auto workerStarted = std::chrono::steady_clock::now();

        // Validate before touching either Retango's estimates or the SDK volume.
        // No tracking/empty clouds are ordinary skipped frames, not failures.
        if (g_reconstruction->frame_points.empty()) {
            g_reconstruction->BinderUnlock();
            return 0;
        }
        if (!g_reconstruction->frame_image || !g_reconstruction->frame_image->IsValid() ||
            g_reconstruction->frame_pose.size() <= SCREEN_CAMERA ||
            !geometry::RigidPose(g_reconstruction->frame_pose[COLOR_CAMERA]) ||
            !geometry::RigidPose(g_reconstruction->frame_viewmat) ||
            !geometry::Invertible(g_reconstruction->frame_calibration) ||
            !std::isfinite(g_reconstruction->frame_timestamp)) {
            LOGE("Reconstruction: rejected invalid frame geometry/image");
            g_reconstruction->BinderUnlock();
            return 0;
        }
        for (float coefficient : g_reconstruction->frame_distortion) {
            if (!std::isfinite(coefficient)) {
                LOGE("Reconstruction: rejected invalid camera distortion");
                g_reconstruction->BinderUnlock();
                return 0;
            }
        }

        //process camera calibration
        if (!g_reconstruction->RecoverScan()) {
            g_reconstruction->BinderUnlock();
            return 0;
        }
        if (!g_reconstruction->t3dr_is_running_.load()) {
            g_reconstruction->BinderUnlock();
            return 0;
        }
        int scale = g_reconstruction->frame_image->GetWidth() > 1000 ? 3 : 1;
        const auto calibrationStarted = std::chrono::steady_clock::now();
        Tango3DR_CameraCalibration camera = g_reconstruction->GetCalibration(scale);
        if (!std::isfinite(camera.fx) || !std::isfinite(camera.fy) || camera.fx <= 0 || camera.fy <= 0 ||
            !g_reconstruction->scan.SetColorCalibration(camera)) {
            LOGE("Reconstruction: rejected camera calibration");
            g_reconstruction->BinderUnlock();
            return 0;
        }
        const auto calibrationFinished = std::chrono::steady_clock::now();

        g_reconstruction->ApplyExperimentalDepth();
        if (!g_reconstruction->t3dr_is_running_.load()) {
            g_reconstruction->BinderUnlock();
            return 0;
        }
        g_reconstruction->depth.ADD(g_reconstruction->frame_points, g_reconstruction->frame_pose[COLOR_CAMERA], g_reconstruction->frame_image);

        //get data in Tango3DR format
        Tango3DR_ImageBuffer t3dr_image;
        t3dr_image.width = (uint32_t) g_reconstruction->frame_image->GetWidth() / scale;
        t3dr_image.height = (uint32_t) g_reconstruction->frame_image->GetHeight() / scale;
        t3dr_image.stride = (uint32_t) (g_reconstruction->frame_image->GetWidth()) / scale;
        t3dr_image.timestamp = g_reconstruction->frame_timestamp;
        t3dr_image.format = TANGO_3DR_HAL_PIXEL_FORMAT_YCrCb_420_SP;
        t3dr_image.data = g_reconstruction->frame_image->ExtractYUVDownscaled(scale);
        Tango3DR_Pose image_pose = g_reconstruction->texturize.Extract3DRPose(g_reconstruction->frame_pose[COLOR_CAMERA]);
        g_reconstruction->frame_pose[OPENGL_CAMERA] = g_reconstruction->frame_viewmat;

        //process pointcloud
        const auto sdkStarted = std::chrono::steady_clock::now();
        // Independently owned until persistence; Update only borrows this cloud.
        Tango3DR_PointCloud* pcl = g_reconstruction->depth.PCL(g_reconstruction->frame_timestamp);
        if (!g_reconstruction->scan.Update(pcl,
                &image_pose, &t3dr_image, &image_pose, g_reconstruction->holes_filling)) {
            if (pcl) {
                Tango3DR_PointCloud_destroy(pcl);
                delete pcl;
            }
            if (g_reconstruction->scan.UpdateFailed()) {
                g_reconstruction->RecoverScan();
            } else if (g_reconstruction->scan.UpdateRejected()) {
                g_reconstruction->recovery_state.store(3);
                g_reconstruction->capture_backoff.Rejected();
            } else if (g_reconstruction->scan.UpdateAccepted()) {
                g_reconstruction->capture_backoff.Reset();
                int rejected = 3;
                g_reconstruction->recovery_state.compare_exchange_strong(rejected, 0);
            }
            g_reconstruction->BinderUnlock();
            return 0;
        }

        //confirm that frame is in dataset
        const auto saveStarted = std::chrono::steady_clock::now();
        camera = g_reconstruction->GetCalibration(1);
        bool distortion_written = true;
        if (!g_reconstruction->distortion_persisted
                || (g_reconstruction->frame_distortion != g_reconstruction->persisted_distortion)) {
            distortion_written = g_reconstruction->dataset->WriteDistortion(g_reconstruction->frame_distortion);
            if (distortion_written) {
                g_reconstruction->persisted_distortion = g_reconstruction->frame_distortion;
                g_reconstruction->distortion_persisted = true;
            }
        }
        int index = g_reconstruction->texturize.GetLatestIndex(g_reconstruction->dataset) + 1;
        auto imageFinished = saveStarted;
        FrameWriter imageWriter([&]() {
            bool written = distortion_written
                    && g_reconstruction->texturize.Add(g_reconstruction->frame_image,
                            g_reconstruction->frame_timestamp, &camera,
                            g_reconstruction->frame_pose, g_reconstruction->dataset);
            imageFinished = std::chrono::steady_clock::now();
            return written;
        }, SCANNER_MODERN != 0 && distortion_written);
        // Image/pose/timestamp files are independent of preview/cloud files.
        // The binder retains all borrowed inputs until the writer has joined.
        // Serial fallback preserves the previous early-failure ordering.
        bool writeGeometry = distortion_written && (imageWriter.Parallel() || imageWriter.Wait());

        //process reconstructed geometry
        const auto previewStarted = std::chrono::steady_clock::now();
        bool preview_written = false;
        try {
            preview_written = writeGeometry
                    && g_reconstruction->dataset->WritePreview(index, g_reconstruction->scan.Added());
        } catch (...) {
            LOGE("Reconstruction: preview write failed");
        }

        //save point cloud into dataset
        const auto cloudStarted = std::chrono::steady_clock::now();
        bool pointcloud_written = false;
        try {
            pointcloud_written = preview_written
                    && g_reconstruction->dataset->WritePointCloud(index, *pcl);
        } catch (...) {
            LOGE("Reconstruction: point cloud write failed");
        }
        bool frame_written = imageWriter.Wait();
        Tango3DR_PointCloud_destroy(pcl);
        delete pcl;

        // Commit state last so interrupted frames are not advertised as complete.
        bool committed = false;
        try {
            committed = frame_written && preview_written && pointcloud_written
                    && g_reconstruction->texturize.Commit(&camera, g_reconstruction->dataset);
        } catch (...) {
            LOGE("Reconstruction: frame publication failed");
        }
        if (!committed) {
            g_reconstruction->scan.DiscardAdded();
            g_reconstruction->scan.RequireRecovery();
            g_reconstruction->t3dr_is_running_ = false;
            g_reconstruction->write_failed.store(true);
            g_reconstruction->BinderUnlock();
            return 0;
        }
        g_reconstruction->recovery_state.store(0);
        g_reconstruction->capture_backoff.Reset();
        const auto mergeStarted = std::chrono::steady_clock::now();
        // Camera update/readback holds the scene lock for much longer than the
        // mesh draw. Do not serialize completed frame publication behind it.
        g_reconstruction->scan_mutex_.lock();
        const auto mergeLocked = std::chrono::steady_clock::now();
        g_reconstruction->scan.Merge();
        g_reconstruction->frame_index = index;
        g_reconstruction->committed_frames = index + 1;
        if (!g_reconstruction->preview_history.empty()) {
            g_reconstruction->preview_history.clear();
            g_reconstruction->preview_history.rehash(0);
        }
        g_reconstruction->preview_cache_last = -2;
        g_reconstruction->scan_mutex_.unlock();

        //postprocess pointcloud
        const auto estimatesStarted = std::chrono::steady_clock::now();
        g_reconstruction->depth.RES(g_reconstruction->capture_resolution);
        if (g_reconstruction->frame_sparse) g_reconstruction->depth.UPD(g_reconstruction->frame_image, g_reconstruction->frame_pose[COLOR_CAMERA], true);
        if (g_reconstruction->holes_filling) g_reconstruction->depth.ADD(g_reconstruction->scan.Components(), g_reconstruction->frame_pose[COLOR_CAMERA], false);

        //photo mode
        if (g_reconstruction->photo_mode) {
            g_reconstruction->t3dr_is_running_ = false;
        }

        g_reconstruction->request_newprojection = true;
        const auto finished = std::chrono::steady_clock::now();
        const auto milliseconds = [](std::chrono::steady_clock::time_point begin,
                                     std::chrono::steady_clock::time_point end) {
            return std::chrono::duration<double, std::milli>(end - begin).count();
        };
        const double timings[12] = {
            milliseconds(g_reconstruction->frame_capture_started, workerStarted),
            milliseconds(workerStarted, sdkStarted),
            milliseconds(sdkStarted, saveStarted),
            milliseconds(saveStarted, estimatesStarted),
            milliseconds(estimatesStarted, finished),
            milliseconds(saveStarted, imageFinished),
            milliseconds(previewStarted, cloudStarted),
            milliseconds(cloudStarted, mergeStarted),
            milliseconds(mergeStarted, mergeLocked),
            milliseconds(mergeLocked, estimatesStarted),
            milliseconds(calibrationStarted, calibrationFinished),
            imageWriter.Parallel() ? 1.0 : 0.0
        };
        g_reconstruction->RecordCaptureTimings(timings);
        g_reconstruction->BinderUnlock();
        return 0;
    }

    Reconstruction::Reconstruction() {
        sem_init(&binder_semaphore_, 0, 1);
        dataset = nullptr;
        jumped = false;
        paused = true;
        tracked = false;
        lost = false;
        photo_mode = false;
        request_newprojection = true;
        t3dr_is_running_ = false;
        frame_index = -1;

        sprintf(pose_feedback, "");
        g_reconstruction = this;
        selector.Init(360, 640);
    }

    void Reconstruction::RecordCaptureTimings(const double* milliseconds) {
        // The reconstruction worker owns the binder. No telemetry thread, file
        // writes or UI polling: emit one aggregate per 30 committed frames.
        if (capture_timing_count == 0) capture_window_started = frame_capture_started;
        for (int i = 0; i < 12; ++i) capture_timing_sums[i] += milliseconds[i];
        if (++capture_timing_count < 30) return;
        const double span = std::chrono::duration<double, std::milli>(
                frame_capture_started - capture_window_started).count();
        const double interval = span / 29.0;
        LOGI("CAPTURE_MS samples=30 prepare=%.2f point_yuv=%.2f sdk=%.2f persist_merge=%.2f estimates=%.2f "
             "image_meta=%.2f preview_sync=%.2f cloud_commit=%.2f merge_wait=%.2f merge=%.2f calibration=%.2f io_parallel=%.0f%% size=%dx%d input_points=%zu interval=%.2f capture_hz=%.2f",
             capture_timing_sums[0] / 30, capture_timing_sums[1] / 30,
             capture_timing_sums[2] / 30, capture_timing_sums[3] / 30, capture_timing_sums[4] / 30,
             capture_timing_sums[5] / 30, capture_timing_sums[6] / 30, capture_timing_sums[7] / 30,
             capture_timing_sums[8] / 30, capture_timing_sums[9] / 30,
              capture_timing_sums[10] / 30, capture_timing_sums[11] * 100 / 30,
               frame_image->GetWidth(), frame_image->GetHeight(), frame_points.size(),
               interval, interval > 0 ? 1000.0 / interval : 0.0);
        scan.LogStorageStats();
        capture_timing_count = 0;
        for (double& value : capture_timing_sums) value = 0;
    }

    void Reconstruction::BinderLock() {
        int result;
        do {
            result = sem_wait(&binder_semaphore_);
        } while ((result == -1) && (errno == EINTR));
        if (result == -1) abort();
    }

    bool Reconstruction::BinderTryLock() {
        return sem_trywait(&binder_semaphore_) == 0;
    }

    bool Reconstruction::TryLockFrame(bool& running, bool faceMode) {
        if (!BinderTryLock()) return false;
        // Read the desired state exactly once while holding the binder. Recheck
        // cooldown here so a worker's just-published failure cannot be missed.
        running = t3dr_is_running_.load();
        if (running && !faceMode && !capture_backoff.Ready()) {
            BinderUnlock();
            return false;
        }
        return true;
    }

    void Reconstruction::BinderUnlock() {
        sem_post(&binder_semaphore_);
    }

    bool Reconstruction::RecoverScan() {
        if (!scan.UpdateFailed()) return true;
        recovery_state.store(1);
        event_mutex_.lock();
        event_ = "Recovering scan";
        event_mutex_.unlock();
        bool storageFailure = false;
        bool cancelled = false;
        bool recovered = scan.RecoverContext(dataset, texturize, committed_frames, storageFailure,
                                             t3dr_is_running_, cancelled);
        event_mutex_.lock();
        if (cancelled) {
            // Keep the volume marked for replay on Resume; accepted meshes/files
            // remain untouched. A deliberate pause is not history corruption.
            recovery_state.store(0);
            event_.clear();
        } else if (recovered) {
            recovery_state.store(0);
            capture_backoff.Reset();
            event_.clear();
            request_newprojection = true;
        } else {
            t3dr_is_running_ = false;
            if (storageFailure) {
                recovery_state.store(0);
                history_failed.store(true);
                event_ = "HISTORY_FAILED";
            } else {
                recovery_state.store(2);
                event_ = "Reconstruction unavailable; retry scanning";
            }
            LOGE("Reconstruction recovery failed (%s); committed scan preserved",
                 storageFailure ? "dataset" : "SDK/device");
        }
        event_mutex_.unlock();
        return recovered;
    }

    void Reconstruction::AddPoses() {
        float m = request_distance;
        float step = request_distance * 0.25f;
        for (float x = -m; x <= m; x += step) {
            for (float y = -m; y <= m; y += step) {
                for (float z = -m; z <= m; z += step) {
                    if ((fabs(x) > 0) || (fabs(y) > 0) || (fabs(z) > 0)) {
                        if (fabs(x) + fabs(y) + fabs(z) < request_distance * 1.5f) {
                            glm::mat4 mat = frame_viewmat;
                            mat[3][0] += x;
                            mat[3][1] += y;
                            mat[3][2] += z;
                            request_matrix.push_back(mat);
                        }
                    }
                }
            }
        }
    }

    Reconstruction::CVDescription Reconstruction::DetectFeatures(Image* frame) {
        cv::Mat mat(frame->GetHeight(), frame->GetWidth(), CV_8UC1);
        for (int x = 0; x < frame->GetWidth(); x++) {
            for (int y = 0; y < frame->GetHeight(); y++) {
                glm::ivec4 color = frame->GetColorRGBA(x, y);
                mat.at<uchar>(y, x) = (color.r + color.g + color.b) / 3;
            }
        }
        CVDescription output;
        detector->detect(mat, output.keypoints);
        if (!output.keypoints.empty()) {
            extractor->compute(mat, output.keypoints, output.descriptors);
        }
        return output;
    }

    double Reconstruction::GetAccuracy(std::vector<cv::DMatch>& matches, Reconstruction::CVDescription& a, Reconstruction::CVDescription& b) {
        std::vector<float> errors;
        for (cv::DMatch& m : matches) {
            cv::Point2f p1 = a.keypoints[m.queryIdx].pt;
            cv::Point2f p2 = b.keypoints[m.trainIdx].pt;
            float dx = p1.x - p2.x;
            float dy = p1.y - p2.y;
            float diff = sqrt(dx * dx + dy * dy);
            if (!std::isfinite(diff)) return INT_MAX;
            errors.push_back(diff);
        }
        double error = INT_MAX;
        if (!errors.empty()) {
            error = 0;
            int size = std::max(1, (int)errors.size() / 2);
            std::sort(errors.begin(), errors.end());
            for (int i = 0; i < size; i++) {
                error += errors[i] / (float)size;
            }
        }
        return error;
    }

    Tango3DR_CameraCalibration Reconstruction::GetCalibration(int scale) {
        float w = frame_image->GetWidth() / scale;
        float h = frame_image->GetHeight() / scale;
        Tango3DR_CameraCalibration camera = {};
        texturize.ApplyDistortion(camera, frame_distortion);
        camera.width = (uint32_t) w;
        camera.height = (uint32_t) h;
        camera.cx = fabs(w * (1.0f - frame_calibration[2][0]) / 2.0f);
        camera.cy = fabs(h * (1.0f - frame_calibration[2][1]) / 2.0f);
        camera.fx = fabs(w * frame_calibration[0][0] / 2.0f);
        camera.fy = fabs(h * frame_calibration[1][1] / 2.0f);
        return camera;
    }

    std::string Reconstruction::GetEvent() {
        event_mutex_.lock();
        std::string output = event_;
        if (strlen(pose_feedback) > 0)
            output = pose_feedback;

        if (lost)
            output = tracked ? "MT_LOST" : "MT_INIT";
        if (!texturize.GetEvent().empty())
            output = texturize.GetEvent();
        if (!GLSL::GetError().empty())
            output = GLSL::GetError();
        if (recovery_state.load() == 1) output = "Recovering scan";
        if (recovery_state.load() == 2) output = "Reconstruction unavailable; retry scanning";
        if (recovery_state.load() == 3 && t3dr_is_running_.load())
            output = "Frame rejected; accepted scan kept";
        if (output.empty()) {
            //output = depth.DBG() + scan.DebugInfo();
        }
        event_ = "";
        event_mutex_.unlock();
        return output;
    }

    bool Reconstruction::InitTexturing(std::string input, bool fast) {
        return texturize.Init(input, true, fast);
    }

    void Reconstruction::PreviewChange(int frames) {
        BinderLock();
        if (history_failed.load()) {
            BinderUnlock();
            return;
        }

        //update cache
        int last = texturize.GetLatestIndex(dataset);
        if (preview_cache_last != last) {
            preview_history.clear();
            for (int i = 0; i <= last; i++) {
                bool frameValid = false;
                std::vector<std::pair<GridIndex, Tango3DR_Mesh *>> preview =
                        dataset->ReadPreview(i, true, &frameValid);
                if (!frameValid) {
                    event_mutex_.lock();
                    event_ = "HISTORY_FAILED";
                    event_mutex_.unlock();
                    history_failed.store(true);
                    BinderUnlock();
                    return;
                }
                for (std::pair<GridIndex, Tango3DR_Mesh *>& p : preview) {
                    preview_history[p.first].push_back(i);
                }
            }
            preview_cache_last = last;
        }

        //select frame
        long previousFrame = frame_index;
        int64_t target = (int64_t) frame_index + frames;
        if (target <= -1) target = -1;
        if (target >= last) target = last;
        frame_index = (long) target;

        // Find the latest update for every grid cell at the selected frame.
        std::unordered_map<GridIndex, int, GridIndexHasher> desired;
        std::map<int, bool> framesToRead;
        for (const std::pair<const GridIndex, std::vector<int>>& history : preview_history) {
            std::vector<int>::const_iterator it = std::upper_bound(
                    history.second.begin(), history.second.end(), (int) frame_index);
            if (it == history.second.begin()) continue;
            int source = *(--it);
            desired[history.first] = source;
            framesToRead[source] = true;
        }

        // Read each required frame once and keep only its selected grid cells.
        std::vector<std::pair<GridIndex, Tango3DR_Mesh *> > data;
        data.reserve(desired.size());
        bool framesValid = true;
        for (const std::pair<const int, bool>& source : framesToRead) {
            int i = source.first;
            bool frameValid = false;
            std::vector<std::pair<GridIndex, Tango3DR_Mesh *>> preview =
                    dataset->ReadPreview(i, false, &frameValid);
            if (!frameValid) {
                framesValid = false;
                break;
            }
            for (std::pair<GridIndex, Tango3DR_Mesh *>& d : preview) {
                std::unordered_map<GridIndex, int, GridIndexHasher>::const_iterator selected = desired.find(d.first);
                if ((selected != desired.end()) && (selected->second == i)) {
                    data.push_back(d);
                } else {
                    Tango3DR_Mesh_destroy(d.second);
                    delete d.second;
                }
            }
        }

        if (!framesValid || (data.size() != desired.size())) {
            for (std::pair<GridIndex, Tango3DR_Mesh *>& p : data) {
                Tango3DR_Mesh_destroy(p.second);
                delete p.second;
            }
            event_mutex_.lock();
            event_ = "HISTORY_FAILED";
            event_mutex_.unlock();
            history_failed.store(true);
            frame_index = previousFrame;
            BinderUnlock();
            return;
        }

        // Replace the preview with the selected historical state.
        render_mutex_.lock();
        scan.ClearGeometry();
        scan.Merge(data);
        render_mutex_.unlock();
        BinderUnlock();
    }

    void Reconstruction::RenderGL(glm::mat4 view) {
        int w = request_image->GetWidth();
        int h = request_image->GetHeight();
        if (!renderer) {
            renderer = new GLRenderer();
            renderer->Init(scene.renderer->width, scene.renderer->height, w, h);
        }
        glClearColor(1, 1, 1, 1);
        renderer->Rtt(true);
        glViewport(0, 0, w, h);
        scene.CustomRender(frame_calibration * view);
        renderer->Rtt(false);

        if (rendered_image) {
            delete rendered_image;
        }
        rendered_image = renderer->ReadRtt(0, 0, w, h);
    }

    void Reconstruction::Setup(double res, double dmin, double dmax, int noise, bool holesFilling,
                               bool poseCorrection, bool distortion, bool clearing, std::string dataset_path,
                               std::string cache_path) {
        holes_filling = holesFilling;
        pose_correction = poseCorrection;
        lost = true;
        distortion_persisted = false;
        persisted_distortion.clear();
        preview_cache_last = -2;
        preview_history.clear();
        texturize.SetDistortion(distortion);

        BinderLock();
        render_mutex_.lock();
        scan.ConfigurePaging(cache_path, std::fabs(res));
        capture_resolution=std::fabs(res);
        double previewResolution=capture_resolution;
#if SCANNER_MODERN
        previewResolution=CapturePreviewResolution(capture_resolution,coverage_preview_requested.load(),poseCorrection,holesFilling);
#endif
        coverage_preview=previewResolution!=capture_resolution;
        LOGI("CAPTURE_POLICY requested=%.5f preview=%.5f coverage=%d",capture_resolution,previewResolution,int(coverage_preview.load()));
        if (dataset) delete dataset;
        committed_frames = 0;
        capture_timing_count = 0;
        for (double& value : capture_timing_sums) value = 0;
        recovery_state.store(0);
        frame_index = -1;
        capture_backoff.Reset();
        start = std::chrono::steady_clock::now();
        dataset = new Dataset(dataset_path);
        if (scan.Context())
            scan.Reset3DR(previewResolution, dmin, dmax, noise, clearing);
        else
            scan.Setup3DR(previewResolution, dmin, dmax, noise, clearing);
        render_mutex_.unlock();
        BinderUnlock();
    }


    void Reconstruction::SetPhotoMode(bool on) {
        photo_mode.store(on);
    }

    void Reconstruction::Start(ReconstructionThread thread) {
        struct thread_info *tinfo = 0;
        pthread_t threadId;
        int result = -1;
        switch (thread) {
            case DUMMY:
                result = pthread_create(&threadId, NULL, ProcessDummy, tinfo);
                break;
            case POSE_CORRECTION:
                result = pthread_create(&threadId, NULL, ProcessPoseCorrection, tinfo);
                break;
            case RECONSTRUCTION:
                result = pthread_create(&threadId, NULL, ProcessReconstruction, tinfo);
                break;
        }
        if (result == 0) pthread_detach(threadId);
        else BinderUnlock();
    }

    bool Reconstruction::FinishCapture() {
        BinderLock();
        struct Release { Reconstruction& r;~Release(){r.BinderUnlock();} } release{*this};
        if(t3dr_is_running_.load()||!dataset||history_failed.load()||write_failed.load())return false;
        int count=0,w=0,h=0;double cx=0,cy=0,fx=0,fy=0;
        dataset->ReadState(count,w,h,cx,cy,fx,fy);
        if(count<=0||count>100000||count!=committed_frames||frame_index!=count-1||w<=0||h<=0||w>8192||h>8192||
           !std::isfinite(cx)||!std::isfinite(cy)||!std::isfinite(fx)||!std::isfinite(fy)||fx<=0||fy<=0)return false;
        for(const char* extension:{".jpg",".mat",".pcl",".bin",".tms"}) {
            struct stat info{};
            if(stat(dataset->GetFileName(count-1,extension).c_str(),&info)!=0||!S_ISREG(info.st_mode)||info.st_size<=0)return false;
        }
        return true;
    }

    bool Reconstruction::PrepareFullResolution() {
        BinderLock();
        double preview=scan.Resolution();
        if(!coverage_preview) { BinderUnlock();return true; }
        bool ready=false;
        {
            std::lock_guard<std::mutex> render(render_mutex_);
            ready=scan.SelectResolution(capture_resolution);
            if(ready)coverage_preview=false;
        }
        BinderUnlock();
        if(ready&&Undo(false,false))return true;
        // A failed detailed reconstruction must never export the coarse proxy.
        // Keep the recording and restore a recoverable coverage context.
        BinderLock();
        {
            std::lock_guard<std::mutex> render(render_mutex_);
            scan.SelectResolution(preview);scan.RequireRecovery();coverage_preview=true;
        }
        BinderUnlock();return false;
    }

    bool Reconstruction::Undo(bool fromUser, bool applyTextures) {
        BinderLock();
        render_mutex_.lock();
        if (history_failed.load()) {
            render_mutex_.unlock();
            BinderUnlock();
            return false;
        }
        request_newprojection = true;
        if (!scan.Data().empty() || !fromUser) {
            render_mutex_.unlock();

            //mesh reconstruction
            double cx = 0, cy = 0, fx = 0, fy = 0;
            Tango3DR_Status ret;
            int persistedCount = 0, width = 0, height = 0;
            dataset->ReadState(persistedCount, width, height, cx, cy, fx, fy);
            int count = fromUser ? std::max(0, std::min(persistedCount, (int) frame_index + 1)) : persistedCount;
            if ((width <= 0) || (height <= 0) || (width > 8192) || (height > 8192)) {
                event_mutex_.lock();
                event_ = "HISTORY_FAILED";
                event_mutex_.unlock();
                history_failed.store(true);
                BinderUnlock();
                return false;
            }

            render_mutex_.lock();
            scan.ClearContext();
            render_mutex_.unlock();
            if (scan.UpdateFailed()) {
                event_mutex_.lock();
                recovery_state.store(2);
                event_ = "Reconstruction unavailable; retry scanning";
                event_mutex_.unlock();
                t3dr_is_running_ = false;
                BinderUnlock();
                return false;
            }

            int scale = width > 1000 ? 3 : 1;
            texturize.SetCalibration(scan.Context(), dataset, scale);
            Tango3DR_ImageBuffer image = {};
            image.width = (uint32_t) width / scale;
            image.height = (uint32_t) height / scale;
            image.stride = (uint32_t) width / scale;
            image.format = TANGO_3DR_HAL_PIXEL_FORMAT_YCrCb_420_SP;
#if SCANNER_MODERN
            const bool readImage=applyTextures||fromUser;
#else
            const bool readImage=applyTextures;
            if (!readImage) image.data = new unsigned char[width / scale * height / scale * 2]{};
#endif
            std::unordered_map<GridIndex, bool, GridIndexHasher> added;
            bool replayValid = true;
            bool historyInvalid = false;
            for (int i = 0; replayValid && (i < count); i++) {
                std::ostringstream ss;
                ss << "CONVERT ";
                ss << i + 1;
                ss << "/";
                ss << count + 1;
                texturize.SetEvent(ss.str());

                std::vector<glm::mat4> poses;
                bool poseValid = dataset->ReadPose(i, poses);
                Tango3DR_PointCloud t3dr_depth = dataset->ReadPointCloud(i);
                if (!poseValid || (poses.size() <= COLOR_CAMERA) ||
                    !geometry::RigidPose(poses[COLOR_CAMERA]) || !geometry::Cloud(&t3dr_depth)) {
                    Tango3DR_PointCloud_destroy(&t3dr_depth);
                    replayValid = false;
                    historyInvalid = true;
                    break;
                }
                Tango3DR_Pose image_pose = texturize.Extract3DRPose(poses[COLOR_CAMERA]);

                Tango3DR_GridIndexArray t3dr_updated = {};
                image.timestamp = t3dr_depth.timestamp;
                if (readImage) {
                    std::string imagePath = dataset->GetFileName(i, ".jpg");
                    FILE* imageFile = fopen(imagePath.c_str(), "rb");
                    if (!imageFile) {
                        Tango3DR_PointCloud_destroy(&t3dr_depth);
                        replayValid = false;
                        historyInvalid = true;
                        break;
                    }
                    fclose(imageFile);
                    Image frame(imagePath);
                    if (!frame.IsValid()) {
                        Tango3DR_PointCloud_destroy(&t3dr_depth);
                        replayValid = false;
                        historyInvalid = true;
                        break;
                    }
                    image.data = frame.ExtractYUVDownscaled(scale);
                }
                // Geometry-only modern replay neither decodes nor invents image
                // pixels. The later texture pass reads the original photographs.
#if SCANNER_MODERN
                const Tango3DR_ImageBuffer* replayImage=readImage?&image:nullptr;
#else
                const Tango3DR_ImageBuffer* replayImage=&image;
#endif
                ret = Tango3DR_updateFromPointCloud(scan.Context(), &t3dr_depth, &image_pose,
                                                    replayImage, &image_pose, &t3dr_updated);
                if (ret == TANGO_3DR_SUCCESS && (!t3dr_updated.num_indices || t3dr_updated.indices)) {
                    GridIndex index;
                    unsigned long size = t3dr_updated.num_indices;
                    for (unsigned long it = 0; it < size; ++it) {
                        index.indices[0] = t3dr_updated.indices[it][0];
                        index.indices[1] = t3dr_updated.indices[it][1];
                        index.indices[2] = t3dr_updated.indices[it][2];
                        added[index] = true;
                    }
                } else replayValid = false;
                Tango3DR_GridIndexArray_destroy(&t3dr_updated);
                Tango3DR_PointCloud_destroy(&t3dr_depth);
                if (readImage) delete[] image.data;
            }
            if (!readImage) delete[] image.data;
            texturize.SetEvent("");

            //unpack results
            std::vector<std::pair<GridIndex, Tango3DR_Mesh*> > toAdd;
            for (auto &p : added) {
                if (!replayValid) break;
                std::pair<GridIndex, Tango3DR_Mesh*> pair;
                pair.first = p.first;
                pair.second = new Tango3DR_Mesh();
                ret = Tango3DR_extractMeshSegment(scan.Context(), pair.first.indices, pair.second);
                if (ret != TANGO_3DR_SUCCESS || !geometry::Mesh(pair.second)) {
                    Tango3DR_Mesh_destroy(pair.second);
                    delete pair.second;
                    replayValid = false;
                    break;
                }
                toAdd.push_back(pair);
            }

            if (fromUser && replayValid) {
                replayValid = texturize.Truncate(count, dataset);
                historyInvalid = !replayValid;
            }
            if (!replayValid) {
                scan.RequireRecovery();
                for (std::pair<GridIndex, Tango3DR_Mesh *>& p : toAdd) {
                    Tango3DR_Mesh_destroy(p.second);
                    delete p.second;
                }
                event_mutex_.lock();
                event_ = historyInvalid ? "HISTORY_FAILED" : "Reconstruction unavailable; retry scanning";
                recovery_state.store(historyInvalid ? 0 : 2);
                event_mutex_.unlock();
                if (historyInvalid) history_failed.store(true);
                t3dr_is_running_ = false;
                BinderUnlock();
                return false;
            }

            //merge results
            render_mutex_.lock();
            scan.ClearGeometry();
            for (auto &p : toAdd) {
                scan.Add(p.first, p.second);
            }
            frame_index = count - 1;
            committed_frames = count;
            preview_cache_last = -2;
            recovery_state.store(0);
        }
        event_mutex_.lock();
        sprintf(pose_feedback, "");
        event_mutex_.unlock();

        render_mutex_.unlock();
        BinderUnlock();
        return true;
    }
}
