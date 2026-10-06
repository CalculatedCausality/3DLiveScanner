#ifndef TANGO_CALIBRATION_CACHE_H
#define TANGO_CALIBRATION_CACHE_H

#include <tango_3d_reconstruction_api.h>
#include <cmath>

namespace oc {
// Owned by one reconstruction context and accessed under its binder. Reset on
// every context replacement, even if the allocator reuses the same address.
class ColorCalibrationCache {
public:
    void Reset() { valid_ = false; }

    bool Apply(Tango3DR_ReconstructionContext context, const Tango3DR_CameraCalibration& value) {
        if (!context || !value.width || !value.height || !std::isfinite(value.fx) ||
            !std::isfinite(value.fy) || value.fx <= 0 || value.fy <= 0 ||
            !std::isfinite(value.cx) || !std::isfinite(value.cy)) return false;
        for (double coefficient : value.distortion) if (!std::isfinite(coefficient)) return false;
        bool same = valid_ && context_ == context && value.calibration_type == calibration_.calibration_type &&
                    value.width == calibration_.width && value.height == calibration_.height &&
                    value.fx == calibration_.fx && value.fy == calibration_.fy &&
                    value.cx == calibration_.cx && value.cy == calibration_.cy;
        for (int i = 0; same && i < 5; ++i) same = value.distortion[i] == calibration_.distortion[i];
        if (same) return true;
        valid_ = false;
        if (Tango3DR_ReconstructionContext_setColorCalibration(context, &value) != TANGO_3DR_SUCCESS) return false;
        context_ = context;
        calibration_ = value;
        valid_ = true;
        return true;
    }

private:
    bool valid_ = false;
    Tango3DR_ReconstructionContext context_ = nullptr;
    Tango3DR_CameraCalibration calibration_ = {};
};
}
#endif
