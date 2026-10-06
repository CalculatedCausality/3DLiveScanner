#include <cstdio>
#include <cmath>
#include <cctype>
#include <cerrno>
#include <memory>
#include <mutex>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <sys/stat.h>
#include <arcore/service.h>
#include <exporter/csvposes.h>
#include <exporter/floorpln.h>
#include <postproc/optimizer.h>
#include <postproc/poisson.h>
#include <postproc/texturize.h>
#include <gl/scan_visibility.h>
#if SCANNER_MODERN
#include <dataset_texturing.h>
#endif
#include <zconf.h>
#include "app.h"

static oc::App app;

namespace oc {

    void AnalyseCallback(int current, int count) {
        if (count <= 0) {
            app.SetEvent("CONVERT");
        } else {
            std::ostringstream ss;
            ss << "ANALYSE ";
            ss << current;
            ss << "/";
            ss << count;
            app.SetEvent(ss.str());
        }
    }

    App::App() :  visible_scan_revision(~uint64_t(0)),
                  gyro(true),
                  lastMovex(0),
                  lastMovey(0),
                  lastMovez(0),
                  lastOrbit(0),
                  lastPitch(0),
                  lastYaw(0) {
        ar = nullptr;
        oriented = false;
    }

    bool App::OnARServiceConnected(JNIEnv *env, jobject context, double res, double dmin,
                                   double dmax, int noise, bool holesFilling, bool poseCorrection,
                                   bool distortion, bool offset, bool flashlight, int mode,
                                   bool clearing, std::string dataset_path, std::string cache_path) {
#if SCANNER_MODERN
        reconstruction.depth_test_runtime.Invalidate();
#endif

        {
            std::lock_guard<std::mutex> renderLock(reconstruction.render_mutex_);
            if (ar) delete ar;
            ar = new ARCoreService(env, context, (ARCoreService::Mode)mode, flashlight);
            ar->SetOffset(offset ? (float) fabs(res) : 0);
            ar->SetResolution((float)fabs(res));
            reconstruction.paused = true;
            export_scratch_directory = cache_path;
        }
        reconstruction.Setup(res, dmin, dmax, noise, holesFilling, poseCorrection, distortion, clearing,
                             dataset_path, cache_path);
        if (!reconstruction.scan.Context() || reconstruction.scan.UpdateFailed()) return false;

        std::string access = reconstruction.dataset->GetPath() + "/test.txt";
        FILE* file = fopen(access.c_str(), "w");
        if (file) {
            fclose(file);
            return true;
        } else {
            return false;
        }
    }

    void App::OnSurfaceChanged(int width, int height, bool fullhd) {
        reconstruction.BinderLock();
        reconstruction.render_mutex_.lock();
        if (ar) {
            ar->OnDisplayGeometryChanged(0, width, height, fullhd);
        }
        reconstruction.scene.SetupViewPort(width, height);
        if (reconstruction.renderer) {
            delete reconstruction.renderer;
            reconstruction.renderer = 0;
        }
        reconstruction.request_newprojection = true;
        selector.Init(width, height);
        reconstruction.render_mutex_.unlock();
        reconstruction.BinderUnlock();
    }

    void App::OnSurfaceCreated() {
        reconstruction.BinderLock();
        reconstruction.render_mutex_.lock();
        reconstruction.scene.AbandonGlContext();
        if (reconstruction.renderer) {
            reconstruction.renderer->AbandonGlContext();
            delete reconstruction.renderer;
            reconstruction.renderer = 0;
        }
        reconstruction.request_newprojection = true;
        if (ar) {
            ar->OnGlContextLost();
            ar->OnPause();
            // A surface can be recreated while the Activity is backgrounded.
            // Only a foreground draw may resume the camera session.
            reconstruction.paused = true;
        }
        reconstruction.render_mutex_.unlock();
        reconstruction.BinderUnlock();
    }

