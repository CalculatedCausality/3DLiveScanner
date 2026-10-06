#include "tango/scan.h"
#include "tango/geometry_validation.h"

#include <ctime>
#include <utility>
#include <thread/capture_backoff.h>
#include <algorithm>
#include <limits>
#if SCANNER_MODERN
#include <sys/statvfs.h>
#include <unistd.h>
#include "../../reconstruction/paging.h"
#endif

namespace oc {

    namespace {
        Tango3DR_ReconstructionContext CreateContext(double res, double dmin, double dmax,
                                                     int noise, bool clearing,
                                                     const std::string& pagingDirectory,
                                                     const PagingPlan& pagingPlan) {
            if (!std::isfinite(res) || res <= 0 || !std::isfinite(dmin) || dmin < 0 ||
                !std::isfinite(dmax) || dmax <= dmin || noise < 0) return nullptr;
            Tango3DR_Config config = Tango3DR_Config_create(TANGO_3DR_CONFIG_RECONSTRUCTION);
            if (!config) return nullptr;
            bool valid = Tango3DR_Config_setDouble(config, "resolution", res) == TANGO_3DR_SUCCESS &&
                         Tango3DR_Config_setDouble(config, "min_depth", dmin) == TANGO_3DR_SUCCESS &&
                         Tango3DR_Config_setDouble(config, "max_depth", dmax) == TANGO_3DR_SUCCESS &&
                         Tango3DR_Config_setBool(config, "generate_color", true) == TANGO_3DR_SUCCESS &&
                         Tango3DR_Config_setBool(config, "use_space_clearing", clearing) == TANGO_3DR_SUCCESS &&
                         Tango3DR_Config_setBool(config, "use_parallel_integration", true) == TANGO_3DR_SUCCESS;
            // Keep the existing optional-tuning behavior for this parameter.
            Tango3DR_Config_setInt32(config, "min_num_vertices", noise);
#if SCANNER_MODERN
            if (valid && pagingPlan.enabled)
                valid = Tango3DR_Config_setInt32(config, "max_update_chunks", pagingPlan.update_chunks) == TANGO_3DR_SUCCESS;
#endif
#ifdef TANGO
            valid = valid && Tango3DR_Config_setInt32(config, "update_method", TANGO_3DR_PROJECTIVE_UPDATE) == TANGO_3DR_SUCCESS;
#endif
            Tango3DR_ReconstructionContext result = valid ? Tango3DR_ReconstructionContext_create(config) : nullptr;
            Tango3DR_Config_destroy(config);
#if SCANNER_MODERN
            if (result && pagingPlan.enabled) {
                if (ScannerReconstruction_enablePaging(result, pagingDirectory.c_str(),
                        pagingPlan.resident_bytes, pagingPlan.backing_bytes,
                        pagingPlan.logical_chunks) != TANGO_3DR_SUCCESS) {
                    // Never present an unconfigured pager as a large-capacity scan.
                    Tango3DR_ReconstructionContext_destroy(result);
                    LOGE("Unable to initialize private reconstruction paging");
                    return nullptr;
                }
            }
#else
            (void)pagingDirectory;
            (void)pagingPlan;
#endif
            return result;
        }
    }

    bool GridIndex::operator==(const GridIndex &o) const {
        return indices[0] == o.indices[0] && indices[1] == o.indices[1] && indices[2] == o.indices[2];
    }

    TangoScan::TangoScan() {
        context = nullptr;
        revision = 0;
        graph = 0;
        compo = 0;
        merge = 0;
        res_ = 0.04;
        dmin_ = 0;
        dmax_ = 15;
        noise_ = 0;
        clearing_ = true;
    }

    TangoScan::~TangoScan() {
        if (context != nullptr) {
            Tango3DR_ReconstructionContext_destroy(context);
            context = nullptr;
        }
    }

    void TangoScan::Clear() {
        ClearGeometry();
        ClearContext();
    }

    void TangoScan::ClearContext() {
        Setup3DR(res_, dmin_, dmax_, noise_, clearing_);
        lastMerged.clear();
    }

