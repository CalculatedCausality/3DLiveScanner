// The same harness is compiled against the captured pre-edit and current Retango.
// Only Image's owned-pixel lifetime is supplied here; every geometry/estimation
// operation runs production retango.cc, GLM, geometry_validation and Delaunay.
#include <tango_3d_reconstruction_api.h>
#include "data/file3d.h"
#include "gl/opengl.h"
#define private public
#include "tango/retango.h"
#undef private
#include <cassert>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>

namespace oc {
Image::Image(int w, int h) : instances(0), width(w), height(h),
    data(w > 0 && h > 0 ? new unsigned char[w * h * 4]() : nullptr), texture(-1) {}
Image::~Image() { delete[] data; }
}

namespace {
using oc::Retango;
using Points = std::vector<glm::vec4>;
size_t checkpoints = 0;

template<typename T> void bytes(std::ofstream& out, const T& value) {
    out.write(reinterpret_cast<const char*>(&value), sizeof(value));
}
template<typename T> void points(std::ofstream& out, const std::vector<T>& values) {
    bytes(out, uint64_t(values.size()));
    for (const auto& v : values)
        for (int j = 0; j < v.length(); ++j) bytes(out, v[j]);
}
void snapshot(std::ofstream& out, Retango& r, bool projected = false) {
    // Do not compare the intentionally deferred cache until it has been consumed.
    points(out, r.GET());
    points(out, r.output); // Exact camera-space values consumed by Android PCL().
    points(out, r.input);
    points(out, r.estimated);
    bytes(out, bool(r.mask));
    if (r.mask) {
        bytes(out, r.width);
        bytes(out, r.height);
        bytes(out, r.size);
        for (int i = 0; i < r.width * r.height; ++i) {
            bytes(out, r.mask[i]);
            bytes(out, r.finished[i]);
        }
    }
    if (projected) points(out, r.converted);
    ++checkpoints;
}

void pending(const Retango& r) {
#ifdef RETANGO_LAZY
    assert(!r.convertedReady);
    assert(r.converted.empty());
#endif
}
void projected(const Retango& r, const glm::mat4& addPose) {
#ifdef RETANGO_LAZY
    assert(r.convertedReady);
#endif
    assert(r.converted.size() == r.input.size());
    const glm::mat4 inverse = glm::inverse(addPose);
    for (size_t i = 0; i < r.input.size(); ++i) {
        glm::vec4 v = inverse * glm::vec4(r.input[i], 1.0f);
        v.x /= fabs(v.z * v.w);
        v.y /= fabs(v.z * v.w);
        assert(v == r.converted[i]);
    }
}

glm::mat4 pose(float translation, float angle) {
    return glm::rotate(glm::translate(glm::mat4(1), glm::vec3(translation, -.1f, .3f)),
                       angle, glm::vec3(0, 1, 0));
}
Points grid(int w, int h, const glm::mat4& camera) {
    Points result;
    result.reserve(w * h);
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        glm::vec4 p = camera * glm::vec4((x - w * .5f) * 2.f / w,
                                        (y - h * .5f) * 1.5f / h,
                                        2.f + ((x + y) % 7) * .01f, 1);
        p.w = .1f + ((x + y) % 10) * .1f;
        result.push_back(p);
    }
    return result;
}

void dense(std::ofstream& out) {
    Retango r;
    oc::Image image(640, 480), resized(321, 241);
    for (int frame = 0; frame < 4; ++frame) {
        const glm::mat4 addPose = pose(.3f * frame, .07f * frame);
        Points p = grid(160 + 40 * frame, 120, addPose);
        const size_t accepted = p.size();
        const float nan = std::numeric_limits<float>::quiet_NaN();
        p.emplace_back(nan, 0, 2, 1);
        p.emplace_back(0, 0, 2, 0);
        p.emplace_back(0, 0, 2, 1.01f);
        p.push_back(addPose * glm::vec4(0, 0, -1, 1));
        r.ADD(p, addPose, frame % 2 ? &resized : &image);
        assert(r.GET().size() == accepted && r.output.size() == accepted);
        assert(std::equal(p.begin(), p.begin() + accepted, r.merged.begin()));
        pending(r);
        snapshot(out, r);
    }
}