    bool App::OnDrawFrame(bool facemode, float compassYaw, int viewmode, bool anchors, bool grid, bool smooth) {
        reconstruction.render_mutex_.lock();

        //clear screen
        glClearColor(0, 0, 0, 0);
        glClearStencil(0);
        glDisable(GL_BLEND);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

        //camera transformation
        glm::vec3 pos = reconstruction.scene.renderer->camera.position;
        if (facemode) {
            lastOrbit = orbit;
            lastPitch = pitch;
            lastYaw   = yaw;
            viewmode  = 1;
            reconstruction.scene.renderer->camera.position = glm::vec3(lastMovex, lastMovez, lastMovey);
            reconstruction.scene.renderer->camera.rotation = glm::quat(glm::vec3(-lastYaw, -lastPitch, 0));
            reconstruction.scene.renderer->camera.position += reconstruction.scene.renderer->camera.rotation * glm::vec3(0, 0, lastOrbit);
            reconstruction.scene.renderer->camera.scale    = glm::vec3(1, 1, 1);
        } else if (!gyro) {
            if (smooth) {
                lastMovex = lastMovex * 0.9f + movex * 0.1f;
                lastMovey = lastMovey * 0.9f + movey * 0.1f;
                lastMovez = lastMovez * 0.9f + movez * 0.1f;
                lastOrbit = lastOrbit * 0.95f + orbit * 0.05f;
                lastPitch = lastPitch * 0.95f + pitch * 0.05f;
                lastYaw   = lastYaw   * 0.95f + yaw   * 0.05f;
            } else {
                lastMovex = movex;
                lastMovey = movey;
                lastMovez = movez;
                lastOrbit = orbit;
                lastPitch = pitch;
                lastYaw   = yaw;
            }
            reconstruction.scene.renderer->camera.position = glm::vec3(lastMovex, lastMovez, lastMovey);
            reconstruction.scene.renderer->camera.rotation = glm::quat(glm::vec3(lastYaw, lastPitch, 0));
            reconstruction.scene.renderer->camera.scale    = glm::vec3(1, 1, 1);
            if (lastOrbit > 0) {
                reconstruction.scene.renderer->camera.position += reconstruction.scene.renderer->camera.rotation * glm::vec3(0, 0, lastOrbit);
            }
        }

        //AR and reconstruction thread initialisation
        bool zoomable = (viewmode == 0) || (viewmode == 3);
        const bool arActive = ResumeARIfActive();
        if (arActive) {
            ar->SetNVScheme(ARCoreCamera::WHITE2BLUE);
            if (!oriented && ar->HasCoordinateSystem()) {
                reconstruction.dataset->WriteYaw(compassYaw);
                oriented = true;
            }

            const bool processed = ar->Process();
            {
                auto diagnostic=ar->GetCaptureDiagnostics();
                std::lock_guard<std::mutex> lock(capture_diagnostic_mutex);
                capture_diagnostics=std::move(diagnostic);
            }
            if (processed) {
                if (ar->GetPoseDiff() < 25) {
                    reconstruction.lost = false;
                    reconstruction.tracked = true;
                }
                glm::mat4 pose = ar->GetPose()[OPENGL_CAMERA];
                reconstruction.scene.renderer->camera.SetTransformation(pose);
                if (anchors) {
                    reconstruction.scene.UpdateAnchors(ar->GetActiveAnchors(), movez + 5.0f);
                }
                if (!facemode)
                    reconstruction.scene.UpdateFrustum(ar->GetView());
                pos = reconstruction.scene.renderer->camera.position;

                float maxDiff = 25;
                bool frameRunning = false;
                if (reconstruction.TryLockFrame(frameRunning, ar->IsFaceMode())) {
                    // The UI can change the desired state without the binder. Admit
                    // a frame from one snapshot so capture/validation/submission agree.
                    // The worker rechecks pause after replay before integrating this frame.
                    reconstruction.frame_capture_started = std::chrono::steady_clock::now();
                    glm::mat4 lastMatrix = reconstruction.frame_calibration * reconstruction.frame_viewmat;
                    Image* lastFrame = 0;
                    if (reconstruction.frame_image) {
                        lastFrame = reconstruction.frame_image;
                    }

                    reconstruction.frame_calibration = ar->GetProjection();
                    reconstruction.frame_distortion = ar->GetDistortion();
                    reconstruction.frame_image = (frameRunning || ar->IsFaceMode())
                            ? ar->GetImage() : nullptr;
                    reconstruction.frame_pose = ar->GetPose();
                    reconstruction.frame_timestamp = std::chrono::duration<double>(
                            std::chrono::steady_clock::now() - reconstruction.start).count();
                    reconstruction.frame_viewmat = ar->GetView();

                    if (!ar->IsFaceMode()) {

                        if (frameRunning &&
                            (!reconstruction.frame_image || !reconstruction.frame_image->IsValid())) {
                            reconstruction.frame_points.clear();
                            reconstruction.BinderUnlock();
                        } else if (ar->GetPoseDiff() < maxDiff) {
                            if (frameRunning) {
                                reconstruction.event_mutex_.lock();
                                sprintf(reconstruction.pose_feedback, "");
                                reconstruction.event_mutex_.unlock();
                            }

                            reconstruction.frame_sparse = ar->GetMode() == ARCoreService::HUAWEI_SFM;
                            if (frameRunning) {
#if SCANNER_MODERN
                                ar->ConfigureDepthTest(reconstruction.depth_test_runtime.Enabled(),reconstruction.depth_test_runtime.Generation());
#endif
                                reconstruction.frame_points = ar->GetPointCloud(maxDiff);
                                reconstruction.frame_depth_test = ar->TakeDepthTestFrame();

                                int scale = glm::max(1, reconstruction.frame_image->GetWidth() / 360);
                                if (reconstruction.request_image) {
                                    delete reconstruction.request_image;
                                    reconstruction.request_image = 0;
                                }

                                if (reconstruction.frame_points.empty()) {
                                    reconstruction.BinderUnlock();
                                } else if (reconstruction.pose_correction && lastFrame) {

                                    //create requested image for pose correction
                                    reconstruction.request_image = reconstruction.frame_image->Downscale(scale);
                                    reconstruction.request_distance = 0.015f;
                                    reconstruction.AddPoses();
                                    int w = reconstruction.request_image->GetWidth();
                                    int h = reconstruction.request_image->GetHeight();
                                    if (!reconstruction.renderer) {
                                        reconstruction.renderer = new GLRenderer();
                                        reconstruction.renderer->Init(reconstruction.scene.renderer->width, reconstruction.scene.renderer->height, w, h);
                                    }

                                    //update projective texture
                                    if (reconstruction.request_newprojection) {
                                        if (reconstruction.scene.projectTexture >= 0) {
                                            Image temp(255, 255, 255, 255);
                                            temp.SetTexture(reconstruction.scene.projectTexture);
                                            temp.UpdateTexture();
                                            temp.SetTexture(reconstruction.scene.projectDepth);
                                            temp.UpdateTexture();
                                        }
                                        reconstruction.renderer->Rtt(true);
                                        reconstruction.scene.BindDepthShader();
                                        OnDrawScan(pos, true);
                                        reconstruction.renderer->Rtt(false);
                                        Image* temp = reconstruction.renderer->ReadRtt();
                                        reconstruction.scene.projectDepth = GLSL::Image2GLTexture(temp, false);
                                        reconstruction.scene.projectTexture = GLSL::Image2GLTexture(lastFrame, false);
                                        reconstruction.scene.projectMatrix = lastMatrix;
                                        delete temp;
                                    }

                                    //pose correction
                                    glClearColor(1, 1, 1, 1);
                                    reconstruction.renderer->Rtt(true);
                                    reconstruction.scene.BindMixedShader();
                                    glViewport(0, 0, w, h);
                                    OnDrawScan(pos, true);
                                    reconstruction.renderer->Rtt(false);
                                    if (reconstruction.rendered_image) {
                                        delete reconstruction.rendered_image;
                                    }
                                    reconstruction.rendered_image = reconstruction.renderer->ReadRtt(0, 0, w, h);
                                    glClearColor(0, 0, 0, 1);
                                    reconstruction.renderer->Rtt(true);
                                    reconstruction.scene.BindDepthShader();
                                    glViewport(0, 0, w, h);
                                    OnDrawScan(pos, true);
                                    reconstruction.renderer->Rtt(false);
                                    if (reconstruction.rendered_depth) {
                                        delete reconstruction.rendered_depth;
                                    }
                                    reconstruction.rendered_depth = reconstruction.renderer->ReadRtt(0, 0, w, h);
                                    reconstruction.Start(Reconstruction::POSE_CORRECTION);
                                } else {
                                    reconstruction.Start(Reconstruction::RECONSTRUCTION);
                                }
                            } else {
                                reconstruction.Start(Reconstruction::DUMMY);
                            }
                        } else {
                            if (reconstruction.tracked) {
                                reconstruction.jumped = true;
                                reconstruction.event_mutex_.lock();
                                sprintf(reconstruction.pose_feedback, "MT_JUMP");
                                reconstruction.event_mutex_.unlock();
                            }
                            reconstruction.BinderUnlock();
                        }
                    } else {
                        reconstruction.BinderUnlock();
                    }

                    if (lastFrame) {
                        delete lastFrame;
                    }
                }
            } else {
#if SCANNER_MODERN
                if(!reconstruction.lost)reconstruction.depth_test_runtime.Invalidate();
#endif
                reconstruction.lost = true;
            }
            reconstruction.scene.renderer->camera.projection = ar->GetProjection();
        }
        if (facemode) {
            glEnable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
        } else {
            glEnable(GL_DEPTH_TEST);
            glEnable(GL_CULL_FACE);
            glCullFace(GL_BACK);
        }
        if (zoomable) {
            glm::vec4 move = reconstruction.scene.renderer->camera.GetTransformation() * glm::vec4(0, 0, movez, 0);
            reconstruction.scene.renderer->camera.position += glm::vec3(move.x, move.y, move.z);
        }

        //render virtual objects (static meshes, frustum, selection, grid...)
        reconstruction.scene.Render(gyro && zoomable);
        if (grid) {
            if (ar && zoomable) {
                reconstruction.scene.RenderGrid(glm::vec3((int)pos.x, pos.y - 2, (int)pos.z), 20, 0x808000);
            } else if (!gyro) {
                bool floorplan = false;
                for (Mesh& m : reconstruction.scene.static_meshes_) {
                    if (m.image) {
                        int separator = 0;
                        std::string name = m.image->GetName();
                        for (int i = 0; i < name.size(); i++) {
                            if (name[i] == '/') {
                                separator = i;
                            }
                        }
                        if (strcmp(name.substr(separator).c_str(), "/floorplan.png") == 0) {
                            floorplan = true;
                        }
                    }
                }
                if (!facemode && !floorplan) {
                    int z = lastOrbit > 0 ? lastOrbit : lastMovez;
                    int c = glm::max(0, 128 - abs(z));
                    int color = c * 256 + c * 65536;
                    reconstruction.scene.RenderGrid(glm::vec3((int)lastMovex, lowest - 0.001f, (int)lastMovey), 40, color);
                }
            }
        }

        //render scene
        glEnable(GL_BLEND);
        glEnable(GL_STENCIL_TEST);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glStencilFunc(GL_ALWAYS, 1, 0xFF);
        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
        reconstruction.scene.BindMixedShader();
        bool output = OnDrawScan(reconstruction.scene.renderer->camera.position, zoomable);
        glDisable(GL_BLEND);

        //camera preview
        if (arActive && ((viewmode == 1) || (viewmode == 2))) {
            if (!reconstruction.scene.showNormals) {
                glStencilFunc(GL_EQUAL, 1, 0xFF);
                glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
                ar->RenderCamera(ARCoreCamera::NONE);
            }
            glStencilFunc(GL_NOTEQUAL, 1, 0xFF);
            ar->RenderCamera(viewmode == 1 ? ARCoreCamera::GRAYSCALE : ARCoreCamera::DEPTH_INV);
        }
        if (arActive && (viewmode == 0)) {
            glDisable(GL_DEPTH_TEST);
            int w = reconstruction.scene.renderer->width;
            int h = reconstruction.scene.renderer->height;
            glViewport(0, 5 * h / 6, w / 6, h / 6);
            ar->RenderCamera();
            glViewport(5 * w / 6, 5 * h / 6, w / 6, h / 6);
            ar->RenderCamera(ARCoreCamera::DEPTH_INV, 4);
            glViewport(0, 0, w, h);
            glEnable(GL_DEPTH_TEST);
        }
        reconstruction.render_mutex_.unlock();

#ifndef NDEBUG
        int error = glGetError();
        if (error > 0) {
            LOGI("GLERROR %d", error);
        }
#endif
        return output;
    }

