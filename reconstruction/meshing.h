// Copyright 2026 3DLiveScanner contributors. SPDX-License-Identifier: Apache-2.0
// Extraction-private methods, included inside Extractor.
// Conservative cell-local remeshing of the Freudenthal surface. Every vertex
// and segment on a cell face is retained, including face-diagonal crossings.
// There are no cross-cell decisions, new samples, or changes to topology.
    void reducedCell(const Index* p, const Voxel* const* v) {
        static const int tets[6][4] = {{0,1,3,7},{0,3,2,7},{0,2,6,7},
                                      {0,6,4,7},{0,4,5,7},{0,5,1,7}};
        // A body diagonal that does not cross strictly inside the cell cannot
        // supply a removable interior vertex. Keep the inexpensive legacy path.
        if ((v[0]->sdf < 0) == (v[7]->sdf < 0) || v[0]->sdf == 0 || v[7]->sdf == 0) {
            for (const auto& tet : tets) tetra(p,v,tet);
            return;
        }
        const double h = context.config.values[Resolution];
        int edgeIds[8][8];
        for (auto& row : edgeIds) std::fill(row,row+8,-1);
        int ends[19][2], nv = 0, nf = 0;
        glm::dvec3 points[19], normal(0);
        Face original[12];
        auto edge = [&](int a, int b) {
            if (b < a) std::swap(a,b);
            if (v[a]->sdf == 0) b = a;
            else if (v[b]->sdf == 0) a = b;
            int& id = edgeIds[a][b];
            if (id >= 0) return uint32_t(id);
            id = nv++; ends[id][0] = a; ends[id][1] = b;
            // Use precisely the public float positions and canonical endpoint
            // order of vertex(), including for negative coordinates.
            if (p[b] < p[a]) std::swap(a,b);
            double t = a == b ? 0 : clamp(double(v[a]->sdf)/(double(v[a]->sdf)-v[b]->sdf),0,1);
            points[id] = glm::dvec3(glm::vec3(((1-t)*position(p[a])+t*position(p[b]))*h));
            return uint32_t(id);
        };
        auto face = [&](uint32_t a, uint32_t b, uint32_t c, const glm::dvec3& outward) {
            if (a == b || b == c || a == c) return;
            glm::dvec3 n = glm::cross(points[b]-points[a],points[c]-points[a]);
            if (glm::dot(n,n) < 1e-24) return;
            if (glm::dot(n,outward) < 0) { std::swap(b,c); n = -n; }
            original[nf++] = Face{{a,b,c}}; normal += n;
        };
        for (const auto& tet : tets) {
            int in[4], out[4], ni = 0, no = 0;
            glm::dvec3 positive(0), negative(0);
            for (int j : tet) {
                if (v[j]->sdf < 0) { in[ni++] = j; negative += position(p[j]); }
                else { out[no++] = j; positive += position(p[j]); }
            }
            if (!ni || !no) continue;
            glm::dvec3 outward = positive/double(no)-negative/double(ni);
            if (ni == 1) face(edge(in[0],out[0]),edge(in[0],out[1]),edge(in[0],out[2]),outward);
            else if (no == 1) face(edge(out[0],in[0]),edge(out[0],in[1]),edge(out[0],in[2]),outward);
            else {
                uint32_t a = edge(in[0],out[0]), b = edge(in[0],out[1]);
                uint32_t c = edge(in[1],out[1]), d = edge(in[1],out[0]);
                face(a,b,c,outward); face(a,c,d,outward);
            }
        }
        Face replacement[12]; int replacementCount = 0;
        unsigned masks[19] = {};
        for (int i = 0; i < nv; ++i) for (int axis = 0; axis < 3; ++axis) {
            int a = ends[i][0], b = ends[i][1];
            if (((a>>axis)&1) == ((b>>axis)&1)) masks[i] |= 1u<<(2*axis+((a>>axis)&1));
        }
        bool reduced = reducePolygon(points,masks,nv,original,nf,normal,h,replacement,replacementCount);
        if (reduced && context.config.values[GenerateColor])
            reduced = reductionColors(v,ends,points,nv,replacement,replacementCount,normal);
        if (reduced) {
            for (int i = 0; i < replacementCount; ++i) original[i] = replacement[i];
            nf = replacementCount;
        }
        uint32_t ids[19]; std::fill(ids,ids+19,UINT32_MAX);
        for (int i = 0; i < nf; ++i) {
            const Face& f = original[i];
            for (uint32_t id : f) if (ids[id] == UINT32_MAX) {
                int a = ends[id][0], b = ends[id][1];
                // vertex() expects a crossing edge, not an equal-endpoint key.
                if (a == b) {
                    int other = 0;
                    while ((v[other]->sdf < 0) == (v[a]->sdf < 0)) ++other;
                    ids[id] = vertex(p[a],p[other],v[a],v[other]);
                } else ids[id] = vertex(p[a],p[b],v[a],v[b]);
            }
            triangle(ids[f[0]],ids[f[1]],ids[f[2]],
                glm::cross(points[f[1]]-points[f[0]],points[f[2]]-points[f[0]]));
        }
        // Retain the removed interior vertex's component-filter evidence. A
        // smaller triangulation must not erase a previously accepted component.
        if (reduced) ++vertices[ids[original[0][0]]].support;
    }
    bool reductionColors(const Voxel* const* v, const int ends[19][2], const glm::dvec3* p,
                         int nv, const Face* facesIn, int nf, const glm::dvec3& normal) const {
        bool colored = false;
        for (int i = 0; i < 8; ++i) colored |= v[i]->color_weight > 0;
        if (!colored) return true; // exactly the same white on every vertex
        glm::dvec3 color[19];
        for (int i = 0; i < nv; ++i) {
            int a = ends[i][0], b = ends[i][1];
            // ends[] uses numeric corner order. Reverse a mixed-axis order to
            // match vertex()'s lexicographic lattice ordering before rounding.
            int orderA = ((a&1)<<2)+(a&2)+((a&4)>>2);
            int orderB = ((b&1)<<2)+(b&2)+((b&4)>>2);
            if (orderB < orderA) std::swap(a,b);
            double t = a == b ? 0 : clamp(double(v[a]->sdf)/(double(v[a]->sdf)-v[b]->sdf),0,1);
            double wa = (1-t)*v[a]->color_weight, wb = t*v[b]->color_weight;
            for (int j = 0; j < 3; ++j) color[i][j] = clamp(std::round(wa+wb > 0 ?
                (wa*v[a]->color[j]+wb*v[b]->color[j])/(wa+wb) : 255),0,255);
        }
        // Fit an affine RGB field over the common projection using the largest
        // candidate triangle. If ALL old vertices have residual range <= one
        // exported RGB unit/channel, both triangulations do as well. This bounds
        // interpolation differences without blurring a color-only feature.
        int chosen = 0; double area = 0;
        for (int i = 0; i < nf; ++i) {
            const Face& f = facesIn[i];
            double candidate = glm::dot(glm::cross(p[f[1]]-p[f[0]],p[f[2]]-p[f[0]]),normal);
            if (candidate > area) { area = candidate; chosen = i; }
        }
        if (area <= 0) return false;
        const Face& f = facesIn[chosen];
        glm::dvec3 ab = p[f[1]]-p[f[0]], ac = p[f[2]]-p[f[0]], low(0), high(0);
        for (int i = 0; i < nv; ++i) {
            glm::dvec3 q = p[i]-p[f[0]];
            double u = glm::dot(glm::cross(q,ac),normal)/area;
            double w = glm::dot(glm::cross(ab,q),normal)/area;
            glm::dvec3 residual = color[i]-color[f[0]]-u*(color[f[1]]-color[f[0]])-w*(color[f[2]]-color[f[0]]);
            for (int j = 0; j < 3; ++j) {
                low[j] = std::min(low[j],residual[j]); high[j] = std::max(high[j],residual[j]);
                if (high[j]-low[j] > 1) return false;
            }
        }
        return true;
    }
    bool reducePolygon(const glm::dvec3* p, const unsigned* masks, int nv, const Face* facesIn, int nf,
                       glm::dvec3 normal, double h, Face* out, int& count) const {
        if (nf < 4) return false;
        double length = glm::length(normal);
        if (length < 1e-12) return false;
        normal /= length;
        // Both old and new patches must be single-valued graphs over the SAME
        // simple projected polygon. If every vertex is inside a slab of width
        // 0.01 voxel, their piecewise-linear interiors are too. Matching points
        // along the slab normal proves a symmetric Hausdorff bound of 0.01*h.
        // This is a geometry bound, not an untested quadric-error heuristic.
        double low = 0, high = 0;
        for (int i = 1; i < nv; ++i) {
            double distance = glm::dot(p[i]-p[0],normal);
            low = std::min(low,distance); high = std::max(high,distance);
        }
        if (high-low > .01*h) return false;
        int uses[19][19] = {};
        for (int i = 0; i < nf; ++i) {
            const Face& f = facesIn[i];
            if (glm::dot(glm::cross(p[f[1]]-p[f[0]],p[f[2]]-p[f[0]]),normal) <= 1e-16*h*h) return false;
            for (int j = 0; j < 3; ++j) ++uses[f[j]][f[(j+1)%3]];
        }
        int next[19]; std::fill(next,next+19,-1);
        int first = -1, boundaryCount = 0;
        for (int a = 0; a < nv; ++a) for (int b = 0; b < nv; ++b) {
            if (uses[a][b] > 1) return false;
            if (uses[a][b] && !uses[b][a]) {
                if (next[a] >= 0) return false;
                next[a] = b; first = a; ++boundaryCount;
            }
        }
        if (first < 0 || boundaryCount > 12 || boundaryCount-2 >= nf) return false;
        int ring[12], n = 0, at = first;
        do {
            if (at < 0 || n >= boundaryCount) return false;
            ring[n++] = at; at = next[at];
        } while (at != first);
        if (n != boundaryCount || nv != n+1) return false;
        // Find a boundary-vertex fan whose triangles all have positive projected
        // area. Together with the original disk this certifies an injective
        // projection: a consistently oriented triangular disk with simple
        // boundary is a graph. Refuse any diagonal on a shared cell face.
        double best = std::numeric_limits<double>::infinity(); int root = -1;
        for (int start = 0; start < n; ++start) {
            double cost = 0;
            for (int step = 1; step+1 < n; ++step) {
                int a = ring[start], b = ring[(start+step)%n], c = ring[(start+step+1)%n];
                if ((step > 1 && (masks[a]&masks[b])) || (step+2 < n && (masks[a]&masks[c]))) {
                    cost = std::numeric_limits<double>::infinity(); break;
                }
                glm::dvec3 ab = (p[b]-p[a])/h, ac = (p[c]-p[a])/h;
                glm::dvec3 actual = glm::cross(p[b]-p[a],p[c]-p[a]);
                // Same public-float degeneracy test as triangle(). Otherwise a
                // valid projected fan could lose one tiny triangle on output.
                if (glm::dot(actual,actual) < 1e-24) { cost = std::numeric_limits<double>::infinity(); break; }
                double area = glm::dot(glm::cross(ab,ac),normal);
                if (area <= 1e-10) { cost = std::numeric_limits<double>::infinity(); break; }
                double perimeter = glm::dot(ab,ab)+glm::dot(ac,ac)+glm::dot(ac-ab,ac-ab);
                cost += perimeter*perimeter/area;
            }
            if (cost < best) { best = cost; root = start; }
        }
        if (root < 0) return false;
        // Boundary simplicity is checked explicitly, including collinear
        // non-adjacent segments. Do not turn a folded contour into a flat disk.
        for (int i = 0; i < n; ++i) for (int j = i+2; j < n; ++j) {
            if (i == 0 && j == n-1) continue;
            glm::dvec3 a = p[ring[i]], b = p[ring[(i+1)%n]], c = p[ring[j]], d = p[ring[(j+1)%n]];
            double ac = glm::dot(glm::cross(b-a,c-a),normal), ad = glm::dot(glm::cross(b-a,d-a),normal);
            double ca = glm::dot(glm::cross(d-c,a-c),normal), cb = glm::dot(glm::cross(d-c,b-c),normal);
            if (ac*ad <= 0 && ca*cb <= 0) return false;
        }
        count = 0;
        for (int step = 1; step+1 < n; ++step)
            out[count++] = Face{{uint32_t(ring[root]),uint32_t(ring[(root+step)%n]),uint32_t(ring[(root+step+1)%n])}};
        return true;
    }
