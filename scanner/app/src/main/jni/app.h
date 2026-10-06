#ifndef APP_H
#define APP_H

#include <jni.h>
#include <cstdint>
#include <mutex>
#include <string>

#include <editor/effector.h>
#include <editor/selector.h>
#include <thread/reconstr.h>

namespace oc {

    struct EditorBackup {
        std::vector<Mesh> mesh;
    };

    class App {
    public:
        App();
        bool OnARServiceConnected(JNIEnv *env, jobject context, double res, double dmin,
                                  double dmax, int noise, bool holesFilling, bool poseCorrection,
                                  bool distortion, bool offset, bool flashlight, int mode,
                                  bool clearing, std::string dataset, std::string cacheDirectory);
        void OnSurfaceChanged(int width, int height, bool fullhd);
        void OnSurfaceCreated();
        bool OnDrawFrame(bool facemode, float compassYaw, int viewmode, bool anchors, bool grid, bool smooth);
        bool OnDrawScan(glm::vec3 pos, bool zoomable);
        void OnToggleButtonClicked(bool t3dr_is_running);
        bool OnClearButtonClicked();
        void OnUndoButtonClicked(bool fromUser, bool texturize);
        void OnUndoPreviewUpdate(int frames);
        void OnPause();
        void OnResume();

        void Extract(std::string path, int mode);
        bool Load(std::string filename);
        void Optimize(std::string filename);
        bool Save(std::string filename);
        bool SaveWithTextures(std::string filename);
        void SetTextureParams(int detail, int res, int count);
        void SetExperimentalDepth(bool enabled);
        void SetCoveragePreview(bool enabled);
        bool FinishCapture();
        std::string GetExperimentalDepthStatus();
        std::string GetCaptureDiagnostics();
        bool Texturize(std::string input, std::string output, bool poisson, bool twoPass);
        std::string GetTexturingError();

        bool AnimFinished();
        bool DidARJump();
        bool DidHistoryFail();
        bool DidWriteFail();
        float GetView(int axis);
        float GetDistance(float x1, float y1, float x2, float y2);
        float GetFloorLevel(float x, float y, float z);
        void SetView(float p, float y, float mx, float my, float mz, float o, bool g);
        std::string GetEvent();
        int GetRecoveryState() const;
        void SetEvent(std::string value);
        void SetPhotoMode(bool on);
        int GetScanSize();

        void Backup();
        void Restore();
        void ApplyEffect(Effector::Effect effect, float value, int axis);
        void PreviewEffect(Effector::Effect effect, float value, int axis);
        void ShowNormals(bool on);

        void ApplySelection(float x, float y, bool triangle);
        void CompleteSelection(bool inverse);
        void MultSelection(bool increase);
        void CircleSelection(float x, float y, float radius, bool invert);
        void RectSelection(float x1, float y1, float x2, float y2, bool invert);

    private:

        // Called with render_mutex_ held, on the GL thread.
        bool ResumeARIfActive();

        //objects
        ARCoreService* ar;
        Reconstruction reconstruction;
        Effector editor;
        Selector selector;
        std::vector<EditorBackup> backups;
        std::vector<Tango3DR_Mesh*> visible_scan;
        uint64_t visible_scan_revision;
        int visible_scan_min[3];
        int visible_scan_max[3];
        //status
        bool gyro, oriented;
        std::atomic<bool> activity_resumed{false};
        std::atomic<bool> offline_export{false};
        std::mutex texturing_result_mutex;
        std::mutex capture_diagnostic_mutex;
        std::string capture_diagnostics;
        // Protected by texturing_result_mutex; queried after the export JNI call.
        std::string texturing_error;
        int export_texture_size = 2048;
        int export_texture_count = 4;
        std::string export_scratch_directory;
        //transform
        float movex, lastMovex;
        float movey, lastMovey;
        float movez, lastMovez;
        float orbit, lastOrbit;
        float pitch, lastPitch;
        float yaw, lastYaw;
        float lowest;
    };
}

#endif
