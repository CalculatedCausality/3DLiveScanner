#include <tango/calibration_cache.h>
#include <cassert>
#include <iostream>
#include <limits>
#include <vector>

static unsigned calls;
static bool fail;
Tango3DR_Status Tango3DR_ReconstructionContext_setColorCalibration(
        Tango3DR_ReconstructionContext, const Tango3DR_CameraCalibration*) {
    ++calls;
    return fail ? TANGO_3DR_ERROR : TANGO_3DR_SUCCESS;
}

int main() {
    int handles[2] = {};
    auto first = reinterpret_cast<Tango3DR_ReconstructionContext>(&handles[0]);
    auto second = reinterpret_cast<Tango3DR_ReconstructionContext>(&handles[1]);
    Tango3DR_CameraCalibration base{};
    base.width = 360; base.height = 640;
    base.fx = base.fy = 300; base.cx = 180; base.cy = 320;
    base.calibration_type = TANGO_3DR_CALIBRATION_POLYNOMIAL_3_PARAMETERS;
    oc::ColorCalibrationCache cache;
    assert(cache.Apply(first, base));
    for (int i = 0; i < 100; ++i) assert(cache.Apply(first, base));
    assert(calls == 1);
    std::vector<Tango3DR_CameraCalibration> changed;
    auto value = base; ++value.width; changed.push_back(value);
    value = base; ++value.height; changed.push_back(value);
    value = base; value.fx = std::nextafter(value.fx, std::numeric_limits<double>::infinity()); changed.push_back(value);
    value = base; ++value.fy; changed.push_back(value);
    value = base; ++value.cx; changed.push_back(value);
    value = base; ++value.cy; changed.push_back(value);
    value = base; value.calibration_type = TANGO_3DR_CALIBRATION_POLYNOMIAL_5_PARAMETERS; changed.push_back(value);
    for (int i = 0; i < 5; ++i) { value = base; value.distortion[i] = .125; changed.push_back(value); }
    for (const auto& update : changed) {
        unsigned before = calls;
        assert(cache.Apply(first, update));
        assert(cache.Apply(first, update));
        assert(calls == before + 1);
        assert(cache.Apply(first, base));
    }
    unsigned before = calls;
    assert(cache.Apply(second, base) && calls == before + 1);
    cache.Reset(); // Recreated context can reuse exactly the same handle address.
    assert(cache.Apply(second, base) && calls == before + 2);
    value = base; value.fx += 10;
    fail = true;
    assert(!cache.Apply(second, value));
    assert(!cache.Apply(second, value));
    assert(calls == before + 4);
    fail = false;
    assert(cache.Apply(second, value) && calls == before + 5);
    value.fx = std::numeric_limits<double>::quiet_NaN();
    assert(!cache.Apply(second, value) && calls == before + 5);
    value = base; value.distortion[4] = std::numeric_limits<double>::infinity();
    assert(!cache.Apply(second, value) && calls == before + 5);
    assert(!cache.Apply(nullptr, base) && calls == before + 5);
    value = base; value.width = 0;
    assert(!cache.Apply(second, value) && calls == before + 5);
    std::cout << "PASS: calibration cache coalesces identical values, detects every field change, retries failures, and resets reused contexts\n";
}
