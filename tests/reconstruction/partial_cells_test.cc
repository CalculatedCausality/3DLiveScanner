// A fully observed tetrahedron must survive unrelated unknown cube corners.
// Never infer triangles from three measured nodes or sub-threshold weights.
#ifndef MESHING_CORE_SOURCE
#define MESHING_CORE_SOURCE "../../reconstruction/core.cc"
#endif
#include MESHING_CORE_SOURCE
#include <cstdio>

int main(int argc, char** argv) {
    assert(argc == 2);
    const int tetrahedra[6][4]={{0,1,3,7},{0,3,2,7},{0,2,6,7},
                               {0,6,4,7},{0,4,5,7},{0,5,1,7}};
    for (const auto& tetrahedron : tetrahedra)
      for (bool paged : {false,true}) for (int base : {-17,-1,0,15,16})
        for (int missing=-1; missing<6; ++missing) {
            auto config=Tango3DR_Config_create(TANGO_3DR_CONFIG_RECONSTRUCTION);
            assert(config);
            assert(Tango3DR_Config_setDouble(config,"resolution",.02)==0);
            auto context=Tango3DR_ReconstructionContext_create(config);
            Tango3DR_Config_destroy(config); assert(context);
            if (paged) assert(ScannerReconstruction_enablePaging(context,argv[1],8*98304,64*98304,32)==0);
            recon::Index nodes[4];
            int negative=0;
            for (int i=0;i<4;++i) {
                const int corner=tetrahedron[i];
                nodes[i]=recon::Index(base+(corner&1),base+((corner>>1)&1),base+(corner>>2));
                if (nodes[i].x==base) ++negative;
            }
            {
                recon::Transaction transaction(*context);
                for (int i=0;i<4;++i) {
                    if (i==missing) continue;
                    auto* voxel=transaction.writable(nodes[i],true); assert(voxel);
                    voxel->sdf=(nodes[i].x==base && missing!=5)?-1:1;
                    voxel->weight=missing==4?.999f:1;
                }
                if(paged)context->paged_volume.swap(transaction.paged_next);
                else context->volume.swap(transaction.next);
            }
            const auto owner=recon::chunkOf(nodes[0]);
            const Tango3DR_GridIndex key={owner.x,owner.y,owner.z};
            Tango3DR_Mesh mesh{};
            assert(Tango3DR_extractMeshSegment(context,key,&mesh)==0);
            if (missing==-1) {
                assert(mesh.num_faces==unsigned(negative==2?2:1));
                assert(mesh.num_vertices==unsigned(negative*(4-negative)));
                for (unsigned i=0;i<mesh.num_vertices;++i) {
                    assert(std::abs(mesh.vertices[i][0]-(base+.5)*.02)<1e-6);
                    for (int j=1;j<3;++j) {
                        assert(mesh.vertices[i][j]>=base*.02-1e-6);
                        assert(mesh.vertices[i][j]<=(base+1)*.02+1e-6);
                    }
                }
            } else assert(mesh.num_faces==0 && mesh.num_vertices==0);
            Tango3DR_Mesh_destroy(&mesh);
            Tango3DR_ReconstructionContext_destroy(context);
        }
    std::puts("PASS: all six supported partial-cell sections, unknown/weak-node exclusion, both chunk boundaries, RAM and minimum-budget pager");
}
