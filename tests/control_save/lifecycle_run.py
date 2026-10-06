#!/usr/bin/env python3
"""Exercise production Activity hooks, surface recreation and camera admission.

Camera/GL calls are host fakes; the GL drawing body is deliberately excluded.
This does not simulate Android camera policy or claim a device lifecycle pass.
"""
from pathlib import Path
import tempfile
from run import compile_test, definition

source = r'''
#include <atomic>
#include <cassert>
#include <future>
#include <iostream>
#include <mutex>
struct Camera {
    int pauses = 0, resumes = 0, lost = 0;
    void OnPause() { ++pauses; }
    void OnResume() { ++resumes; }
    void OnGlContextLost() { ++lost; }
};
struct Gl { void AbandonGlContext() {} };
struct Reconstruction {
    struct DepthTest { int invalidations=0; void Invalidate(){++invalidations;} } depth_test_runtime;
    std::mutex render_mutex_, binder;
    std::atomic<bool> t3dr_is_running_{true};
    bool paused = true, request_newprojection = false;
    Gl scene;
    Gl* renderer = nullptr;
    void BinderLock() { binder.lock(); }
    void BinderUnlock() { binder.unlock(); }
};
struct App {
    Reconstruction reconstruction;
    std::atomic<bool> activity_resumed{false};
    std::atomic<bool> offline_export{false};
    Camera camera;
    Camera* ar = &camera;
    void OnPause();
    void OnResume();
    void OnSurfaceCreated();
    bool ResumeARIfActive();
};
'''
for signature in ["void App::OnPause()", "void App::OnResume()", "void App::OnSurfaceCreated()",
                  "bool App::ResumeARIfActive()"]:
    source += definition("scanner/app/src/main/jni/app.cc", signature) + "\n"
source += r'''
int main() {
    App app;
    const auto draw = [&] {
        std::lock_guard<std::mutex> lock(app.reconstruction.render_mutex_);
        return app.ResumeARIfActive();
    };
    assert(!draw());
    app.OnResume();
    assert(draw());
    assert(app.camera.resumes == 1);
    // Pause must not wait for the reconstruction worker's held binder.
    app.reconstruction.BinderLock();
    auto pause = std::async(std::launch::async, [&] { app.OnPause(); });
    assert(pause.wait_for(std::chrono::milliseconds(250)) == std::future_status::ready);
    pause.get();
    app.reconstruction.BinderUnlock();
    assert(app.camera.pauses == 1 && !app.reconstruction.t3dr_is_running_.load());
    assert(app.reconstruction.depth_test_runtime.invalidations == 1);
    for (int i = 0; i < 50; ++i) assert(!draw());
    app.OnSurfaceCreated();
    assert(app.camera.lost == 1 && app.camera.resumes == 1);
    assert(app.reconstruction.paused && !draw());
    app.OnResume();
    assert(draw());
    assert(app.camera.resumes == 2);
    // Activity resume re-enables camera admission, not scan integration.
    assert(!app.reconstruction.t3dr_is_running_.load());
    app.OnPause();
    assert(!draw());
    app.offline_export.store(true);
    app.OnResume();
    assert(!draw());
    app.OnSurfaceCreated();
    assert(!draw());
    app.offline_export.store(false);
    assert(draw());
    assert(app.reconstruction.render_mutex_.try_lock());
    app.reconstruction.render_mutex_.unlock();
    std::cout << "PASS: background camera admission, surface recreation, foreground resume, pause without replay lock\n";
}
'''
with tempfile.TemporaryDirectory(prefix="scanner-lifecycle-", dir="/tmp/opencode") as temporary:
    compile_test(Path(temporary), "test", source, ["-DSCANNER_MODERN=1"])