    bool App::OnDrawScan(glm::vec3 pos, bool zoomable) {
        // Retain mesh buffers through the GL draw, not through the camera wait
        // and readback preceding it. The scene lock is already held by callers.
        std::lock_guard<std::mutex> scanLock(reconstruction.scan_mutex_);
        bool output = !reconstruction.scene.static_meshes_.empty();
        ScanVisibility visibility;
        if(!GetScanVisibility(reconstruction.scan.Resolution(),&pos[0],visibility))return output;
        GLSL::CurrentShader()->UniformFloat("u_uniformBegin", visibility.fadeBegin);
        GLSL::CurrentShader()->UniformFloat("u_uniformFactor", visibility.fadeFactor);
        GLSL::CurrentShader()->UniformVec3("u_uniformCamera", pos.x, pos.y, pos.z);
        reconstruction.scene.renderer->PrepareRender();
        int rangeMin[3];
        int rangeMax[3];
        bool rebuild = visible_scan_revision != reconstruction.scan.Revision();
        for (int i = 0; i < 3; i++) {
            rangeMin[i] = visibility.minimum[i];
            rangeMax[i] = visibility.maximum[i];
            if (!rebuild) {
                rebuild = (rangeMin[i] != visible_scan_min[i]) || (rangeMax[i] != visible_scan_max[i]);
            }
        }
        if (rebuild) {
            visible_scan.clear();
            for (const std::pair<const GridIndex, Tango3DR_Mesh*>& segment : reconstruction.scan.Data()) {
                bool visible = true;
                for (int i = 0; i < 3; i++) {
                    visible &= segment.first.indices[i] >= rangeMin[i]
                            && segment.first.indices[i] <= rangeMax[i];
                }
                if (visible && segment.second->num_vertices > 0 && segment.second->num_faces > 0) {
                    visible_scan.push_back(segment.second);
                }
            }
            visible_scan_revision = reconstruction.scan.Revision();
            for (int i = 0; i < 3; i++) {
                visible_scan_min[i] = rangeMin[i];
                visible_scan_max[i] = rangeMax[i];
            }
        }
        if (!reconstruction.lost || zoomable) {
            for (Tango3DR_Mesh* segment : visible_scan) {
                reconstruction.scene.renderer->RenderPrepared(&segment->vertices[0][0], &segment->normals[0][0], 0,
                                                              (unsigned int*)&segment->colors[0][0],
                                                              segment->num_faces * 3, &segment->faces[0][0]);
                output = true;
            }
        }
        return output;
    }

    void App::OnToggleButtonClicked(bool t3dr_is_running) {
#if SCANNER_MODERN
        if(!t3dr_is_running)reconstruction.depth_test_runtime.Invalidate();
#endif
        // The worker retains exclusive ownership of the volume. Pause/resume is
        // only a desired-running flag and must not block the UI behind replay.
        reconstruction.t3dr_is_running_.store(t3dr_is_running);
        if (t3dr_is_running) {
            reconstruction.capture_backoff.Reset();
            int expected = 2;
            if (reconstruction.recovery_state.compare_exchange_strong(expected, 0)) {
                std::lock_guard<std::mutex> eventLock(reconstruction.event_mutex_);
                if (reconstruction.event_ == "Reconstruction unavailable; retry scanning")
                    reconstruction.event_.clear();
            }
            expected = 3;
            reconstruction.recovery_state.compare_exchange_strong(expected, 0);
        }
    }

    bool App::OnClearButtonClicked() {
#if SCANNER_MODERN
        reconstruction.depth_test_runtime.Invalidate();
#endif
        reconstruction.BinderLock();
        reconstruction.render_mutex_.lock();
        if (!reconstruction.texturize.Clear(reconstruction.dataset)) {
            reconstruction.write_failed.store(true);
            reconstruction.render_mutex_.unlock();
            reconstruction.BinderUnlock();
            return false;
        }
        if (ar) ar->Clear();
        reconstruction.frame_index = -1;
        reconstruction.committed_frames = 0;
        reconstruction.recovery_state.store(0);
        reconstruction.capture_backoff.Reset();
        reconstruction.preview_cache_last = -2;
        reconstruction.preview_history.clear();
        reconstruction.scan.Clear();
        reconstruction.request_newprojection = true;
        for (Mesh& m : reconstruction.scene.static_meshes_)
            m.Destroy();
        reconstruction.scene.static_meshes_.clear();

        reconstruction.event_mutex_.lock();
        sprintf(reconstruction.pose_feedback, "");
        reconstruction.event_mutex_.unlock();

        reconstruction.write_failed.store(false);
        reconstruction.history_failed.store(false);
        reconstruction.render_mutex_.unlock();
        reconstruction.BinderUnlock();
        return true;
    }

    void App::OnUndoButtonClicked(bool fromUser, bool texturize) {
        reconstruction.Undo(fromUser, texturize);
    }

    void App::OnUndoPreviewUpdate(int frames) {
        reconstruction.PreviewChange(frames);
    }

    bool App::ResumeARIfActive() {
        // Keep offline GL work available while backgrounded, but never reopen
        // or process the camera until the Activity explicitly resumes.
        if (!ar || !activity_resumed.load() || offline_export.load()) return false;
        if (reconstruction.paused) {
            ar->OnResume();
            reconstruction.paused = false;
        }
        return true;
    }

    void App::OnPause() {
        activity_resumed.store(false);
#if SCANNER_MODERN
        reconstruction.depth_test_runtime.Invalidate();
#endif
        reconstruction.t3dr_is_running_.store(false);
        // Serialize with camera processing, without waiting on scan replay.
        std::lock_guard<std::mutex> renderLock(reconstruction.render_mutex_);
        if (ar && !reconstruction.paused) {
            ar->OnPause();
            reconstruction.paused = true;
        }
    }

    void App::Extract(std::string path, int mode) {
        reconstruction.BinderLock();
        reconstruction.render_mutex_.lock();
        if (mode == -100) {
            ExporterFloorplan floorplan;
            floorplan.SetCallback(AnalyseCallback);
            floorplan.Process(reconstruction.dataset, path);
        } else if (mode == -200) {

            //get orientation
            float yaw = glm::radians(reconstruction.dataset->ReadYaw());
            float s = glm::sin(-yaw);
            float c = glm::cos(-yaw);
            glm::vec3 p, v;

            //export pointcloud
            std::vector<Mesh> mesh = reconstruction.scan.Export();
            for (Mesh& m : mesh) {
                for (unsigned int i = 0; i < m.vertices.size(); i++) {
                    p = m.vertices[i];
                    v = m.vertices[i];
                    v.x = p.x * s - p.z * c;
                    v.z = p.x * c + p.z * s;
                    m.vertices[i] = v;
                }
                m.indices.clear();
                m.MirrorZ();
                m.SwapYZ();
            }
            File3d(path, true).WriteModel(mesh);
        }
        reconstruction.render_mutex_.unlock();
        reconstruction.BinderUnlock();
    }

