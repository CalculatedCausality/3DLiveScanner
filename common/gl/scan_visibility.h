// SPDX-License-Identifier: Apache-2.0
#ifndef SCANNER_SCAN_VISIBILITY_H
#define SCANNER_SCAN_VISIBILITY_H
#include <cmath>
#include <climits>

namespace oc {
struct ScanVisibility {
    float radius=8.f,fadeBegin=7.25f,fadeFactor=1.f/.75f;
    int minimum[3]{},maximum[3]{};
};
inline bool GetScanVisibility(float resolution,const float* position,ScanVisibility& out) {
    if(!position||!std::isfinite(resolution)||resolution<=0)return false;
    const double step=double(resolution)*16;
    // Retain the old coarse-survey full-range mode, but no longer make fine
    // scanning invisible after 1.8 m merely because its voxel size is 1 cm.
    out.radius=resolution>.05f?10000.f:8.f;
    out.fadeBegin=out.radius-.75f;out.fadeFactor=1.f/.75f;
    for(int i=0;i<3;++i) {
        if(!std::isfinite(position[i]))return false;
        double low=std::floor((double(position[i])-out.radius)/step);
        double high=std::floor((double(position[i])+out.radius)/step);
        if(low<INT_MIN||low>INT_MAX||high<INT_MIN||high>INT_MAX)return false;
        // Include owners whose positive face intersects the visible region.
        out.minimum[i]=int(low);out.maximum[i]=int(high);
    }
    return true;
}
}
#endif
