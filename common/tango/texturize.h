#ifndef TANGO_TEXTURIZE_H
#define TANGO_TEXTURIZE_H

#include <data/dataset.h>
#include <gl/opengl.h>

namespace oc {

    class TangoTexturize {
    public:
        TangoTexturize();
        ~TangoTexturize();
        TangoTexturize(const TangoTexturize&) = delete;
        TangoTexturize& operator=(const TangoTexturize&) = delete;
        bool Add(Image* image, double timestamp, Tango3DR_CameraCalibration* camera, std::vector<glm::mat4> matrix, Dataset* dataset);
        bool Commit(Tango3DR_CameraCalibration* camera, Dataset* dataset);
        void ApplyDistortion(Tango3DR_CameraCalibration& camera, const std::vector<float>& frame_distortion);
        bool ApplyFrames(Dataset* dataset);
        bool ApplyFrames(Dataset* dataset, const std::vector<int>& frames);
        Tango3DR_CameraCalibration Camera() { return camera; }
        bool Clear(Dataset* dataset);
        bool CreateContext(Tango3DR_Mesh* mesh, bool verbose, bool ignoreConfig);
        bool ResetContext();
        Tango3DR_TexturingContext Context() { return context; }
        bool DeleteLast(Dataset* dataset);
        Tango3DR_Pose Extract3DRPose(glm::mat4 matrix);
        std::string GetEvent() { return event; }
        int GetLatestIndex(Dataset* dataset);
        double GetTimestamp(Dataset* dataset, int index);
        int GetWidth() { return width; }
        int GetHeight() { return height; }
        bool Init(std::string filename, bool verbose, bool ignoreConfig);
        bool Process(std::string filename, bool verbose = true, bool poisson = false);
        bool SetCalibration(Tango3DR_ReconstructionContext context, Dataset* dataset, int scale = 1);
        void SetDistortion(bool on) { useDistortion = on; }
        void SetEvent(std::string value) { event = value; }
        void SetTextureParams(int detail, int res, int count) { meshSimplification = detail, textureResolution = res; textureCount = count; }
        bool Truncate(int count, Dataset* dataset);
        bool UpdatePoses(Dataset* dataset);

    private:
        void ScaleMesh(Tango3DR_Mesh* mesh, float s);

        int poses;
        std::string event;
        Tango3DR_CameraCalibration camera;
        Tango3DR_TexturingContext context;
        int width, height;
        double cx, cy, fx, fy;
        bool useDistortion;

        int meshSimplification;
        int textureCount;
        int textureResolution;
        float scale;
    };
}
#endif