    std::string App::GetEvent() {
        std::string message=reconstruction.GetEvent();
        if(reconstruction.coverage_preview.load()) {
            if(!message.empty())message+='\n';
            message+="Coverage preview; full detail when processed";
        }
#if SCANNER_MODERN
        if(reconstruction.depth_test_runtime.Enabled()) {
            if(!message.empty())message+='\n';
            message+=reconstruction.depth_test_runtime.Status();
        }
#endif
        return message;
    }

    void App::SetExperimentalDepth(bool enabled) {
#if SCANNER_MODERN
        reconstruction.depth_test_runtime.SetEnabled(enabled);
#endif
    }

    void App::SetCoveragePreview(bool enabled) {
        reconstruction.coverage_preview_requested.store(enabled);
    }

    bool App::FinishCapture() { return reconstruction.FinishCapture(); }

    std::string App::GetExperimentalDepthStatus() {
#if SCANNER_MODERN
        return reconstruction.depth_test_runtime.Status();
#else
        return "TPU test: unavailable in legacy build";
#endif
    }

    std::string App::GetCaptureDiagnostics() {
        std::lock_guard<std::mutex> lock(capture_diagnostic_mutex);
        return capture_diagnostics.empty()?"Capture calibration not available":capture_diagnostics;
    }

    int App::GetRecoveryState() const {
        return reconstruction.recovery_state.load();
    }

    void App::SetEvent(std::string value) {
        reconstruction.event_mutex_.lock();
        reconstruction.texturize.SetEvent(value);
        reconstruction.event_mutex_.unlock();
    }

    bool App::Load(std::string filename) {
        reconstruction.BinderLock();
        reconstruction.render_mutex_.lock();

        File3d io(filename, false);
        io.ReadModel(INT_MAX, reconstruction.scene.static_meshes_);
        if (io.GetType() == TYPE::PLY) {
            for (Mesh& m : reconstruction.scene.static_meshes_) {
                m.SwapYZ();
                m.MirrorZ();
            }
        }

        int count = 0;
        glm::vec3 min(9999), max(-9999);
        for (Mesh& m : reconstruction.scene.static_meshes_) {
            count += m.vertices.size();
            for (glm::vec3& v : m.vertices) {
                if (min.x > v.x) min.x = v.x;
                if (min.y > v.y) min.y = v.y;
                if (min.z > v.z) min.z = v.z;
                if (max.x < v.x) max.x = v.x;
                if (max.y < v.y) max.y = v.y;
                if (max.z < v.z) max.z = v.z;
            }
            if (io.GetType() != TYPE::PLY) {
                m.GenerateFaceNormals();
            }
        }
        lastMovex = (min.x + max.x) * 0.5f;
        lastMovez = (min.y + max.y) * 0.5f;
        lastMovey = (min.z + max.z) * 0.5f;
        lowest = min.y;
        LOGI("Loaded model with %d vertices", count);
        reconstruction.render_mutex_.unlock();
        reconstruction.BinderUnlock();
        return count > 0;
    }

    void App::Optimize(std::string filename) {
        reconstruction.BinderLock();
        reconstruction.render_mutex_.lock();
        Optimizer().Process(filename);
        reconstruction.render_mutex_.unlock();
        reconstruction.BinderUnlock();
    }

    bool App::Save(std::string filename) {
        if(!reconstruction.PrepareFullResolution())return false;
        reconstruction.BinderLock();
        reconstruction.render_mutex_.lock();
        int count = 0;
        if (ar && ar->IsFaceMode()) {
            Mesh face = ar->GetFace();
            count = face.vertices.size();
            face.image = reconstruction.frame_image;

            std::string name = filename.substr(0, filename.size() - 4) + "_face.jpg";
            face.image->SetName(name);
            face.image->Write(name);

            reconstruction.scene.static_meshes_.clear();
            reconstruction.scene.static_meshes_.push_back(face);
            File3d(filename, true).WriteModel(reconstruction.scene.static_meshes_);
        } else {
            for (Mesh& m : reconstruction.scene.static_meshes_)
                m.Destroy();
            reconstruction.scene.static_meshes_.clear();

            reconstruction.lost = false;
            sprintf(reconstruction.pose_feedback, "");
            reconstruction.scene.static_meshes_ = reconstruction.scan.Export();
            bool valid = true;
            for (Mesh& m : reconstruction.scene.static_meshes_) {
                if (!m.Reindex()) {
                    valid = false;
                    break;
                }
                m.GenerateFaceNormals();
                count += m.vertices.size();
            }
            if (!valid) {
                reconstruction.scene.static_meshes_.clear();
                reconstruction.render_mutex_.unlock();
                reconstruction.BinderUnlock();
                return false;
            }
            if (count == 0) {
                reconstruction.scene.static_meshes_.clear();
                reconstruction.render_mutex_.unlock();
                reconstruction.BinderUnlock();
                return false;
            }
            File3d output(filename, true);
            if (!output.WriteModel(reconstruction.scene.static_meshes_)) {
                reconstruction.scene.static_meshes_.clear();
                reconstruction.render_mutex_.unlock();
                reconstruction.BinderUnlock();
                return false;
            }

            if (ar) ar->Clear();
            reconstruction.scan.Clear();
            for (Mesh& m : reconstruction.scene.static_meshes_)
                m.Destroy();
            reconstruction.scene.static_meshes_.clear();

            if (count > 0) {
                ExporterCSVPoses().Process(reconstruction.dataset, reconstruction.dataset->GetPath() + "/posesOBJ.csv", false);
                ExporterCSVPoses().Process(reconstruction.dataset, reconstruction.dataset->GetPath() + "/posesPLY.csv", true);
            }
        }
        reconstruction.render_mutex_.unlock();
        reconstruction.BinderUnlock();
        return count != 0;
    }

    bool App::SaveWithTextures(std::string filename) {
        reconstruction.BinderLock();
        struct BinderRelease {
            Reconstruction& owner;
            ~BinderRelease() { owner.BinderUnlock(); }
        } binderRelease{reconstruction};
        try {
            std::lock_guard<std::mutex> renderLock(reconstruction.render_mutex_);
            // Stage every name before mutating live images. Cleanup uses a no-allocation
            // swap and in-place flip, so failed writes/allocation cannot strand editor state.
            struct TextureState {
                Image* image;
                std::string name;
                bool flip;
                bool renamed = false;
                bool flipped = false;
                TextureState(Image* value, const std::string& target)
                    : image(value), name(target), flip(value->GetExtension() == "jpg") {}
                ~TextureState() {
                    if (flipped) image->UpsideDown();
                    if (renamed) image->SwapName(name);
                }
            };
            std::vector<std::unique_ptr<TextureState>> textures;
            int index = 0;
            for (Mesh& mesh : reconstruction.scene.static_meshes_) {
                if (!mesh.imageOwner) continue;
                if (!mesh.image) return false;
                std::ostringstream target;
                target << filename.substr(0, filename.size() - 4) << "_" << index++ << ".png";
                textures.emplace_back(new TextureState(mesh.image, target.str()));
            }
            bool success = true;
            for (const auto& texture : textures) {
                texture->image->SwapName(texture->name);
                texture->renamed = true;
                if (texture->flip) {
                    texture->image->UpsideDown();
                    texture->flipped = true;
                }
                const bool written = texture->image->Write(texture->image->GetName());
                success = written && success;
            }
            const bool modelWritten = File3d(filename, true).WriteModel(reconstruction.scene.static_meshes_);
            return modelWritten && success;
        } catch (const std::exception& failure) {
            LOGE("Unable to save textured model: %s", failure.what());
            return false;
        } catch (...) {
            LOGE("Unable to save textured model");
            return false;
        }
    }

    void App::OnResume() {
        activity_resumed.store(true);
    }

