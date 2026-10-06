// SPDX-License-Identifier: Apache-2.0
#ifndef SCANNER_CAPTURE_POLICY_H
#define SCANNER_CAPTURE_POLICY_H
#include <algorithm>
#include <cmath>
namespace oc {
inline double CapturePreviewResolution(double requested,bool coverage,bool poseCorrection,bool holes) {
    // Only the preview volume becomes coarse. Camera filtering, recorded clouds
    // and later model reconstruction retain the requested resolution policy.
    if(!std::isfinite(requested)||requested<=0||!coverage||poseCorrection||holes)return requested;
    return std::max(requested,.05);
}
}
#endif
