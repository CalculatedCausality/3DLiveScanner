#include <data/dataset.h>
#include <data/image.h>
#include <glm/gtc/quaternion.hpp>
#include <array>
#include <cassert>
#include <cmath>
#include <iostream>
#include <set>

int main(int argc, char** argv) {
    assert(argc == 2);
    oc::Dataset dataset(argv[1]);
    assert(dataset.ValidateCommittedFrames());
    int count=0,width=0,height=0;
    double cx=0,cy=0,fx=0,fy=0;
    dataset.ReadState(count,width,height,cx,cy,fx,fy);
    assert(count==3 && width==64 && height==64);
    Tango3DR_Config config=Tango3DR_Config_create(TANGO_3DR_CONFIG_RECONSTRUCTION);
    assert(config);
    assert(Tango3DR_Config_setDouble(config,"resolution",.04)==TANGO_3DR_SUCCESS);
    assert(Tango3DR_Config_setDouble(config,"min_depth",.1)==TANGO_3DR_SUCCESS);
    assert(Tango3DR_Config_setDouble(config,"max_depth",5)==TANGO_3DR_SUCCESS);
    assert(Tango3DR_Config_setInt32(config,"min_num_vertices",0)==TANGO_3DR_SUCCESS);
    Tango3DR_ReconstructionContext context=Tango3DR_ReconstructionContext_create(config);
    assert(context);
    Tango3DR_Config_destroy(config);
    Tango3DR_CameraCalibration calibration{};
    calibration.calibration_type=TANGO_3DR_CALIBRATION_POLYNOMIAL_3_PARAMETERS;
    calibration.width=width; calibration.height=height;
    calibration.cx=cx; calibration.cy=cy; calibration.fx=fx; calibration.fy=fy;
    assert(Tango3DR_ReconstructionContext_setColorCalibration(context,&calibration)==TANGO_3DR_SUCCESS);
    std::set<std::array<int32_t,3>> dirty;
    for(int i=0;i<count;++i) {
        auto poses=dataset.ReadPose(i);
        assert(poses.size()==oc::MAX_CAMERA);
        const auto& matrix=poses[oc::COLOR_CAMERA];
        glm::quat rotation=glm::quat_cast(matrix);
        Tango3DR_Pose pose{};
        for(int k=0;k<3;++k) pose.translation[k]=matrix[3][k];
        for(int k=0;k<4;++k) pose.orientation[k]=rotation[k];
        Tango3DR_PointCloud cloud=dataset.ReadPointCloud(i);
        assert(cloud.num_points==1024);
        oc::Image frame(dataset.GetFileName(i,".jpg"));
        assert(frame.IsValid() && frame.GetWidth()==width && frame.GetHeight()==height);
        Tango3DR_ImageBuffer image{};
        image.width=width; image.height=height; image.stride=width;
        image.format=TANGO_3DR_HAL_PIXEL_FORMAT_YCrCb_420_SP;
        image.data=frame.ExtractYUVDownscaled(1);
        assert(image.data);
        Tango3DR_GridIndexArray changed{};
        assert(Tango3DR_updateFromPointCloud(context,&cloud,&pose,&image,&pose,&changed)==TANGO_3DR_SUCCESS);
        for(unsigned k=0;k<changed.num_indices;++k)
            dirty.insert({{changed.indices[k][0],changed.indices[k][1],changed.indices[k][2]}});
        delete[] image.data;
        Tango3DR_GridIndexArray_destroy(&changed);
        Tango3DR_PointCloud_destroy(&cloud);
    }
    size_t triangles=0,vertices=0;
    double worst=0;
    for(const auto& key:dirty) {
        Tango3DR_GridIndex index={key[0],key[1],key[2]};
        Tango3DR_Mesh mesh{};
        assert(Tango3DR_extractMeshSegment(context,index,&mesh)==TANGO_3DR_SUCCESS);
        triangles+=mesh.num_faces; vertices+=mesh.num_vertices;
        for(unsigned v=0;v<mesh.num_vertices;++v) {
            for(int axis=0;axis<3;++axis) assert(std::isfinite(mesh.vertices[v][axis]));
            worst=std::max(worst,std::abs(double(mesh.vertices[v][2])-2.));
        }
        Tango3DR_Mesh_destroy(&mesh);
    }
    Tango3DR_ReconstructionContext_destroy(context);
    assert(triangles>0 && vertices>0 && worst<.12);
    assert(dataset.ValidateCommittedFrames());
    std::cout << "PASS: real legacy-format dataset reader + modern live-image conversion + owned reconstruction: "
              << triangles << " triangles, max plane Z error " << worst << " m\n";
}