    void App::SetTextureParams(int detail, int res, int count) {
        reconstruction.BinderLock();
        reconstruction.texturize.SetTextureParams(detail, res, count);
        export_texture_size = res;
        export_texture_count = count;
        reconstruction.BinderUnlock();
    }

    std::string App::GetTexturingError() {
        std::lock_guard<std::mutex> result(texturing_result_mutex);
        return texturing_error;
    }

    bool App::Texturize(std::string input, std::string output, bool poisson, bool twoPass) {
        struct BinderGuard {
            Reconstruction& value;
            explicit BinderGuard(Reconstruction& r) : value(r) { value.BinderLock(); }
            ~BinderGuard() { value.BinderUnlock(); }
        } binder(reconstruction);
        std::unique_lock<std::mutex> render(reconstruction.render_mutex_);
        std::lock_guard<std::mutex> result(texturing_result_mutex);
        struct ContextGuard {
            Reconstruction& value;
            ~ContextGuard() {
                value.texturize.ResetContext();
                std::lock_guard<std::mutex> status(value.event_mutex_);
                value.texturize.SetEvent("");
            }
        } context{reconstruction};
        struct OfflineGuard {
            std::atomic<bool>& active;
            ~OfflineGuard() { active.store(false); }
        } offlineGuard{offline_export};
        texturing_error = "The input model or output path is invalid.";
        struct FailureLog {
            const std::string& message;
            ~FailureLog() { if (!message.empty()) LOGE("MODEL_EXPORT_FAILED: %s", message.c_str()); }
        } failureLog{texturing_error};
        struct DatasetArtifactGuard {
            std::vector<std::string> files;
            std::string staged;
            bool published = false;
            ~DatasetArtifactGuard() {
                if (!published) for (const std::string& file : files) remove(file.c_str());
                if (!staged.empty()) remove(staged.c_str());
            }
        } artifacts;
        try {
        if (!reconstruction.dataset || input.empty() || output.empty() || input == output) return false;
        struct stat inputInfo, outputInfo;
        if (stat(input.c_str(), &inputInfo) == 0 && stat(output.c_str(), &outputInfo) == 0
                && inputInfo.st_dev == outputInfo.st_dev && inputInfo.st_ino == outputInfo.st_ino) return false;
        texturing_error = "The captured dataset has no readable frames or valid camera calibration.";
        if (!reconstruction.texturize.UpdatePoses(reconstruction.dataset)
                || reconstruction.texturize.GetLatestIndex(reconstruction.dataset) < 0) return false;
        reconstruction.lost = false;
        reconstruction.scan.Clear();

        texturing_error = "The model could not be prepared for texturing.";
        bool streamed = false;
        std::string workingOutput = output;
#if SCANNER_MODERN
        if (!poisson && !twoPass) {
            // Keep the requested destination untouched until both texturing and
            // orientation validation succeed. Texture resources have unique
            // names and are retained only once the final OBJ is published.
            artifacts.staged = output + ".texturing-XXXXXX.obj";
            int stage = mkstemps(&artifacts.staged[0], 4);
            if (stage < 0) {
                artifacts.staged.clear();
                texturing_error = "Could not create export working files. Check available storage.";
                return false;
            }
            if (close(stage) != 0) return false;
            workingOutput = artifacts.staged;
            DatasetTextureSettings options;
            options.textureSize = export_texture_size;
            options.textureCount = export_texture_count;
            options.scratchDirectory = export_scratch_directory;
            DatasetTextureReport report;
            // Export owns the immutable dataset through the binder, but must
            // not hold the camera/render lock through minutes of offline I/O.
            // In particular Activity.onPause must remain responsive.
            offline_export.store(true);
            if (ar && !reconstruction.paused) {
                ar->OnPause();
                reconstruction.paused = true;
            }
            render.unlock();
            bool exported = ExportDatasetTexturedObj(input, workingOutput, reconstruction.dataset, options,
                    [this](const std::string& stage, double fraction) {
                        std::ostringstream progress;
                        progress << stage << " " << int(fraction * 100) << "%";
                        std::lock_guard<std::mutex> status(reconstruction.event_mutex_);
                        reconstruction.texturize.SetEvent(progress.str());
                        return true;
                    }, report);
            artifacts.files.swap(report.artifacts);
            if (!exported) {
                texturing_error = report.error.empty() ? "The captured images could not texture this model." : report.error;
                return false;
            }
            LOGI("MODEL_TEXTURED: preserved %llu faces, textured %llu faces; atlas=%d pages at %d, photo_tile=%dx%d, photo_scale=%.3f",
                 static_cast<unsigned long long>(report.faces),
                 static_cast<unsigned long long>(report.observedFaces), report.atlasPages,
                 options.textureSize, report.tileWidth, report.tileHeight, report.photoScale);
            streamed = true;
        } else if (stat(input.c_str(), &inputInfo) == 0 && inputInfo.st_size > 32LL * 1024 * 1024) {
            texturing_error = "This model is too large for Poisson reconstruction or Photos analysis. Turn those options off in Postprocessing and retry.";
            return false;
        }
#endif
        if (!streamed) {
        if (poisson) {
            if (!reconstruction.InitTexturing(input, true)) return false;
#if SCANNER_MODERN
            // Modern extraction requires observed faces, including preparation passes.
            if (!reconstruction.texturize.ApplyFrames(reconstruction.dataset)) return false;
#endif
            // Poisson mutates its argument. Work on the destination to retain the source on failure.
            if (!reconstruction.texturize.Process(output, false, false)) return false;
            reconstruction.texturize.SetEvent("POISSON");
            Poisson().Process(output);
            input = output;
            reconstruction.texturize.SetEvent("");
        }

        //texturize
        if (twoPass) {
            if (!reconstruction.InitTexturing(input, true)) return false;
#if SCANNER_MODERN
            if (!reconstruction.texturize.ApplyFrames(reconstruction.dataset)) return false;
#endif
            if (!reconstruction.texturize.Process(output, true, false)) return false;
#if !SCANNER_MODERN
            // Validate all source images/poses before the legacy analysis pass reads them.
            if (!reconstruction.InitTexturing(output, false)
                    || !reconstruction.texturize.ApplyFrames(reconstruction.dataset)) return false;
#endif

            std::vector<int> frames;
            {
                oc::Texturize texturize;
                texturize.SetCallback(AnalyseCallback);
                texturize.Process(reconstruction.dataset, output, false);
                SetEvent("");
                for (int i : texturize.GetFrames()) {
                    frames.push_back(i);
                }
            }

            if (!reconstruction.InitTexturing(output, false)) return false;
            if (!reconstruction.texturize.ApplyFrames(reconstruction.dataset, frames)
                    || !reconstruction.texturize.Process(output, true, poisson)) return false;
        } else {
            if (!reconstruction.InitTexturing(input, false)) return false;
            if (!reconstruction.texturize.ApplyFrames(reconstruction.dataset)
                    || !reconstruction.texturize.Process(output, true, poisson)) return false;
        }
        }

        texturing_error = "The textured model could not be validated or written. Check available storage.";
        // Stream into a sibling temporary file, replacing the OBJ only after complete validation.
        // Legacy compass mapping intentionally includes the scanner's extra quarter turn.
        const double yaw = glm::radians(static_cast<double>(reconstruction.dataset->ReadYaw()));
        if (!std::isfinite(yaw)) return false;
        const double s = std::sin(-yaw), c = std::cos(-yaw);
        std::ifstream source(workingOutput);
        if (!source) return false;
        std::string temporary = output + ".reorient-XXXXXX";
        std::vector<char> name(temporary.begin(), temporary.end());
        name.push_back(0);
        int fd = mkstemp(name.data());
        if (fd < 0) return false;
        struct RewriteGuard {
            const char* name;
            FILE* file;
            ~RewriteGuard() { if (file) fclose(file); remove(name); }
        } rewrite{name.data(), fdopen(fd, "w")};
        if (!rewrite.file) { close(fd); return false; }
        size_t vertices = 0, normals = 0, coords = 0, faces = 0;
        size_t maximumIndex[3] = {};
        std::string line;
        while (std::getline(source, line)) {
            std::istringstream fields(line);
            fields.imbue(std::locale::classic());
            std::string kind;
            fields >> kind;
            if (kind == "v" || kind == "vn") {
                double x, y, z;
                if (!(fields >> x >> y >> z) || !std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) return false;
                double rx = x * s - z * c, rz = x * c + z * s;
                if (!std::isfinite(rx) || !std::isfinite(rz)) return false;
                std::string suffix;
                std::getline(fields, suffix);
                if (!suffix.empty() && !std::isspace(static_cast<unsigned char>(suffix[0])) && suffix[0] != '#') return false;
                std::ostringstream rotated;
                rotated.imbue(std::locale::classic());
                rotated << std::setprecision(std::numeric_limits<double>::max_digits10)
                        << kind << ' ' << rx << ' ' << y << ' ' << rz << suffix;
                line = rotated.str();
                if (kind == "v") ++vertices;
                else ++normals;
            } else if (kind == "vt") {
                double u;
                if (!(fields >> u) || !std::isfinite(u)) return false;
                // OBJ texture coordinates have one to three components.
                std::string token;
                int count = 1;
                while (fields >> token) {
                    if (token[0] == '#') break;
                    std::istringstream number(token);
                    number.imbue(std::locale::classic());
                    if (!(number >> u) || !number.eof() || !std::isfinite(u) || ++count > 3) return false;
                }
                ++coords;
            } else if (kind == "f") {
                int count = 0;
                std::string index;
                while (fields >> index) {
                    if (index[0] == '#') break;
                    const size_t available[3] = {vertices, coords, normals};
                    const char* part = index.c_str();
                    for (int component = 0; component < 3; ++component) {
                        // v//vn is valid, but an absent vertex or trailing slash is not.
                        if (component == 1 && *part == '/') { ++part; continue; }
                        char* end = nullptr;
                        errno = 0;
                        long value = strtol(part, &end, 10);
                        if (errno == ERANGE || end == part || value == 0 || (*end && *end != '/')) return false;
                        if (value < 0) {
                            if (static_cast<unsigned long>(-(value + 1)) >= available[component]) return false;
                        } else if (static_cast<unsigned long>(value) > maximumIndex[component]) {
                            maximumIndex[component] = static_cast<size_t>(value);
                        }
                        if (!*end) break;
                        if (component == 2) return false;
                        part = end + 1;
                    }
                    ++count;
                }
                if (count < 3) return false;
                ++faces;
            }
            line += '\n';
            if (fwrite(line.data(), 1, line.size(), rewrite.file) != line.size()) return false;
        }
        if (source.bad() || !vertices || !faces || maximumIndex[0] > vertices
                || maximumIndex[1] > coords || maximumIndex[2] > normals) return false;
        if (fflush(rewrite.file) != 0 || fsync(fileno(rewrite.file)) != 0) return false;
        int closed = fclose(rewrite.file);
        rewrite.file = nullptr;
        if (closed != 0 || rename(name.data(), output.c_str()) != 0) return false;
        artifacts.published = true;
        LOGI("MODEL_EXPORT_DONE: vertices=%zu faces=%zu", vertices, faces);
        texturing_error.clear();
        return true;
        } catch (const std::bad_alloc&) {
            texturing_error = "Not enough working memory to finish this model.";
            return false;
        } catch (const std::exception& failure) {
            LOGE("Texturing failed: %s", failure.what());
            return false;
        } catch (...) {
            LOGE("Texturing failed");
            return false;
        }
    }

