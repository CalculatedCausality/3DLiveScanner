#!/usr/bin/env python3
"""Exercise production preview draw/count locking with a blocked fake GL draw.

This is a concurrency/lifetime regression, not a GPU or scan-speed benchmark.
"""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
app = (ROOT / 'scanner/app/src/main/jni/app.cc').read_text()


def definition(signature):
    start = app.index(signature)
    opening = app.index('{', start)
    depth = 0
    for end in range(opening, len(app)):
        depth += (app[end] == '{') - (app[end] == '}')
        if not depth:
            return app[start:end + 1]
    raise AssertionError(signature)


source = r'''
#include <gl/scan_visibility.h>
#include <glm/glm.hpp>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <future>
#include <iostream>
#include <mutex>
#include <utility>
#include <vector>
using namespace std::chrono;
std::promise<void>* drawing = nullptr;
std::shared_future<void> releaseDraw;
struct Tango3DR_Mesh {
    float vertices[3][3] = {}, normals[3][3] = {};
    unsigned char colors[3][4] = {};
    unsigned int faces[1][3] = {{0,1,2}};
    int num_vertices=3, num_faces=1;
};
namespace oc {
struct GridIndex { int indices[3] = {}; };
struct Shader {
    void UniformFloat(const char*, float) {}
    void UniformVec3(const char*, float, float, float) {}
};
struct GLSL { static Shader* CurrentShader() { static Shader s; return &s; } };
struct Renderer {
    void PrepareRender() {}
    void RenderPrepared(float* v, float*, int, unsigned int*, int, unsigned int* f) {
        assert(v[0] == 3 && f[2] == 2);
        if (drawing) { drawing->set_value(); releaseDraw.wait(); }
        // A publisher must not free/replace these arrays while the draw uses them.
        assert(v[0] == 3 && f[2] == 2);
    }
};
struct Scan {
    std::vector<std::pair<const GridIndex, Tango3DR_Mesh*>> data;
    uint64_t revision=0;
    double Resolution() { return .05; }
    uint64_t Revision() { return revision; }
    const decltype(data)& Data() { return data; }
    int Size() { return data.size(); }
};
struct Reconstruction {
    std::mutex render_mutex_, scan_mutex_;
    Scan scan;
    bool lost=false;
    struct Scene { Renderer draw; Renderer* renderer=&draw; std::vector<int> static_meshes_; } scene;
};
struct App {
    Reconstruction reconstruction;
    uint64_t visible_scan_revision=~uint64_t(0);
    int visible_scan_min[3]={}, visible_scan_max[3]={};
    std::vector<Tango3DR_Mesh*> visible_scan;
    bool OnDrawScan(glm::vec3, bool);
    int GetScanSize();
};
// DRAW
// SIZE
}
int main() {
    oc::App app;
    auto& r=app.reconstruction;
    auto* mesh=new Tango3DR_Mesh;
    mesh->vertices[0][0]=3;
    r.scan.data.emplace_back(oc::GridIndex{},mesh);
    std::promise<void> inDraw, finishDraw, publishing;
    drawing=&inDraw; auto entered=inDraw.get_future();
    releaseDraw=finishDraw.get_future().share();
    auto draw=std::async(std::launch::async,[&] {
        std::lock_guard<std::mutex> scene(r.render_mutex_);
        return app.OnDrawScan(glm::vec3(0),true);
    });
    assert(entered.wait_for(seconds(5))==std::future_status::ready);
    auto attempted=publishing.get_future();
    auto publish=std::async(std::launch::async,[&] {
        publishing.set_value();
        std::lock_guard<std::mutex> guard(r.scan_mutex_);
        delete r.scan.data[0].second; r.scan.data.clear(); ++r.scan.revision;
    });
    attempted.wait();
    assert(publish.wait_for(milliseconds(50))==std::future_status::timeout);
    finishDraw.set_value(); assert(draw.get()); publish.get(); drawing=nullptr;
    // Cached pointers were freed: revision invalidation must rebuild before use.
    assert(!app.OnDrawScan(glm::vec3(0),true));
    assert(app.visible_scan.empty());
    // Count queries are readers too; they must wait for the publishing lock.
    std::unique_lock<std::mutex> guard(r.scan_mutex_);
    std::promise<void> counting; auto countStarted=counting.get_future();
    auto count=std::async(std::launch::async,[&] { counting.set_value(); return app.GetScanSize(); });
    countStarted.wait();
    assert(count.wait_for(milliseconds(50))==std::future_status::timeout);
    guard.unlock(); assert(count.get()==0);
    assert(r.scan_mutex_.try_lock()); r.scan_mutex_.unlock();
    assert(r.render_mutex_.try_lock()); r.render_mutex_.unlock();
    std::cout<<"PASS: draw buffer lifetime, cache invalidation, count-reader exclusion and early-return unlock\n";
}
'''
source = source.replace('// DRAW', definition('bool App::OnDrawScan('))
source = source.replace('// SIZE', definition('int App::GetScanSize()'))
with tempfile.TemporaryDirectory(prefix='scanner-preview-lock-', dir='/tmp/opencode') as tmp:
    work = Path(tmp)
    (work / 'test.cc').write_text(source)
    binary = work / 'test'
    subprocess.run(['c++', '-std=c++11', '-O1', '-g', '-pthread',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                    '-I' + str(ROOT / 'common'), '-I' + str(ROOT / 'third_party/glm'),
                    str(work / 'test.cc'), '-o', str(binary)], check=True, timeout=90)
    subprocess.run([str(binary)], check=True, timeout=20,
                   env=dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',
                            UBSAN_OPTIONS='halt_on_error=1'))
