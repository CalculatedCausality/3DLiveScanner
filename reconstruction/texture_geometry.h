// SPDX-License-Identifier: Apache-2.0
// Private CPU texturing geometry. Camera coordinates: +X right, +Y down, +Z forward.
#ifndef SCANNER_TEXTURE_GEOMETRY_H
#define SCANNER_TEXTURE_GEOMETRY_H
#include "tango_3d_reconstruction_api.h"
#include <cstdint>
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <vector>

namespace scanner_texture {
struct V {
    double x, y, z;
    V(double a=0, double b=0, double c=0) : x(a), y(b), z(c) {}
    explicit V(const float* p) : x(p[0]), y(p[1]), z(p[2]) {}
    double operator[](int i) const { return i == 0 ? x : (i == 1 ? y : z); }
};
inline V operator+(V a,V b) { return V(a.x+b.x,a.y+b.y,a.z+b.z); }
inline V operator-(V a,V b) { return V(a.x-b.x,a.y-b.y,a.z-b.z); }
inline V operator*(V a,double s) { return V(a.x*s,a.y*s,a.z*s); }
inline double dot(V a,V b) { return a.x*b.x+a.y*b.y+a.z*b.z; }
inline V cross(V a,V b) { return V(a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x); }
inline double length(V a) { return std::sqrt(dot(a,a)); }
inline bool finite(V a) { return std::isfinite(a.x)&&std::isfinite(a.y)&&std::isfinite(a.z); }
struct Triangle {
    V p[3], n[3];
    std::array<uint8_t,4> color[3];
    double edge = 0, score = 0;
    int page=0, x=0, y=0, side=0;
};
// Median-split BVH, bounded to O(triangles) storage; two-sided segment tests.
class Visibility {
    struct Node { V lo,hi; size_t begin,end; int left=-1,right=-1; };
    std::vector<Node> nodes;
    std::vector<size_t> order;
    const std::vector<Triangle>* triangles = nullptr;
    int build(size_t begin,size_t end) {
        Node n; n.begin=begin; n.end=end;
        n.lo=V(1e100,1e100,1e100); n.hi=V(-1e100,-1e100,-1e100);
        for(size_t i=begin;i<end;++i) for(V p:(*triangles)[order[i]].p) {
            n.lo=V(std::min(n.lo.x,p.x),std::min(n.lo.y,p.y),std::min(n.lo.z,p.z));
            n.hi=V(std::max(n.hi.x,p.x),std::max(n.hi.y,p.y),std::max(n.hi.z,p.z));
        }
        int id=static_cast<int>(nodes.size()); nodes.push_back(n);
        if(end-begin>8) {
            V d=n.hi-n.lo; int axis=d.y>d.x?1:0; if(d.z>d[axis]) axis=2;
            size_t mid=(begin+end)/2;
            std::nth_element(order.begin()+begin,order.begin()+mid,order.begin()+end,
                [&](size_t a,size_t b) {
                    const Triangle& x=(*triangles)[a]; const Triangle& y=(*triangles)[b];
                    return x.p[0][axis]+x.p[1][axis]+x.p[2][axis]<y.p[0][axis]+y.p[1][axis]+y.p[2][axis];
                });
            int l=build(begin,mid),r=build(mid,end); nodes[id].left=l; nodes[id].right=r;
        }
        return id;
    }
    bool blocked(int id,V origin,V direction,size_t ignore,double limit) const {
        const Node& n=nodes[id]; double near=0,far=limit;
        for(int a=0;a<3;++a) {
            if(std::abs(direction[a])<1e-30) {
                if(origin[a]<n.lo[a]||origin[a]>n.hi[a]) return false;
            } else {
                double u=(n.lo[a]-origin[a])/direction[a],v=(n.hi[a]-origin[a])/direction[a];
                if(u>v) std::swap(u,v);
                near=std::max(near,u); far=std::min(far,v);
                if(near>far) return false;
            }
        }
        if(n.left>=0) return blocked(n.left,origin,direction,ignore,limit)||blocked(n.right,origin,direction,ignore,limit);
        for(size_t i=n.begin;i<n.end;++i) {
            if(order[i]==ignore) continue;
            const Triangle& t=(*triangles)[order[i]];
            V e1=t.p[1]-t.p[0],e2=t.p[2]-t.p[0],h=cross(direction,e2);
            double det=dot(e1,h);
            if(std::abs(det)<=1e-14*length(e1)*length(h)) continue;
            V s=origin-t.p[0]; double u=dot(s,h)/det;
            if(u< -1e-10||u>1+1e-10) continue;
            V q=cross(s,e1); double v=dot(direction,q)/det;
            if(v< -1e-10||u+v>1+1e-10) continue;
            double hit=dot(e2,q)/det;
            if(hit>1e-8&&hit<limit) return true;
        }
        return false;
    }
public:
    void init(const std::vector<Triangle>& t) {
        triangles=&t; order.resize(t.size()); std::iota(order.begin(),order.end(),0);
        nodes.reserve(t.size()*2); if(!t.empty()) build(0,t.size());
    }
    bool visible(V origin,V point,size_t face) const {
        V d=point-origin;
        // Exclude the endpoint by 10 micrometres (or 1e-7 of the ray).
        double limit=1-std::max(1e-7,1e-5/std::max(length(d),1e-5));
        return nodes.empty()||!blocked(0,origin,d,face,limit);
    }
};
struct Camera {
    V origin, row[3];
    bool init(const Tango3DR_Pose& p) {
        origin=V(p.translation[0],p.translation[1],p.translation[2]);
        double x=p.orientation[0],y=p.orientation[1],z=p.orientation[2],w=p.orientation[3];
        double norm=x*x+y*y+z*z+w*w;
        if(!finite(origin)||!std::isfinite(norm)||std::abs(norm-1)>0.01) return false;
        double s=1/std::sqrt(norm); x*=s;y*=s;z*=s;w*=s;
        // Transpose of camera-to-world quaternion rotation.
        row[0]=V(1-2*(y*y+z*z),2*(x*y+z*w),2*(x*z-y*w));
        row[1]=V(2*(x*y-z*w),1-2*(x*x+z*z),2*(y*z+x*w));
        row[2]=V(2*(x*z+y*w),2*(y*z-x*w),1-2*(x*x+y*y));
        return true;
    }
    V local(V p) const { p=p-origin; return V(dot(row[0],p),dot(row[1],p),dot(row[2],p)); }
};
inline bool project(V p,const Tango3DR_CameraCalibration& c,double& u,double& v) {
    if(p.z<=1e-5||!finite(p)) return false;
    double x=p.x/p.z,y=p.y/p.z,r=x*x+y*y;
    const double* k=c.distortion;
    double radial=1+k[0]*r+k[1]*r*r,dx=0,dy=0;
    if(c.calibration_type==TANGO_3DR_CALIBRATION_POLYNOMIAL_3_PARAMETERS) radial+=k[2]*r*r*r;
    if(c.calibration_type==TANGO_3DR_CALIBRATION_POLYNOMIAL_5_PARAMETERS) {
        radial+=k[4]*r*r*r; dx=2*k[2]*x*y+k[3]*(r+2*x*x); dy=k[2]*(r+2*y*y)+2*k[3]*x*y;
    }
    u=c.fx*(x*radial+dx)+c.cx; v=c.fy*(y*radial+dy)+c.cy;
    return std::isfinite(u)&&std::isfinite(v)&&u>=0&&v>=0&&u<=c.width-1&&v<=c.height-1;
}
} // namespace scanner_texture
#endif