    float App::GetDistance(float x1, float y1, float x2, float y2) {
        reconstruction.BinderLock();
        reconstruction.render_mutex_.lock();
        glm::mat4 matrix = reconstruction.scene.renderer->camera.projection * reconstruction.scene.renderer->camera.GetView();
        glm::vec3 a = selector.Transform(reconstruction.scene.static_meshes_, matrix, x1, y1);
        glm::vec3 b = selector.Transform(reconstruction.scene.static_meshes_, matrix, x2, y2);
        reconstruction.render_mutex_.unlock();
        reconstruction.BinderUnlock();
        return glm::length(a - b);
    }

    float App::GetFloorLevel(float x, float y, float z) {
        reconstruction.BinderLock();
        reconstruction.render_mutex_.lock();
        float output = INT_MAX;
        glm::vec3 p = glm::vec3(x, z, y);
        for (unsigned int i = 0; i < reconstruction.scene.static_meshes_.size(); i++) {
            float value = reconstruction.scene.static_meshes_[i].GetFloorLevel(p);
            if (value < INT_MIN + 1000)
                continue;
            if (output > value)
                output = value;
        }
        if (output > INT_MAX - 1000)
            output = INT_MIN;
        reconstruction.render_mutex_.unlock();
        reconstruction.BinderUnlock();
        return output;
    }

    void App::Backup() {
        EditorBackup backup;
        for (Mesh& m : reconstruction.scene.static_meshes_) {
            backup.mesh.push_back(m);
        }
        backups.push_back(backup);
        if (backups.size() > 3) {
            backups.erase(backups.begin());
        }
    }

    void App::Restore() {
        reconstruction.render_mutex_.lock();
        if (!backups.empty()) {
            int index = backups.size() - 1;
            reconstruction.scene.static_meshes_.clear();
            for (Mesh& m : backups[index].mesh) {
                reconstruction.scene.static_meshes_.push_back(m);
            }
            backups.erase(backups.begin() + index);
        }
        reconstruction.render_mutex_.unlock();
    }

    void App::ApplyEffect(Effector::Effect effect, float value, int axis) {
        reconstruction.render_mutex_.lock();
        if (effect == Effector::CLONE)
            Backup();
        if (effect == Effector::DELETE)
            Backup();
        if (effect == Effector::MOVE)
            Backup();
        if (effect == Effector::ROTATE)
            Backup();
        if (effect == Effector::SCALE)
            Backup();
        editor.ApplyEffect(reconstruction.scene.static_meshes_, effect, value, axis);
        reconstruction.scene.vertex = reconstruction.scene.TexturedVertexShader();
        reconstruction.scene.fragment = reconstruction.scene.TexturedFragmentShader();
        reconstruction.render_mutex_.unlock();
    }

    void App::PreviewEffect(Effector::Effect effect, float value, int axis) {
        reconstruction.render_mutex_.lock();
        std::string vs = reconstruction.scene.TexturedVertexShader();
        std::string fs = reconstruction.scene.TexturedFragmentShader();
        editor.PreviewEffect(vs, fs, effect, axis);
        reconstruction.scene.vertex = vs;
        reconstruction.scene.fragment = fs;
        reconstruction.scene.uniform = value / 255.0f;
        reconstruction.render_mutex_.unlock();
    }

    void App::ShowNormals(bool on) {
        reconstruction.scene.showNormals = on;
    }

    void App::ApplySelection(float x, float y, bool triangle) {
        reconstruction.render_mutex_.lock();
        glm::mat4 matrix = reconstruction.scene.renderer->camera.projection * reconstruction.scene.renderer->camera.GetView();
        Backup();
        if (triangle)
          selector.SelectTriangle(reconstruction.scene.static_meshes_, matrix, x, y);
        else
          selector.SelectObject(reconstruction.scene.static_meshes_, matrix, x, y);
        glm::vec3 center = selector.GetCenter(reconstruction.scene.static_meshes_);
        editor.SetCenter(center);
        reconstruction.scene.uniformPos = center;
        reconstruction.render_mutex_.unlock();
    }

