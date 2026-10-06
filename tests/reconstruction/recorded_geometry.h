#ifndef RECORDED_GEOMETRY_H
#define RECORDED_GEOMETRY_H

#include <glm/glm.hpp>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace recorded {
using V = glm::dvec3;
struct Triangle { V a, b, c; };
inline double segmentDistance2(V p, V a, V b) {
    V d = b-a;
    double n = glm::dot(d,d);
    double t = n > 0 ? std::max(0.,std::min(1.,glm::dot(p-a,d)/n)) : 0;
    V delta = p-(a+t*d); return glm::dot(delta,delta);
}
inline double triangleDistance2(V p, const Triangle& t) {
    V ab=t.b-t.a, ac=t.c-t.a, ap=p-t.a, cross=glm::cross(ab,ac);
    if (glm::dot(cross,cross) < 1e-30)
        return std::min(segmentDistance2(p,t.a,t.b),std::min(segmentDistance2(p,t.b,t.c),segmentDistance2(p,t.c,t.a)));
    double d1=glm::dot(ab,ap), d2=glm::dot(ac,ap);
    if (d1<=0 && d2<=0) return glm::dot(ap,ap);
    V bp=p-t.b; double d3=glm::dot(ab,bp), d4=glm::dot(ac,bp);
    if (d3>=0 && d4<=d3) return glm::dot(bp,bp);
    double vc=d1*d4-d3*d2;
    if (vc<=0 && d1>=0 && d3<=0) return segmentDistance2(p,t.a,t.b);
    V cp=p-t.c; double d5=glm::dot(ab,cp), d6=glm::dot(ac,cp);
    if (d6>=0 && d5<=d6) return glm::dot(cp,cp);
    double vb=d5*d2-d1*d6;
    if (vb<=0 && d2>=0 && d6<=0) return segmentDistance2(p,t.a,t.c);
    double va=d3*d6-d5*d4;
    if (va<=0 && d4-d3>=0 && d5-d6>=0) return segmentDistance2(p,t.b,t.c);
    double denominator=va+vb+vc;
    V delta=p-(t.a+ab*(vb/denominator)+ac*(vc/denominator));
    return glm::dot(delta,delta);
}
struct Box {
    V low=V(std::numeric_limits<double>::infinity());
    V high=V(-std::numeric_limits<double>::infinity());
    void add(V p) { low=glm::min(low,p); high=glm::max(high,p); }
    double distance2(V p) const {
        V d=glm::max(glm::max(low-p,p-high),V(0)); return glm::dot(d,d);
    }
};
class BVH {
    struct Node { Box box; int left=-1,right=-1; size_t begin=0,end=0; };
    const std::vector<Triangle>& triangles;
    std::vector<size_t> indices;
    std::vector<Node> nodes;
    V center(size_t i) const { const auto& t=triangles[i]; return (t.a+t.b+t.c)/3.; }
    int build(size_t begin,size_t end) {
        int id=int(nodes.size()); nodes.emplace_back();
        Box box, centers;
        for (size_t i=begin;i<end;++i) {
            const auto& t=triangles[indices[i]];
            box.add(t.a); box.add(t.b); box.add(t.c); centers.add(center(indices[i]));
        }
        nodes[id].box=box; nodes[id].begin=begin; nodes[id].end=end;
        if (end-begin>8) {
            V extent=centers.high-centers.low;
            int axis=extent.x>=extent.y && extent.x>=extent.z ? 0 : extent.y>=extent.z ? 1 : 2;
            size_t middle=begin+(end-begin)/2;
            std::nth_element(indices.begin()+begin,indices.begin()+middle,indices.begin()+end,
                [&](size_t a,size_t b) { double x=center(a)[axis],y=center(b)[axis]; return x==y ? a<b : x<y; });
            int left=build(begin,middle), right=build(middle,end);
            nodes[id].left=left; nodes[id].right=right;
        }
        return id;
    }
    void search(int id,V p,double& best) const {
        const Node& n=nodes[id];
        if (n.box.distance2(p)>best) return;
        if (n.left<0) {
            for (size_t i=n.begin;i<n.end;++i) best=std::min(best,triangleDistance2(p,triangles[indices[i]]));
        } else {
            int a=n.left,b=n.right;
            if (nodes[a].box.distance2(p)>nodes[b].box.distance2(p)) std::swap(a,b);
            search(a,p,best); search(b,p,best);
        }
    }
public:
    explicit BVH(const std::vector<Triangle>& t):triangles(t),indices(t.size()) {
        std::iota(indices.begin(),indices.end(),0);
        nodes.reserve(t.size()/2+1);
        if (!t.empty()) build(0,t.size());
    }
    double distance(V p) const {
        double best=std::numeric_limits<double>::infinity();
        if (!nodes.empty()) search(0,p,best);
        return std::sqrt(best);
    }
};
inline void geometrySelfTest() {
    Triangle t{V(0,0,0),V(1,0,0),V(0,1,0)};
    if (std::abs(triangleDistance2(V(.25,.25,2),t)-4)>1e-12 ||
        std::abs(triangleDistance2(V(2,0,0),t)-1)>1e-12 ||
        std::abs(triangleDistance2(V(.7,.7,0),t)-.08)>1e-12)
        throw std::runtime_error("Triangle distance self-test failed");
    std::vector<Triangle> triangles;
    for (int i=0;i<32;++i) {
        V shift(i*.13,std::sin(i)*.2,std::cos(i)*.4);
        triangles.push_back(Triangle{t.a+shift,t.b+shift,t.c+shift});
    }
    BVH bvh(triangles);
    for (int i=0;i<80;++i) {
        V p(i*.06,std::cos(i*.4),std::sin(i*.31));
        double brute=std::numeric_limits<double>::infinity();
        for (const auto& tri:triangles) brute=std::min(brute,triangleDistance2(p,tri));
        if (std::abs(bvh.distance(p)-std::sqrt(brute))>1e-10)
            throw std::runtime_error("BVH distance self-test failed");
    }
}
} // namespace recorded
#endif
