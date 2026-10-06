// Copyright 2026 3DLiveScanner contributors. SPDX-License-Identifier: Apache-2.0
// Paged-only per-ray node batching, promoted from generation_fusion.cc.in.
// Preserve each voxel's original weight/color update order and work ticks.
// RAM uses the original path: batching was slower in the target RAM workload.
namespace recon {
void integrate(Transaction& tx, const float* point, const Pose& camera,
               const Tango3DR_ImageBuffer* image, const Pose& color_pose) {
    if (!tx.context.pager) { integrateDirect(tx,point,camera,image,color_pose); return; }
    const double* cfg = tx.context.config.values;
    double h = cfg[Resolution], mu = 3*h;
    glm::dvec3 p(point[0], point[1], point[2]);
    glm::dvec3 surface = camera.world(p);
    double range = glm::length(p);
    glm::dvec3 ray = camera.q * (p / range);
    double axial_mu = mu;
    glm::dvec3 rgb;
    bool has_color = image && cfg[GenerateColor] && sample(*image, tx.context.color,
        color_pose, surface, cfg[Rectify] != 0, rgb);

    struct Pending {
        Index node;
        double sdf = 0;
        double weights[16];
        uint16_t colored = 0;
        unsigned count = 0;
        bool valid = false, create = false;
    } pending[8];
    auto flush = [&](Pending& item) {
        if (!item.valid) return;
        Voxel* v = tx.writable(item.node,item.create);
        if (v) for (unsigned i = 0; i < item.count; ++i) {
            double weight = item.weights[i];
            double total = v->weight + weight;
            v->sdf = float((v->sdf*v->weight + item.sdf*weight)/total);
            v->weight = float(std::min(cfg[MaxWeight],total));
            if (item.colored & (uint16_t(1) << i)) {
                total = v->color_weight + weight;
                for (int j = 0; j < 3; ++j) v->color[j] = float((v->color[j]*v->color_weight + rgb[j]*weight)/total);
                v->color_weight = float(std::min(cfg[MaxWeight],total));
            }
        }
        item.valid = false; item.create = false; item.colored = 0; item.count = 0;
    };

    double start = cfg[Clearing] ? 0 : std::max(0.0,range-mu);
    double end = range+mu;
    const double step = h*.5;
    for (double t = start; t <= end; t += step) {
        tx.tick();
        glm::dvec3 q = (camera.t + ray*t)/h;
        Index base(int(std::floor(q.x)),int(std::floor(q.y)),int(std::floor(q.z)));
        bool in_band = t >= range-mu;
        if (!in_band && !hasChunkSupport(tx,base)) {
            double distance = std::min(range-mu-t,nextChunkSupportBoundary(q,base,ray,h));
            double steps = std::max(1.0,std::floor(distance/step));
            t += (steps-1)*step;
            continue;
        }
        for (int z = 0; z <= 1; ++z) for (int y = 0; y <= 1; ++y) for (int x = 0; x <= 1; ++x) {
            tx.tick();
            Index n(base.x+x,base.y+y,base.z+z);
            double weight = point[3] * (x ? q.x-base.x : 1-(q.x-base.x)) *
                (y ? q.y-base.y : 1-(q.y-base.y)) * (z ? q.z-base.z : 1-(q.z-base.z));
            if (weight < 1e-6) continue;
            // Each 2x2x2 support cube has unique coordinate parity. A straight
            // ray cannot return to a node displaced by a parity collision.
            unsigned slot = (uint32_t(n.x)&1u) | ((uint32_t(n.y)&1u)<<1) | ((uint32_t(n.z)&1u)<<2);
            Pending& item = pending[slot];
            if (item.valid && (item.node.x != n.x || item.node.y != n.y || item.node.z != n.z || item.count == 16))
                flush(item);
            if (!item.valid) {
                if (!in_band) {
                    const Voxel* old = tx.read(n);
                    if (!old || !old->weight) continue;
                }
                double sdf = (p.z - camera.local(position(n)*h).z)/axial_mu;
                if (sdf < -1) continue;
                item.node = n; item.sdf = std::min(1.0,sdf); item.valid = true;
            }
            item.create = item.create || in_band;
            item.weights[item.count] = weight;
            if (in_band && has_color) item.colored |= uint16_t(1) << item.count;
            ++item.count;
        }
    }
    for (Pending& item : pending) flush(item);
}
} // namespace recon