    void TangoScan::ClearGeometry() {
        Tango3DR_Status ret;
        for (const std::pair<const GridIndex, Tango3DR_Mesh*>& p : meshes) {
            ret = Tango3DR_Mesh_destroy(p.second);
            if (ret != TANGO_3DR_SUCCESS) { LOGE("Failed to release scan mesh"); }
            delete p.second;
        }
        meshes.clear();
        revision++;
    }

    std::string TangoScan::DebugInfo() {
        std::string output = "";
        char buffer[4096];
        if (graph > 10) {
            sprintf(buffer, "Generating graph: %dms\n", graph);
            output += buffer;
        }
        if (compo > 10) {
            sprintf(buffer, "Generating components: %dms\n", compo);
            output += buffer;
        }
        if (merge > 10) {
            sprintf(buffer, "Merging components: %dms\n", merge);
            output += buffer;
        }
        return output;
    }

    void TangoScan::Delete(std::vector<GridIndex>& toDelete) {
        Tango3DR_Status ret;
        for (GridIndex gi : toDelete) {
            std::unordered_map<GridIndex, Tango3DR_Mesh*, GridIndexHasher>::iterator mesh = meshes.find(gi);
            if (mesh != meshes.end()) {
                ret = Tango3DR_Mesh_destroy(mesh->second);
                if (ret != TANGO_3DR_SUCCESS) { LOGE("Failed to release deleted scan mesh"); }
                delete mesh->second;
                meshes.erase(mesh);
                revision++;
            }
        }
    }

    void TangoScan::Add(GridIndex index, Tango3DR_Mesh* mesh) {
        Replace(index, mesh);
    }

    void TangoScan::Replace(const GridIndex& index, Tango3DR_Mesh* mesh) {
        revision++;
        std::unordered_map<GridIndex, Tango3DR_Mesh*, GridIndexHasher>::iterator old = meshes.find(index);
        if (old != meshes.end()) {
            Tango3DR_Mesh_destroy(old->second);
            delete old->second;
            meshes.erase(old);
        }
        if (!mesh) return;
        if (mesh->num_vertices == 0) {
            Tango3DR_Mesh_destroy(mesh);
            delete mesh;
            return;
        }
        meshes.emplace(index, mesh);
    }

    void TangoScan::DiscardAdded() {
        for (std::pair<GridIndex, Tango3DR_Mesh*>& p : added) {
            Tango3DR_Mesh_destroy(p.second);
            delete p.second;
        }
        added.clear();
    }

    std::vector<Mesh> TangoScan::Export() {
        std::vector<Mesh> output;
        output.reserve(meshes.size());
        for (auto& m : meshes) {
            output.emplace_back();
            Mesh& mesh = output.back();
            mesh.vertices.reserve(m.second->num_vertices);
            mesh.colors.reserve(m.second->num_vertices);
            mesh.indices.reserve(m.second->num_faces * 3);
            for (int i = 0; i < m.second->num_vertices; i++) {
                glm::vec3 v(0);
                v.x = m.second->vertices[i][0];
                v.y = m.second->vertices[i][1];
                v.z = m.second->vertices[i][2];
                mesh.vertices.push_back(v);

                glm::ivec3 c(0);
                c.r = m.second->colors[i][0];
                c.g = m.second->colors[i][1];
                c.b = m.second->colors[i][2];
                mesh.colors.push_back(File3d::CodeColor(c));
            }
            for (int i = 0; i < m.second->num_faces; i++) {
                mesh.indices.push_back(m.second->faces[i][0]);
                mesh.indices.push_back(m.second->faces[i][1]);
                mesh.indices.push_back(m.second->faces[i][2]);
            }
        }
        return output;
    }

    void TangoScan::Merge() {
        lastMerged.clear();
        for (const std::pair<GridIndex, Tango3DR_Mesh*>& p : added) {
            lastMerged.push_back(p.first);
            Replace(p.first, p.second);
        }
        added.clear();
    }