void sparse(std::ofstream& out) {
    oc::Image image(640, 480), resized(480, 640);
    Retango r;
    for (int frame = 0; frame < 6; ++frame) {
        const glm::mat4 addPose = pose(frame * .07f, frame * .03f);
        const glm::mat4 updatePose = pose(frame * .07f + .12f, frame * .03f + .08f);
        Points p;
        for (int y = -2; y <= 2; ++y) for (int x = -2; x <= 2; ++x) {
            glm::vec4 v = addPose * glm::vec4(x * .12f, y * .12f, 2 + .01f * x * y, 1);
            v.w = .75f;
            p.push_back(v);
        }
        oc::Image* img = frame % 2 ? &resized : &image;
        r.ADD(p, addPose, img);
        pending(r);
        snapshot(out, r);
        r.UPD(img, updatePose, frame % 2);
        assert(!r.estimated.empty()); // Delaunay must actually produce geometry.
        projected(r, addPose);
        snapshot(out, r, true);
        const Points cached = r.converted;
#ifdef RETANGO_LAZY
        // Deliberately corrupt the saved pose to prove repeated UPD uses its ready
        // cache, rather than clearing/rebuilding it (the next ADD replaces it).
        r.convertedPose = glm::mat4(0);
#endif
        r.UPD(img, pose(-.08f, -.02f), true);
        assert(r.converted == cached);
        snapshot(out, r, true);
    }

    // Two widely separated floor points bypass Delaunay and exercise pairing.
    Retango pair;
    Points p = {glm::vec4(-.8f, -.5f, 2, 1), glm::vec4(.8f, -.5f, 2, .5f)};
    pair.ADD(p, glm::mat4(1), &image);
    pair.UPD(&image, glm::mat4(1), false);
    assert(pair.estimated.size() > 10);
    snapshot(out, pair, true);
    pair.ADD(p, glm::mat4(1), &image);
    assert(pair.GET().size() > p.size()); // Estimates become public on the next ADD.
    snapshot(out, pair);

    // Exercise optional wall-estimation paths with points above/below the camera.
    Retango wall;
    Points tall = {glm::vec4(-1.2f, -1.8f, 3, 1), glm::vec4(-1.1f, .4f, 3, 1),
                   glm::vec4(1.2f, -1.8f, 3.2f, 1), glm::vec4(1.1f, .4f, 3.2f, 1)};
    wall.ADD(tall, glm::mat4(1), &image);
    wall.UPD(&image, pose(.1f, .04f), true);
    projected(wall, glm::mat4(1));
    snapshot(out, wall, true);
    wall.ADD(tall, glm::mat4(1), &image);
    snapshot(out, wall);
}

void holes(std::ofstream& out) {
    Retango r;
    oc::Image image(640, 480), resized(320, 240);
    Points p = {glm::vec4(-.8f, -.5f, 2, 1), glm::vec4(.8f, -.5f, 2, 1)};
    glm::mat4 identity(1);
    r.ADD(p, identity, &image);
    pending(r);
    // Hole filling's actual insertion kernel must be ready immediately after ADD,
    // before UPD ever materializes converted. Duplicate suppression must persist.
    r.AddVoxel(glm::vec4(0, .25f, 2, 1), identity);
    r.AddVoxel(glm::vec4(0, .25f, 2, 1), identity);
    assert(r.estimated.size() == 1);
    snapshot(out, r);
    oc::Component outer, hole;
    outer.valid = hole.valid = outer.closed = hole.closed = true;
    for (int i = 0; i < 8; ++i) {
        oc::Edge e;
        e.point[0] = glm::vec4(-.7f + .2f * i, i % 2 ? .6f : -.6f, 2, 1);
        e.point[1] = glm::vec4(-.5f + .2f * i, i % 2 ? -.6f : .6f, 2, 1);
        outer.edges.push_back(e);
    }
    const glm::vec4 square[] = {glm::vec4(-.4f, -.4f, 2, 1), glm::vec4(.4f, -.4f, 2, 1),
                                glm::vec4(.4f, .4f, 2, 1), glm::vec4(-.4f, .4f, 2, 1)};
    for (int i = 0; i < 4; ++i) {
        oc::Edge e;
        e.point[0] = square[i];
        e.point[1] = square[(i + 1) % 4];
        hole.edges.push_back(e);
    }
    r.ADD(std::vector<oc::Component>{outer, hole}, identity, false);
    snapshot(out, r);
    r.ADD(std::vector<oc::Component>{outer, hole}, identity, true);
    assert(r.estimated.size() > 1); // Component walls used finished without UPD.
    pending(r);
    snapshot(out, r);
    const size_t expected = p.size() + r.estimated.size();
    r.ADD(p, identity, &resized);
    assert(r.GET().size() == expected);
    assert(r.estimated.empty());
    r.AddVoxel(glm::vec4(0, .25f, 2, 1), identity);
    assert(r.estimated.size() == 1); // Resized/reset finished permits insertion again.
    snapshot(out, r);
    r.UPD(&resized, pose(.15f, .06f), true);
    projected(r, identity);
    snapshot(out, r, true);
}

