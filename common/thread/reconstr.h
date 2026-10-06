#ifndef THREAD_RECONSTRUCTION_H
#define THREAD_RECONSTRUCTION_H

#include <mutex>
#include <semaphore.h>
#include <atomic>
#include <chrono>

#include <data/dataset.h>
#include <editor/selector.h>
#include <tango/retango.h>
#include <tango/scan.h>
#include <tango/texturize.h>
#include <thread/scene.h>
#include <thread/capture_backoff.h>
#include <depth/experimental.h>

#include <opencv2/core.hpp>
#include <opencv2/features2d.hpp>

namespace oc {

    class Reconstruction {
    public:
        enum ReconstructionThread { DUMMY, POSE_CORRECTION, RECONSTRUCTION };

        struct CVDescription {
            std::vector< cv::KeyPoint > keypoints;
            cv::Mat descriptors;
        };

        Reconstruction();
        void BinderLock();
        bool BinderTryLock();
        bool TryLockFrame(bool& running, bool faceMode);
        void BinderUnlock();
        void AddPoses();
        CVDescription DetectFeatures(Image* frame);
        double GetAccuracy(std::vector<cv::DMatch>& matches, Reconstruction::CVDescription& a, Reconstruction::CVDescription& b);
        Tango3DR_CameraCalibration GetCalibration(int scale);
        std::string GetEvent();
        bool InitTexturing(std::string input, bool fast);
        void PreviewChange(int frames);
        void RenderGL(glm::mat4 matrix);
        void Setup(double res, double dmin, double dmax, int noise, bool holesFilling,
                   bool poseCorrection, bool distortion, bool clearing, std::string dataset_path,
                   std::string cache_path = "");
        void SetPhotoMode(bool on);
        void Start(ReconstructionThread thread);
        bool Undo(bool fromUser, bool applyTextures);
        bool FinishCapture();
        bool PrepareFullResolution();
        // Called by the reconstruction worker while it owns the binder.
        bool RecoverScan();
        void RecordCaptureTimings(const double* milliseconds);
        void ApplyExperimentalDepth();

    public:
        //objects
        Dataset* dataset;
        Retango depth;
        Scene scene;
        TangoScan scan;
        TangoTexturize texturize;
        //mutexes
        sem_t binder_semaphore_;
        std::mutex render_mutex_;
        // Live preview mesh publication/drawing only. Never held across camera
        // acquisition, GPU readback or dataset I/O. Lock order when combined:
        // binder -> render_mutex_ -> scan_mutex_. The worker needs only scan_mutex_.
        std::mutex scan_mutex_;
        std::mutex event_mutex_;
        //frame data
        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
        std::chrono::steady_clock::time_point frame_capture_started;
        std::chrono::steady_clock::time_point capture_window_started;
        unsigned int capture_timing_count = 0;
        double capture_timing_sums[12] = {};
        glm::mat4 frame_calibration;
        std::vector<float> frame_distortion;
        std::vector<float> persisted_distortion;
        bool distortion_persisted = false;
        std::atomic<bool> write_failed{false};
        std::atomic<bool> history_failed{false};
        glm::mat4 frame_viewmat;
        Image* frame_image = 0;
        std::vector<glm::vec4> frame_points;
        std::shared_ptr<depth_test::Frame> frame_depth_test;
#if SCANNER_MODERN
        depth_test::Runtime depth_test_runtime;
#endif
        std::vector<glm::mat4> frame_pose;
        double frame_timestamp;
        bool frame_sparse;
        std::atomic<bool> coverage_preview_requested{false};
        std::atomic<bool> coverage_preview{false};
        double capture_resolution=.04;
        bool holes_filling;
        std::atomic<bool> photo_mode{false};
        bool pose_correction;
        // Control requests must not wait for committed-history replay to finish.
        std::atomic<bool> t3dr_is_running_{false};
        // 0: normal, 1: replaying, 2: replay paused, 3: atomic frame rejection.
        std::atomic<int> recovery_state{0};
        CaptureBackoff capture_backoff;
        //tracking
        std::string event_;
        bool jumped;
        bool paused;
        bool tracked;
        bool lost;
        char pose_feedback[1024];
        //pose correction
        Selector selector;
        GLRenderer* renderer = 0;
        Image* rendered_depth = 0;
        Image* rendered_image = 0;
        Image* request_image = 0;
        float request_distance;
        bool request_newprojection;
        std::vector<glm::mat4> request_matrix;
        //scan preview
        long frame_index;
        int committed_frames = 0;
        int preview_cache_last = -2;
        std::unordered_map<GridIndex, std::vector<int>, GridIndexHasher> preview_history;
    };
}
#endif