    void TangoScan::Merge(std::vector<std::pair<GridIndex, Tango3DR_Mesh *>>& data) {
        for (const std::pair<GridIndex, Tango3DR_Mesh*>& p : data) {
            Replace(p.first, p.second);
        }
    }

    void TangoScan::Reset3DR(double res, double dmin, double dmax, int noise, bool clearing) {
        Tango3DR_Status ret;
        for (const std::pair<const GridIndex, Tango3DR_Mesh*>& p : meshes) {
            ret = Tango3DR_Mesh_destroy(p.second);
            if (ret != TANGO_3DR_SUCCESS) { LOGE("Failed to release reset scan mesh"); }
            delete p.second;
        }
        meshes.clear();
        revision++;
        Setup3DR(res, dmin, dmax, noise, clearing);
        lastMerged.clear();
    }

    void TangoScan::ConfigurePaging(const std::string& directory, double resolution) {
        paging_directory_ = directory;
        paging_plan_ = PagingPlan();
#if SCANNER_MODERN
        uint64_t physical = 0, available = 0;
        const long pages = sysconf(_SC_PHYS_PAGES), pageSize = sysconf(_SC_PAGESIZE);
        if (pages > 0 && pageSize > 0 && uint64_t(pages) <= UINT64_MAX / uint64_t(pageSize))
            physical = uint64_t(pages) * uint64_t(pageSize);
        struct statvfs space = {};
        if (!directory.empty() && statvfs(directory.c_str(), &space) == 0) {
            const uint64_t blockSize = space.f_frsize ? space.f_frsize : space.f_bsize;
            if (blockSize && uint64_t(space.f_bavail) <= UINT64_MAX / blockSize)
                available = uint64_t(space.f_bavail) * blockSize;
        }
        if (!directory.empty()) paging_plan_ = PlanPaging(resolution, physical, available);
        LOGI("RECON_PAGING enabled=%d resolution=%.5f resident_limit_bytes=%llu backing_limit_bytes=%llu logical_limit=%u frame_chunk_limit=%u available_bytes=%llu",
             paging_plan_.enabled, resolution,
             (unsigned long long)paging_plan_.resident_bytes,
             (unsigned long long)paging_plan_.backing_bytes, paging_plan_.logical_chunks, paging_plan_.update_chunks,
             (unsigned long long)available);
        if (!paging_plan_.enabled)
            LOGE("Reconstruction paging unavailable; using the bounded RAM-only fallback");
#else
        (void)resolution;
#endif
    }

    uint64_t TangoScan::MeshPayloadBytes() const {
        uint64_t total = 0;
        for (const auto& entry : meshes) {
            const Tango3DR_Mesh* mesh = entry.second;
            if (!mesh) continue;
            uint64_t vertices = std::max(mesh->num_vertices, mesh->max_num_vertices);
            uint64_t faces = std::max(mesh->num_faces, mesh->max_num_faces);
            total += sizeof(Tango3DR_Mesh);
            if (mesh->vertices) total += vertices * sizeof(Tango3DR_Vector3);
            if (mesh->normals) total += vertices * sizeof(Tango3DR_Vector3);
            if (mesh->colors) total += vertices * sizeof(Tango3DR_Color);
            if (mesh->faces) total += faces * sizeof(Tango3DR_Face);
            if (mesh->texture_coords) total += vertices * sizeof(Tango3DR_TexCoord);
            if (mesh->texture_ids) total += faces * sizeof(int32_t);
        }
        return total; // Live preview arrays only, not map nodes, GL/AR or export copies.
    }

