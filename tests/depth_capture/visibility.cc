#include <gl/scan_visibility.h>
#include <cassert>
#include <cmath>
#include <limits>
#include <iostream>
int main(){
    float p[3]={.05f,-.17f,2.05f};oc::ScanVisibility v;
    for(float resolution:{.01f,.02f,.04f}){
        assert(oc::GetScanVisibility(resolution,p,v));
        assert(v.radius==8.f&&v.fadeBegin==7.25f);
        float opaque=1.f-(2.f-v.fadeBegin)*v.fadeFactor;
        assert(opaque>1.f); // clipped shader alpha remains fully opaque at 2 m
        assert(std::fabs(1.f-(8.f-v.fadeBegin)*v.fadeFactor)<1e-6);
        double step=16.*resolution;
        for(int i=0;i<3;++i){
            assert(v.minimum[i]*step<=double(p[i])-8);
            assert((v.minimum[i]+1)*step>double(p[i])-8);
            assert(v.maximum[i]*step<=double(p[i])+8);
        }
    }
    assert(oc::GetScanVisibility(.08f,p,v)&&v.radius==10000.f);
    assert(!oc::GetScanVisibility(0,p,v));
    p[0]=std::numeric_limits<float>::infinity();assert(!oc::GetScanVisibility(.01f,p,v));
    std::cout<<"PASS preview radius independent of fine resolution, fade endpoints and intersecting chunk ownership\n";
}
