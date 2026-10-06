#ifndef TANGO_SCAN_H
#define TANGO_SCAN_H

#include <map>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include "data/mesh.h"
#include "gl/glsl.h"
#include "tango/retango.h"
#include "tango/texturize.h"
#include "tango/calibration_cache.h"
#include "tango/paging_policy.h"

namespace oc {

    class TangoScan {
    public:
        TangoScan();
        ~TangoScan();
        void Add(GridIndex index, Tango3DR_Mesh* mesh);
        const std::vector<std::pair<GridIndex, Tango3DR_Mesh*> >& Added() const { return added; };
        void Clear();
        void ClearContext();
        void ClearGeometry();
        void ClearLast() { lastMerged.clear(); }
        const std::vector<Component>& Components() const { return components; }
        Tango3DR_ReconstructionContext Context() { return context; }
        double MinimumDepth() const { return dmin_; }
        double MaximumDepth() const { return dmax_; }
        bool SelectResolution(double resolution) { return Setup3DR(resolution,dmin_,dmax_,noise_,clearing_); }
        bool SetColorCalibration(const Tango3DR_CameraCalibration& camera) { return color_calibration.Apply(context, camera); }
        const std::unordered_map<GridIndex, Tango3DR_Mesh*, GridIndexHasher>& Data() const { return meshes; }
        void Delete(std::vector<GridIndex>& toDelete);
        void DiscardAdded();
        std::string DebugInfo();
        std::vector<Mesh> Export();
        const std::vector<GridIndex>& Last() const { return lastMerged; }
        void Merge();
        void Merge(std::vector<std::pair<GridIndex, Tango3DR_Mesh *> >& data);
        void Reset3DR(double res, double dmin, double dmax, int noise, bool clearing = true);
        void ConfigurePaging(const std::string& directory, double resolution);
        void LogStorageStats() const;
        uint64_t MeshPayloadBytes() const;
        bool Setup3DR(double res, double dmin, double dmax, int noise, bool clearing = true);
        bool RecoverContext(Dataset* dataset, TangoTexturize& texturize, int expectedFrames,
                            bool& storageFailure, const std::atomic<bool>& requestedRunning,
                            bool& cancelled);
        float Resolution() { return (float)res_; }
        uint64_t Revision() const { return revision; }
        bool UpdateFailed() const { return update_failed; }
        bool UpdateRejected() const { return update_rejected; }
        bool UpdateAccepted() const { return update_accepted; }
        void RequireRecovery() { update_failed = true; }
        size_t Size() const { return meshes.size(); }
        // Borrows the worker-owned cloud; consumes and clears t3dr_image->data.
        bool Update(const Tango3DR_PointCloud* pcl, const Tango3DR_Pose* t3dr_depth_pose,
                    Tango3DR_ImageBuffer* t3dr_image, const Tango3DR_Pose* t3dr_image_pose,
                    bool postprocessing);
    private:
        void GenerateComponents();
        void GenerateGraph();
        void MergeComponents();
        void Replace(const GridIndex& index, Tango3DR_Mesh* mesh);

        std::vector<std::pair<GridIndex, Tango3DR_Mesh*> > added;
        std::vector<Component> components;
        Tango3DR_ReconstructionContext context;
        ColorCalibrationCache color_calibration;
        std::vector<GridIndex> lastMerged;
        std::unordered_map<GridIndex, Tango3DR_Mesh*, GridIndexHasher> meshes;
        uint64_t revision;
        bool update_failed = false;
        bool update_rejected = false;
        bool update_accepted = false;
        int64_t last_rejection_log_ms = -5000;
        int last_rejection_status = 0;
        std::map<std::string, Edge> xorEdges;

        //setup
        bool clearing_;
        double res_;
        double dmin_;
        double dmax_;
        int noise_;
        std::string paging_directory_;
        PagingPlan paging_plan_;

        //profiling
        int graph;
        int compo;
        int merge;
    };
}
#endif