    void TangoScan::LogStorageStats() const {
#if SCANNER_MODERN
        ScannerReconstruction_PagingStats stats = {};
        if (!context || ScannerReconstruction_getPagingStats(context, &stats) != TANGO_3DR_SUCCESS) return;
        const bool pagingEnabled = stats.resident_budget_bytes != 0;
        LOGI("RECON_CACHE enabled=%d logical=%llu logical_limit=%u resident=%llu resident_bytes=%llu peak_resident_bytes=%llu resident_limit_bytes=%llu backing_bytes=%llu backing_limit_bytes=%llu reads=%llu writes=%llu evictions=%llu preview_mesh_bytes=%llu",
             pagingEnabled,
             (unsigned long long)stats.logical_chunks, pagingEnabled ? paging_plan_.logical_chunks : 1024u,
             (unsigned long long)stats.resident_chunks,
             (unsigned long long)stats.resident_bytes, (unsigned long long)stats.peak_resident_bytes,
             (unsigned long long)stats.resident_budget_bytes,
             (unsigned long long)stats.backing_bytes, (unsigned long long)stats.backing_budget_bytes,
             (unsigned long long)stats.reads,
             (unsigned long long)stats.writes, (unsigned long long)stats.evictions,
             (unsigned long long)MeshPayloadBytes());
#endif
    }

    bool TangoScan::Setup3DR(double res, double dmin, double dmax, int noise, bool clearing) {
        color_calibration.Reset();
        res_ = res;
        dmin_ = dmin;
        dmax_ = dmax;
        noise_ = noise;
        clearing_ = clearing;
        Tango3DR_ReconstructionContext replacement = CreateContext(res, dmin, dmax, noise, clearing,
                                                                   paging_directory_, paging_plan_);
        if (!replacement) {
            update_failed = true;
            LOGE("Unable to create reconstruction context");
            return false;
        }
        if (context) Tango3DR_ReconstructionContext_destroy(context);
        context = replacement;
        update_failed = false;

#ifdef TANGO
        Tango3DR_ReconstructionContext_setColorCalibration(context, &camera);
        Tango3DR_ReconstructionContext_setDepthCalibration(context, &depth);
#endif

        return true;
    }

    bool TangoScan::RecoverContext(Dataset* dataset, TangoTexturize& texturize, int expectedFrames,
                                   bool& storageFailure, const std::atomic<bool>& requestedRunning,
                                   bool& cancelled) {
        storageFailure = false;
        cancelled = false;
        if (!update_failed) return true;
        if (!requestedRunning.load()) { cancelled = true; return false; }
        if (!dataset || expectedFrames < 0) { storageFailure = true; return false; }
        int count = 0, width = 0, height = 0;
        double cx = 0, cy = 0, fx = 0, fy = 0;
        dataset->ReadState(count, width, height, cx, cy, fx, fy);
        if (count != expectedFrames) { storageFailure = true; return false; }
        if (count > 0 && (width <= 0 || height <= 0 || width > 8192 || height > 8192 ||
            !std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(fx) || !std::isfinite(fy) || fx <= 0 || fy <= 0)) {
            storageFailure = true;
            return false;
        }

        const int scale = width > 1000 ? 3 : 1;
        Tango3DR_CameraCalibration calibration = {};
        std::vector<float> distortion = dataset->ReadDistortion();
        for (float v : distortion) if (!std::isfinite(v)) { storageFailure = true; return false; }
        texturize.ApplyDistortion(calibration, distortion);
        calibration.width = width / scale;
        calibration.height = height / scale;
        calibration.cx = cx / scale;
        calibration.cy = cy / scale;
        calibration.fx = fx / scale;
        calibration.fy = fy / scale;

        // The suspect SDK volume is disposable. Committed preview meshes and
        // dataset files stay intact throughout recovery; no GL work is needed.
        color_calibration.Reset();
        if (context) Tango3DR_ReconstructionContext_destroy(context);
        context = nullptr;
        // Keep this scan's original budgets during replay. Recomputing from free
        // disk while replacing an existing cache could strand committed history.
        Tango3DR_ReconstructionContext replacement = CreateContext(res_, dmin_, dmax_, noise_, clearing_,
                                                                   paging_directory_, paging_plan_);
        if (!replacement) return false;
        bool valid = count == 0 || Tango3DR_ReconstructionContext_setColorCalibration(replacement, &calibration) == TANGO_3DR_SUCCESS;
        for (int i = 0; valid && i < count; ++i) {
            // Pause/backgrounding must release the binder after at most the
            // current SDK frame, not after replaying the entire scan history.
            if (!requestedRunning.load()) {
                cancelled = true;
                valid = false;
                break;
            }
            std::vector<glm::mat4> poses;
            if (!dataset->ReadPose(i, poses) || poses.size() <= COLOR_CAMERA ||
                !geometry::RigidPose(poses[COLOR_CAMERA])) { storageFailure = true; valid = false; break; }
            Tango3DR_Pose pose = texturize.Extract3DRPose(poses[COLOR_CAMERA]);
            Tango3DR_PointCloud cloud = dataset->ReadPointCloud(i);
            if (!geometry::Cloud(&cloud)) {
                Tango3DR_PointCloud_destroy(&cloud);
                storageFailure = true;
                valid = false;
                break;
            }
            Image frame(dataset->GetFileName(i, ".jpg"));
            Tango3DR_ImageBuffer image = {};
            image.width = calibration.width;
            image.height = calibration.height;
            image.stride = calibration.width;
            image.timestamp = cloud.timestamp;
            image.format = TANGO_3DR_HAL_PIXEL_FORMAT_YCrCb_420_SP;
            if (frame.IsValid() && frame.GetWidth() == width && frame.GetHeight() == height) {
                image.data = frame.ExtractYUVDownscaled(scale);
            } else {
                storageFailure = true;
            }
            Tango3DR_GridIndexArray indices = {};
            valid = image.data && Tango3DR_updateFromPointCloud(replacement, &cloud, &pose,
                                                               &image, &pose, &indices) == TANGO_3DR_SUCCESS;
            delete[] image.data;
            Tango3DR_GridIndexArray_destroy(&indices);
            Tango3DR_PointCloud_destroy(&cloud);
        }
        if (!valid) {
            Tango3DR_ReconstructionContext_destroy(replacement);
            return false;
        }
        context = replacement;
        update_failed = false;
        return true;
    }

