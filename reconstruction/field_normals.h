// Copyright 2026 3DLiveScanner contributors. SPDX-License-Identifier: Apache-2.0
// Extraction-private, included in namespace recon after the context definition.
// World-lattice gradients, independent of the requesting segment's triangles.
struct FieldNormals {
    struct Sample { float sdf, weight; };
    Sample values[19*19*19]; // 54,872 bytes; no owning pointers or allocations
    // Read-only field snapshot; cache exact double gradients only for this
    // extraction. 0=not evaluated, 1=usable gradient, 2=missing axis support.
    mutable double gradients[17*17*17][3];
    mutable uint8_t gradient_status[17*17*17] = {};
    Index origin;
    double minimum;
    FieldNormals(const _Tango3DR_ReconstructionContext& context, const Index& key)
        : origin(key.x*16,key.y*16,key.z*16), minimum(context.config.values[MinSurfaceWeight]) {
        std::fill(values,values+19*19*19,Sample{0,0});
        // First stream the twelve negative-face neighbors, then the ordinary
        // eight geometry chunks. Only ONE page pin is held while copying, so
        // even the eight-chunk minimum remains sufficient. The last eight
        // reads warm precisely the geometry halo pinned by chunk() afterwards.
        // Double-negative corner chunks cannot supply an axial difference.
        for (int pass = 0; pass < 2; ++pass)
            for (int z = -1; z <= 1; ++z) for (int y = -1; y <= 1; ++y) for (int x = -1; x <= 1; ++x) {
                int negatives = (x < 0)+(y < 0)+(z < 0);
                if ((pass == 0 && negatives != 1) || (pass == 1 && negatives != 0)) continue;
                Index at(key.x+x,key.y+y,key.z+z);
                PagePin pin;
                const Chunk* chunk = nullptr;
                if (context.pager) {
                    auto found = context.paged_volume.find(at);
                    if (found != context.paged_volume.end()) {
                        pin.set(*found->second); chunk = pin.record->data;
                    }
                } else {
                    auto found = context.volume.find(at);
                    if (found != context.volume.end()) chunk = found->second.get();
                }
                if (!chunk) continue;
                const int lo[3] = {x < 0 ? -1 : 16*x,y < 0 ? -1 : 16*y,z < 0 ? -1 : 16*z};
                const int hi[3] = {x < 0 ? -1 : x ? 17 : 15,y < 0 ? -1 : y ? 17 : 15,z < 0 ? -1 : z ? 17 : 15};
                for (int k = lo[2]; k <= hi[2]; ++k) for (int j = lo[1]; j <= hi[1]; ++j) for (int i = lo[0]; i <= hi[0]; ++i) {
                    const Voxel& voxel = chunk->voxels[(i-16*x)+16*(j-16*y)+256*(k-16*z)];
                    values[(i+1)+19*(j+1)+361*(k+1)] = Sample{voxel.sdf,voxel.weight};
                }
            }
    }
    bool observed(const Sample& s) const { return s.weight >= minimum && std::isfinite(s.sdf); }
    bool gradient(const Index& node, glm::dvec3& result) const {
        int x = node.x-origin.x+1, y = node.y-origin.y+1, z = node.z-origin.z+1;
        assert(x >= 1 && x <= 17 && y >= 1 && y <= 17 && z >= 1 && z <= 17);
        const int cached = (x-1)+17*(y-1)+289*(z-1);
        if (!gradient_status[cached]) {
            glm::dvec3 computed(0);
            const bool valid = computeGradient(x+19*y+361*z,computed);
            if (valid) for (int j=0;j<3;++j) gradients[cached][j]=computed[j];
            gradient_status[cached]=valid?1:2;
        }
        if (gradient_status[cached]!=1) return false;
        for (int j=0;j<3;++j) result[j]=gradients[cached][j];
        return true;
    }
    bool computeGradient(int index, glm::dvec3& result) const {
        const Sample& center = values[index];
        if (!observed(center)) return false;
        static const int stride[3] = {1,19,361};
        for (int j = 0; j < 3; ++j) {
            const Sample& left = values[index-stride[j]];
            const Sample& right = values[index+stride[j]];
            bool lo = observed(left), hi = observed(right);
            if (lo && hi) result[j] = .5*(double(right.sdf)-left.sdf);
            else if (hi) result[j] = double(right.sdf)-center.sdf;
            else if (lo) result[j] = double(center.sdf)-left.sdf;
            else return false;
        }
        return true;
    }
    bool edge(const Index& a, const Index& b, double t, glm::dvec3& normal) const {
        normal = glm::dvec3(0);
        glm::dvec3 ga(0), gb(0), candidate(0);
        // Exact-zero vertices use only that lattice node, regardless of which
        // incident edge first asks for the welded vertex.
        if (t == 0) { if (!gradient(a,candidate)) return false; }
        else if (t == 1) { if (!gradient(b,candidate)) return false; }
        else {
            if (!gradient(a,ga) || !gradient(b,gb)) return false;
            candidate = (1-t)*ga+t*gb;
        }
        double squared = glm::dot(candidate,candidate);
        if (squared > 0 && std::isfinite(squared)) { normal = candidate; return true; }
        // Missing axis evidence or a stationary field has no defensible field
        // direction. Retain the old oriented geometric normal accumulation.
        normal = glm::dvec3(0);
        return false;
    }
};