    void App::CompleteSelection(bool inverse) {
        reconstruction.render_mutex_.lock();
        Backup();
        selector.CompleteSelection(reconstruction.scene.static_meshes_, inverse);
        glm::vec3 center = selector.GetCenter(reconstruction.scene.static_meshes_);
        editor.SetCenter(center);
        reconstruction.scene.uniformPos = center;
        reconstruction.scene.UpdateSelected(false);
        reconstruction.render_mutex_.unlock();
    }

    void App::MultSelection(bool increase) {
        reconstruction.render_mutex_.lock();
        Backup();
        if (increase)
            selector.IncreaseSelection(reconstruction.scene.static_meshes_);
        else
            selector.DecreaseSelection(reconstruction.scene.static_meshes_);
        glm::vec3 center = selector.GetCenter(reconstruction.scene.static_meshes_);
        editor.SetCenter(center);
        reconstruction.scene.uniformPos = center;
        reconstruction.render_mutex_.unlock();
    }

    void App::CircleSelection(float x, float y, float radius, bool invert) {
        reconstruction.render_mutex_.lock();
        Backup();
        glm::mat4 matrix = reconstruction.scene.renderer->camera.projection * reconstruction.scene.renderer->camera.GetView();
        selector.SelectCircle(reconstruction.scene.static_meshes_, matrix, x, y, radius, invert);
        glm::vec3 center = selector.GetCenter(reconstruction.scene.static_meshes_);
        editor.SetCenter(center);
        reconstruction.scene.uniformPos = center;
        reconstruction.render_mutex_.unlock();
    }

    void App::RectSelection(float x1, float y1, float x2, float y2, bool invert) {
        reconstruction.render_mutex_.lock();
        Backup();
        glm::mat4 matrix = reconstruction.scene.renderer->camera.projection * reconstruction.scene.renderer->camera.GetView();
        selector.SelectRect(reconstruction.scene.static_meshes_, matrix, x1, y1, x2, y2, invert);
        glm::vec3 center = selector.GetCenter(reconstruction.scene.static_meshes_);
        editor.SetCenter(center);
        reconstruction.scene.uniformPos = center;
        reconstruction.render_mutex_.unlock();
    }

    void App::SetView(float p, float y, float mx, float my, float mz, float o, bool g) {
        orbit = o;
        pitch = p;
        yaw = y;
        gyro = g;
        movex = mx;
        movey = my;
        movez = mz;
        editor.SetPitch(pitch);
        reconstruction.scene.uniformPitch = pitch;
    }

    bool App::AnimFinished() {
        reconstruction.render_mutex_.lock();
        bool output = (fabs(lastPitch - pitch) < 0.01f) && (fabs(lastYaw - yaw) < 0.01f);
        reconstruction.render_mutex_.unlock();
        return output;
    }

    bool App::DidARJump() {
        bool output = reconstruction.jumped;
        reconstruction.jumped = false;
        return output;
    }

    bool App::DidWriteFail() {
        return reconstruction.write_failed.exchange(false);
    }

    bool App::DidHistoryFail() {
        return reconstruction.history_failed.load();
    }

    float App::GetView(int axis) {
        reconstruction.render_mutex_.lock();
        float output = 0;
        if (axis == 0) output = lastMovex;
        if (axis == 1) output = lastMovey;
        if (axis == 2) output = lastMovez;
        reconstruction.render_mutex_.unlock();
        return output;
    }

    int App::GetScanSize() {
        std::lock_guard<std::mutex> renderLock(reconstruction.render_mutex_);
        std::lock_guard<std::mutex> scanLock(reconstruction.scan_mutex_);
        return reconstruction.scan.Size();
    }

    void App::SetPhotoMode(bool on) {
        reconstruction.SetPhotoMode(on);
    }
}

std::string jbyteArray2string(JNIEnv* env, jbyteArray data)
{
    jbyte *jch = env->GetByteArrayElements( data, 0 );
    int size = env->GetArrayLength( data );
    std::string output;
    for( int i = 0; i < size; i++ )
        output.push_back( (unsigned char)jch[i] );

    env->ReleaseByteArrayElements(data,jch,0);

    return output;
}