    bool TangoScan::Update(const Tango3DR_PointCloud* pcl, const Tango3DR_Pose *t3dr_depth_pose,
                           Tango3DR_ImageBuffer *t3dr_image, const Tango3DR_Pose *t3dr_image_pose,
                           bool postprocessing) {
        Tango3DR_Status ret;
        update_rejected = false;
        update_accepted = false;
        DiscardAdded();
        // A suspect SDK volume is replayed automatically by the worker before
        // accepting another frame. Invalid input alone never poisons the state.
        if (update_failed || !context || !pcl || !t3dr_image || !t3dr_image->data ||
            !t3dr_image->width || !t3dr_image->height ||
            t3dr_image->stride < t3dr_image->width ||
            !geometry::Pose(t3dr_depth_pose) ||
            !geometry::Pose(t3dr_image_pose)) {
            if (t3dr_image) { delete[] t3dr_image->data; t3dr_image->data = nullptr; }
            return false;
        }
        Tango3DR_GridIndexArray t3dr_updated = {};
        if (!geometry::Cloud(pcl)) {
            delete[] t3dr_image->data;
            t3dr_image->data = nullptr;
            return false;
        }
        ret = Tango3DR_updateFromPointCloud(context, pcl, t3dr_depth_pose, t3dr_image, t3dr_image_pose, &t3dr_updated);
        delete[] t3dr_image->data;
        t3dr_image->data = nullptr;
        if (ret != TANGO_3DR_SUCCESS) {
            Tango3DR_GridIndexArray_destroy(&t3dr_updated);
            // The owned backend publishes its staged volume only on SUCCESS.
            // Replaying committed history cannot repair a resource-limit
            // rejection and can hit the same limit repeatedly. Extraction and
            // persistence failures AFTER a successful update still need replay.
#if SCANNER_MODERN
            ScannerReconstruction_PagingStats paging = {};
            // A rejected update is transactional, but an unreadable/corrupt cold
            // page cannot be repaired merely by retrying that same context.
            update_failed = ScannerReconstruction_getPagingStats(context, &paging) != TANGO_3DR_SUCCESS
                    || paging.requires_replay != 0;
            update_rejected = !update_failed && ret != TANGO_3DR_INVALID;
#else
            // The vendor backend only guarantees this for invalid parameters.
            update_failed = ret != TANGO_3DR_INVALID;
#endif
            const int64_t now = CaptureBackoff::NowMs();
            if (update_failed || ret != last_rejection_status || now - last_rejection_log_ms >= 5000) {
#if SCANNER_MODERN
                LOGE("Reconstruction frame rejected (sdk=%d cache=%u replay=%d); preserving committed geometry",
                     ret, paging.last_failure, update_failed);
#else
                LOGE("Reconstruction frame rejected (%d); preserving committed geometry", ret);
#endif
                last_rejection_log_ms = now;
                last_rejection_status = ret;
            }
            return false;
        }
        update_accepted = true;
        last_rejection_status = TANGO_3DR_SUCCESS;
        if (t3dr_updated.num_indices && !t3dr_updated.indices) {
            update_failed = true;
            return false;
        }

        unsigned long size = t3dr_updated.num_indices;
        added.reserve(size);
        std::pair<GridIndex, Tango3DR_Mesh*> pair;
        for (unsigned long it = 0; it < size; ++it) {
            pair.first.indices[0] = t3dr_updated.indices[it][0];
            pair.first.indices[1] = t3dr_updated.indices[it][1];
            pair.first.indices[2] = t3dr_updated.indices[it][2];

            pair.second = new Tango3DR_Mesh();
            ret = Tango3DR_extractMeshSegment(context, t3dr_updated.indices[it], pair.second);
            if (ret != TANGO_3DR_SUCCESS || !geometry::Mesh(pair.second)) {
                Tango3DR_Mesh_destroy(pair.second);
                delete pair.second;
                Tango3DR_GridIndexArray_destroy(&t3dr_updated);
                DiscardAdded();
                update_failed = true;
                LOGE("Reconstruction extraction failed; preserving committed geometry");
                return false;
            }
            added.push_back(pair);
        }
        Tango3DR_GridIndexArray_destroy(&t3dr_updated);

        if (postprocessing) {
            clock_t step0 = clock();
            GenerateGraph();
            clock_t step1 = clock();
            GenerateComponents();
            clock_t step2 = clock();
            MergeComponents();
            clock_t step3 = clock();
            graph = int(step1 - step0) / 1000;
            compo = int(step2 - step1) / 1000;
            merge = int(step3 - step2) / 1000;
        }
        return size > 0;
    }

