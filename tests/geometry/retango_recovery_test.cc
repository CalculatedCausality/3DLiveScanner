#include <tango/retango.h>
#include <cassert>
#include <limits>
#include <iostream>

// Minimal owned pixels for the real Retango::ADD/UpdateCaches path. The test
// does not link or emulate any ARCore, Huawei or Tango SDK entry points.
namespace oc {
Image::Image(int w, int h) : instances(0), width(w), height(h),
    data(new unsigned char[w * h * 4]()), texture(-1) {}
Image::~Image() { delete[] data; }
}

int main() {
    oc::Image image(16, 16);
    oc::Retango depth;
    glm::mat4 pose(1);
    std::vector<glm::vec4> good = {glm::vec4(0, 0, 2, 1), glm::vec4(0.2f, 0.1f, 2, 0.1f)};
    depth.ADD(good, pose, &image);
    const std::vector<glm::vec4> accepted = depth.GET();
    assert(accepted.size() == 2);

    // Missing data, malformed points/pose, and a missing image are frame-local
    // rejections. Every one is followed by a real good-frame ADD on the same
    // Retango instance, with no Clear/Setup/restart in between.
    std::vector<glm::vec4> bad;
    depth.ADD(bad, pose, &image);
    assert(depth.GET().empty());
    depth.ADD(good, pose, &image);
    assert(depth.GET() == accepted);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    bad = {glm::vec4(nan, 0, 2, 1), glm::vec4(0, 0, -1, 1), glm::vec4(1, 0, 0, 1)};
    depth.ADD(bad, pose, &image);
    assert(depth.GET().empty());
    depth.ADD(good, pose, &image);
    assert(depth.GET() == accepted);

    glm::mat4 invalid = pose;
    invalid[3][0] = nan;
    depth.ADD(good, invalid, &image);
    assert(depth.GET().empty());
    depth.ADD(good, pose, &image);
    assert(depth.GET() == accepted);
    depth.ADD(good, glm::mat4(0), &image);
    assert(depth.GET().empty());
    depth.ADD(good, pose, &image);
    assert(depth.GET() == accepted);

    depth.ADD(good, pose, nullptr);
    assert(depth.GET().empty());
    depth.ADD(good, pose, &image);
    assert(depth.GET() == accepted);

    // One corrupt sample must not reject the two usable samples beside it.
    bad.insert(bad.end(), good.begin(), good.end());
    depth.ADD(bad, pose, &image);
    assert(depth.GET() == accepted);
    std::cout << "Retango bad-frame -> good-frame recovery regressions passed\n";
}