#ifdef __cplusplus
extern "C" {
#endif

JNIEXPORT jboolean JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_onARServiceConnected(JNIEnv* env, jclass, jobject context,
        jdouble res, jdouble dmin, jdouble dmax, jint noise, jboolean holesFilling, jboolean poseCorr,
        jboolean distortion, jboolean offset, jboolean flashlight, jint mode, jboolean clearing,
        jbyteArray dataset, jbyteArray cacheDirectory) {
    return app.OnARServiceConnected(env, context, res, dmin, dmax, noise, holesFilling, poseCorr,
                                    distortion, offset, flashlight, mode, clearing, jbyteArray2string(env, dataset),
                                    jbyteArray2string(env, cacheDirectory));
}

JNIEXPORT void JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_onGlSurfaceChanged(
    JNIEnv*, jclass, jint width, jint height, jboolean fullhd) {
  app.OnSurfaceChanged(width, height, fullhd);
}

JNIEXPORT jboolean JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_onGlSurfaceDrawFrame(JNIEnv*, jclass, jboolean facemode, jfloat yaw, jint viewmode, jboolean anchors, jboolean grid, jboolean smooth) {
  return app.OnDrawFrame(facemode, yaw, viewmode, anchors, grid, smooth);
}

JNIEXPORT void JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_onToggleButtonClicked(
    JNIEnv*, jclass, jboolean t3dr_is_running) {
  app.OnToggleButtonClicked(t3dr_is_running);
}

JNIEXPORT jboolean JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_onClearButtonClicked(JNIEnv*, jclass) {
    return (jboolean) app.OnClearButtonClicked();
}

JNIEXPORT void JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_onGlSurfaceCreated(JNIEnv*, jclass) {
  app.OnSurfaceCreated();
}

JNIEXPORT void JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_onUndoButtonClicked(JNIEnv*, jclass, jboolean fromUser, jboolean texturize) {
    app.OnUndoButtonClicked(fromUser, texturize);
}


JNIEXPORT void JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_onUndoPreviewUpdate(JNIEnv *env, jclass clazz, jint frames) {
    app.OnUndoPreviewUpdate(frames);
}

JNIEXPORT void JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_onPause(JNIEnv*, jclass) {
    app.OnPause();
}

JNIEXPORT void JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_onResume(JNIEnv*, jclass) {
    app.OnResume();
}

JNIEXPORT void JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_extract(JNIEnv* env, jclass, jbyteArray path, jint mode) {
    app.Extract(jbyteArray2string(env, path), mode);
}

JNIEXPORT jboolean JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_load(JNIEnv* env, jclass, jbyteArray name) {
    return app.Load(jbyteArray2string(env, name));
}

JNIEXPORT void JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_optimize(JNIEnv* env, jclass, jbyteArray name) {
    app.Optimize(jbyteArray2string(env, name));
}

JNIEXPORT jboolean JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_save(JNIEnv* env, jclass, jbyteArray name) {
    return (jboolean)app.Save(jbyteArray2string(env, name));
}

JNIEXPORT jboolean JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_saveWithTextures(JNIEnv* env, jclass, jbyteArray name) {
    return app.SaveWithTextures(jbyteArray2string(env, name)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_getRecoveryState(JNIEnv*, jclass) {
    return app.GetRecoveryState();
}

JNIEXPORT void JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_setTextureParams(JNIEnv*, jclass, jint detail,jint res, jint count) {
    app.SetTextureParams(detail, res, count);
}

JNIEXPORT jboolean JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_texturize(JNIEnv* env, jclass, jbyteArray input, jbyteArray output, jboolean poisson, jboolean twoPass) {
  if (!input || !output) return JNI_FALSE;
  try {
    auto path = [env](jbyteArray bytes) {
      std::string value(static_cast<size_t>(env->GetArrayLength(bytes)), '\0');
      if (!value.empty()) env->GetByteArrayRegion(bytes, 0, static_cast<jsize>(value.size()), reinterpret_cast<jbyte*>(&value[0]));
      return value;
    };
    std::string source = path(input), destination = path(output);
    if (env->ExceptionCheck() || source.find('\0') != std::string::npos || destination.find('\0') != std::string::npos) return JNI_FALSE;
    return app.Texturize(source, destination, poisson, twoPass) ? JNI_TRUE : JNI_FALSE;
  } catch (...) {
    return JNI_FALSE;
  }
}

JNIEXPORT jstring JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_getTexturingError(JNIEnv* env, jclass) {
    try { return env->NewStringUTF(app.GetTexturingError().c_str()); }
    catch (...) { return nullptr; }
}

JNIEXPORT void JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_setExperimentalDepth(JNIEnv*, jclass, jboolean enabled) {
    app.SetExperimentalDepth(enabled);
}

JNIEXPORT void JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_setCoveragePreview(JNIEnv*, jclass, jboolean enabled) {
    app.SetCoveragePreview(enabled);
}

JNIEXPORT jboolean JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_finishCapture(JNIEnv*, jclass) {
    try { return app.FinishCapture()?JNI_TRUE:JNI_FALSE; }
    catch(...) { return JNI_FALSE; }
}

JNIEXPORT jstring JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_getExperimentalDepthStatus(JNIEnv* env, jclass) {
    try { return env->NewStringUTF(app.GetExperimentalDepthStatus().c_str()); }
    catch (...) { return nullptr; }
}

JNIEXPORT jstring JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_getCaptureDiagnostics(JNIEnv* env, jclass) {
    try { return env->NewStringUTF(app.GetCaptureDiagnostics().c_str()); }
    catch (...) { return nullptr; }
}

JNIEXPORT jstring JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_testExperimentalDepthRuntime(JNIEnv* env, jclass) {
#if SCANNER_MODERN
    // Called only from the debug receiver's background thread. This tests the
    // actual app-UID runtime using generated data; it never starts a camera or
    // touches the active capture/runtime configuration.
    try {
        oc::depth_test::Runtime runtime;runtime.SetEnabled(true);
        oc::depth_test::Frame frame;frame.width=160;frame.height=120;
        frame.generation=runtime.Generation();frame.minDepth=.1;frame.maxDepth=4;
        frame.depth.resize(160*120);frame.confidence.resize(160*120,.85f);
        std::vector<glm::vec4> points;points.reserve(160*120);frame.links.reserve(160*120);
        for(int y=0;y<120;++y)for(int x=0;x<160;++x) {
            uint32_t i=uint32_t(y*160+x);float depth=2.f+float((x*37+y*17)%29-14)*.0007f;
            frame.depth[i]=depth;glm::vec4 point(x*.01f,y*.01f,depth,1);
            points.push_back(point);oc::depth_test::Link link;
            link.pixel=link.point=i;link.depth=depth;link.original=point;
            link.worldPerMetre=glm::dvec3(0,0,1);frame.links.push_back(link);
        }
        frame.pointCount=points.size();auto original=points;oc::depth_test::Stats stats;
        bool okay=runtime.Apply(frame,points,stats)&&stats.inferred&&stats.changed>0&&points.size()==original.size();
        for(size_t i=0;i<points.size()&&okay;++i) {
            okay=std::isfinite(points[i].z)&&std::fabs(points[i].z-original[i].z)<=.02001f&&
                 points[i].x==original[i].x&&points[i].y==original[i].y&&points[i].w==original[i].w;
        }
        std::string result=(okay?"PASS app-UID synthetic runtime: ":"FAIL app-UID synthetic runtime: ")+runtime.Status();
        return env->NewStringUTF(result.c_str());
    } catch(...) { return env->NewStringUTF("FAIL app-UID runtime exception"); }
#else
    return env->NewStringUTF("Unavailable in legacy build");
#endif
}

JNIEXPORT void JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_setView(JNIEnv*, jclass, jfloat pitch, jfloat yaw,
                                                         jfloat x, jfloat y, jfloat z, jfloat o,
                                                         jboolean gyro) {
  app.SetView(pitch, yaw, x, y, z, o, gyro);
}

extern "C"
JNIEXPORT jfloat JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_getDistance(JNIEnv*, jclass, jfloat x1,
                                                       jfloat y1, jfloat x2, jfloat y2) {
    return app.GetDistance(x1, y1, x2, y2);
}

JNIEXPORT jfloat JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_getFloorLevel(JNIEnv*, jclass, jfloat x, jfloat y, jfloat z) {
    return app.GetFloorLevel(x, y, z);
}

JNIEXPORT void JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_restore(JNIEnv*, jclass) {
    app.Restore();
}

JNIEXPORT void JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_applyEffect(JNIEnv*, jclass, jint effect, jfloat value, jint axis) {
    app.ApplyEffect((oc::Effector::Effect) effect, value, axis);
}

JNIEXPORT void JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_previewEffect(JNIEnv*, jclass, jint effect, jfloat value, jint axis) {
    app.PreviewEffect((oc::Effector::Effect) effect, value, axis);
}

JNIEXPORT void JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_applySelect(JNIEnv*, jclass, jfloat x, jfloat y, jboolean triangle) {
    app.ApplySelection(x, y, triangle);
}

JNIEXPORT void JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_completeSelection(JNIEnv*, jclass, jboolean inverse) {
    app.CompleteSelection(inverse);
}

JNIEXPORT void JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_multSelection(JNIEnv*, jclass, jboolean increase) {
    app.MultSelection(increase);
}

extern "C"
JNIEXPORT void JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_circleSelection(JNIEnv*, jclass, jfloat x, jfloat y,
                                                           jfloat radius, jboolean invert) {
    app.CircleSelection(x, y, radius, invert);
}

JNIEXPORT void JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_rectSelection(JNIEnv*, jclass, jfloat x1, jfloat y1,
                                                         jfloat x2, jfloat y2, jboolean invert) {
    app.RectSelection(x1, y1, x2, y2, invert);
}

JNIEXPORT void JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_showNormals(JNIEnv*, jclass, jboolean on) {
    app.ShowNormals(on);
}

JNIEXPORT jboolean JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_animFinished(JNIEnv*, jclass) {
    return (jboolean) app.AnimFinished();
}

JNIEXPORT jboolean JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_didARjump(JNIEnv*, jclass) {
    return (jboolean) app.DidARJump();
}

JNIEXPORT jboolean JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_didWriteFail(JNIEnv*, jclass) {
    return (jboolean) app.DidWriteFail();
}

JNIEXPORT jboolean JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_didHistoryFail(JNIEnv*, jclass) {
    return (jboolean) app.DidHistoryFail();
}

JNIEXPORT jfloat JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_getView(JNIEnv*, jclass, jint axis) {
    return app.GetView(axis);
}

JNIEXPORT jbyteArray JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_getEvent(JNIEnv* env, jclass) {
  std::string message = app.GetEvent();
  int byteCount = (int) message.length();
  const jbyte* pNativeMessage = reinterpret_cast<const jbyte*>(message.c_str());
  jbyteArray bytes = env->NewByteArray(byteCount);
  env->SetByteArrayRegion(bytes, 0, byteCount, pNativeMessage);
  return bytes;
}

JNIEXPORT void JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_setPhotoMode(JNIEnv *env, jclass clazz, jboolean on) {
    app.SetPhotoMode(on);
}

JNIEXPORT jint JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_getScanSize(JNIEnv *env, jclass clazz) {
    return app.GetScanSize();
}

JNIEXPORT jboolean JNICALL
Java_com_lvonasek_arcore3dscanner_main_JNI_isDatasetValid(JNIEnv* env, jclass, jbyteArray path) {
    oc::Dataset dataset(jbyteArray2string(env, path));
    return (jboolean) dataset.ValidateCommittedFrames();
}

#ifdef __cplusplus
}
#endif