    void TangoScan::GenerateComponents() {
        components.clear();
        if (xorEdges.empty())
            return;

        std::map<std::string, std::string> point2edge;
        for (std::map<std::string, Edge>::const_iterator it = xorEdges.begin(); it != xorEdges.end(); ++it)
            point2edge[Mesh::Vector2key(Retango::Vec4ToVec3((*it).second.point[0]))] = (*it).first;

        while (!xorEdges.empty()) {
            Component c;
            std::string key = (*xorEdges.begin()).first;
            while (xorEdges.find(key) != xorEdges.end()) {
                Edge e = xorEdges[key];
                c.edges.push_back(e);
                xorEdges.erase(key);
                key = point2edge[Mesh::Vector2key(Retango::Vec4ToVec3(e.point[1]))];
            }
            if (!c.edges.empty()) {
                c.closed = glm::distance(c.edges[0].point[0], c.edges[c.edges.size() - 1].point[1]) < 0.01f;
                c.valid = true;
                components.push_back(std::move(c));
            }
        }
    }

    void TangoScan::GenerateGraph() {
        xorEdges.clear();

        Edge e;
        int ia, ib, ic;
        unsigned long i;
        std::string key;
        bool foundA, foundB;
        glm::vec4 a(1), b(1);
        std::pair<int, int> keyA, keyB;
        std::string vecA, vecB, vecAB, vecBA;
        std::map<std::string, Edge>::const_iterator itEdge;
        std::map<std::pair<int, int>, bool>::const_iterator it;
        for (std::pair<GridIndex, Tango3DR_Mesh *> &node : added) {

            //process xoring on grid level
            std::map<std::pair<int, int>, bool> xoring;
            for (i = 0; i < node.second->num_faces; ++i) {
                ia = node.second->faces[i][0];
                ib = node.second->faces[i][1];
                ic = node.second->faces[i][2];

                //AB
                keyA.first = ia;
                keyA.second = ib;
                keyB.first = ib;
                keyB.second = ia;
                foundA = xoring.find(keyA) != xoring.end();
                foundB = xoring.find(keyB) != xoring.end();
                if (!foundA && !foundB) {
                    xoring[keyA] = true;
                } else {
                    xoring.erase(foundA ? keyA : keyB);
                }

                //BC
                keyA.first = ib;
                keyA.second = ic;
                keyB.first = ic;
                keyB.second = ib;
                foundA = xoring.find(keyA) != xoring.end();
                foundB = xoring.find(keyB) != xoring.end();
                if (!foundA && !foundB) {
                    xoring[keyA] = true;
                } else {
                    xoring.erase(foundA ? keyA : keyB);
                }

                //CA
                keyA.first = ic;
                keyA.second = ia;
                keyB.first = ia;
                keyB.second = ic;
                foundA = xoring.find(keyA) != xoring.end();
                foundB = xoring.find(keyB) != xoring.end();
                if (!foundA && !foundB) {
                    xoring[keyA] = true;
                } else {
                    xoring.erase(foundA ? keyA : keyB);
                }
            }

            //process xoring on active scene level
            for (it = xoring.begin(); it != xoring.end(); ++it) {
                keyA = it->first;
                a.x = node.second->vertices[keyA.first][0];
                a.y = node.second->vertices[keyA.first][1];
                a.z = node.second->vertices[keyA.first][2];
                b.x = node.second->vertices[keyA.second][0];
                b.y = node.second->vertices[keyA.second][1];
                b.z = node.second->vertices[keyA.second][2];

                vecA = Mesh::Vector2key(Retango::Vec4ToVec3(a));
                vecB = Mesh::Vector2key(Retango::Vec4ToVec3(b));
                vecAB = vecA;
                vecAB += " ^ ";
                vecAB += vecB;
                vecBA = vecB;
                vecBA += " ^ ";
                vecBA += vecA;

                bool foundAB = xorEdges.find(vecAB) != xorEdges.end();
                bool foundBA = xorEdges.find(vecBA) != xorEdges.end();
                if (!foundAB && !foundBA) {
                    e.point[0] = a;
                    e.point[1] = b;
                    xorEdges[vecAB] = e;
                } else {
                    xorEdges.erase(foundAB ? vecAB : vecBA);
                }
            }
        }
    }

    void TangoScan::MergeComponents() {
        for (int i = 0; i < components.size(); i++) {
            if (components[i].closed || !components[i].valid)
                continue;

            while (true) {
                int index = -1;
                glm::vec4 point = components[i].edges[components[i].edges.size() - 1].point[1];
                float min = glm::distance(point, components[i].edges[0].point[0]);
                for (int j = components.size() - 1; j > i; j--) {
                    if (components[j].closed || !components[j].valid)
                        continue;
                    float distance = glm::distance(point, components[j].edges[0].point[0]);
                    if (min > distance) {
                        min = distance;
                        index = j;
                    }
                }

                if (index >= 0) {
                    components[index].valid = false;
                    std::vector<Edge>::const_iterator iter = components[i].edges.end();
                    components[i].edges.insert(iter, components[index].edges.begin(), components[index].edges.end());
                    if (glm::distance(components[i].edges[0].point[0], components[i].edges[components[i].edges.size() - 1].point[1]) < 0.01f) {
                        components[i].closed = true;
                        break;
                    }
                } else {
                    components[i].closed = true;
                    break;
                }
            }
        }
    }
}