void recovery(std::ofstream& out) {
    oc::Image image(640, 480), invalidImage(0, 0);
    Retango r;
    glm::mat4 identity(1);
    Points good = {glm::vec4(-.8f, -.5f, 2, 1), glm::vec4(.8f, -.5f, 2, 1)};
    Points bad, empty;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    bad = {glm::vec4(nan, 0, 2, 1), glm::vec4(0, 0, -1, 1),
           glm::vec4(0, 0, 0, 1), glm::vec4(0, 0, 2, 0)};
    r.ADD(bad, identity, &image); // Invalid first frame, no masks allocated yet.
    pending(r);
    snapshot(out, r);
    for (int i = 0; i < 7; ++i) {
        r.ADD(good, identity, &image);
        r.UPD(&image, identity, false);
        assert(!r.estimated.empty());
        snapshot(out, r, true);
        glm::mat4 invalid = identity;
        invalid[3][0] = nan;
        switch (i) {
            case 0: r.ADD(empty, identity, &image); break;
            case 1: r.ADD(bad, identity, &image); break;
            case 2: r.ADD(good, invalid, &image); break;
            case 3: r.ADD(good, glm::mat4(0), &image); break;
            case 4: r.ADD(good, identity, nullptr); break;
            case 5: r.ADD(good, identity, &invalidImage); break;
            case 6: r.ADD(empty, pose(.1f, .1f), &image); break;
        }
        assert(r.GET().empty() && r.output.empty() && r.input.empty());
        pending(r);
        snapshot(out, r);
        // An UPD with no accepted input must not resurrect stale projections.
        r.UPD(&image, identity, true);
        pending(r);
        snapshot(out, r);
        const size_t expected = good.size() + r.estimated.size();
        r.ADD(good, identity, &image);
        assert(r.GET().size() == expected);
        pending(r);
        snapshot(out, r);
        r.UPD(&image, identity, true);
        projected(r, identity);
        snapshot(out, r, true);
    }
    // Replace an unconsumed (dense) cache twice before entering the sparse path.
    r.ADD(good, identity, &image);
    Points moved = grid(3, 3, pose(.2f, .1f));
    r.ADD(moved, pose(.2f, .1f), &image);
    r.UPD(&image, identity, false);
    projected(r, pose(.2f, .1f));
    snapshot(out, r, true);
}

volatile size_t consumed = 0;
void benchmark(const char* name, int w, int h, int imageW, int imageH, bool update, int iterations) {
    Retango r;
    const glm::mat4 addPose = pose(.3f, .08f);
    oc::Image image(imageW, imageH);
    Points p = grid(w, h, addPose);
    auto frame = [&]() {
        r.ADD(p, addPose, &image);
        if (update) r.UPD(&image, addPose, false);
    };
    for (int i = 0; i < 8; ++i) frame();
    const clock_t cpuStart = std::clock();
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) frame();
    const auto end = std::chrono::steady_clock::now();
    const clock_t cpuEnd = std::clock();
    consumed += r.output.size();
    std::cout << name << ',' << p.size() << ',' << iterations << ',' << std::fixed
              << std::setprecision(6)
              << std::chrono::duration<double, std::micro>(end - start).count() / iterations << ','
              << 1000000.0 * (cpuEnd - cpuStart) / CLOCKS_PER_SEC / iterations << '\n';
}
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "--benchmark") == 0) {
        const int scale = argc == 3 ? std::atoi(argv[2]) : 10;
        assert(scale > 0 && scale <= 1000);
        benchmark("dense_19200_mask640x480_ADD", 160, 120, 640, 480, false, 250 * scale);
        benchmark("dense_76800_mask640x480_ADD", 320, 240, 640, 480, false, 100 * scale);
        benchmark("dense_76800_mask1920x1080_ADD", 320, 240, 1920, 1080, false, 100 * scale);
        benchmark("sparse_25_ADD_UPD", 5, 5, 640, 480, true, 3000 * scale);
        return consumed == 0;
    }
    assert(argc == 2);
    std::ofstream out(argv[1], std::ios::binary);
    assert(out.good());
    dense(out);
    sparse(out);
    holes(out);
    recovery(out);
    out.close();
    assert(out.good());
    std::cout << "PASS: " << checkpoints << " production Retango state checkpoints\n";
}
