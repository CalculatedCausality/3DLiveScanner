#include <sstream>
#include <cmath>
#include <memory>
#include <new>
#include "data/file3d.h"
#include "data/image.h"
#include "gl/camera.h"
#include "tango/texturize.h"
namespace oc {

    TangoTexturize::TangoTexturize() : poses(0), camera{}, context(nullptr), width(0), height(0),
                                       cx(0), cy(0), fx(0), fy(0),
                                       useDistortion(false),
                                       meshSimplification(10),
                                       textureCount(4),
                                       textureResolution(2048),
                                       scale(1) {}

    TangoTexturize::~TangoTexturize() { ResetContext(); }

    bool TangoTexturize::ResetContext() {
        Tango3DR_TexturingContext old = context;
        context = nullptr;
        return !old || Tango3DR_TexturingContext_destroy(old) == TANGO_3DR_SUCCESS;
    }

    bool TangoTexturize::Add(Image *image, double timestamp, Tango3DR_CameraCalibration *camera,
                              std::vector<glm::mat4> matrix, Dataset* dataset) {
        if (!image || !dataset || !camera || matrix.size() < MAX_CAMERA || !std::isfinite(timestamp)) return false;
        if (poses == 0)
            UpdatePoses(dataset);

        //save frame
        width = image->GetWidth();
        height = image->GetHeight();
        std::string image_path = dataset->GetFileName(poses, ".jpg");
        bool success = image->Write(image_path);

        //save transform
        success = dataset->WritePose(poses, matrix) && success;

        //save timestamp
        FILE* file = fopen(dataset->GetFileName(poses, ".tms").c_str(), "w");
        success = file && (fprintf(file, "%lf", timestamp) > 0) && success;
        if (file) success = (fclose(file) == 0) && success;

        if (success) poses++;
        return success;
    }

    bool TangoTexturize::Commit(Tango3DR_CameraCalibration* camera, Dataset* dataset) {
        return dataset && camera && dataset->WriteState(poses, width, height, camera->cx, camera->cy, camera->fx, camera->fy);
    }

    void TangoTexturize::ApplyDistortion(Tango3DR_CameraCalibration& camera, const std::vector<float>& distortion) {
        camera.calibration_type = TANGO_3DR_CALIBRATION_POLYNOMIAL_3_PARAMETERS;
        for (int i = 0; i < 5; ++i) camera.distortion[i] = 0;
        if (useDistortion) {
            switch (distortion.size()) {
                case 3:
                    camera.calibration_type = TANGO_3DR_CALIBRATION_POLYNOMIAL_3_PARAMETERS;
                    camera.distortion[0] = distortion[0];
                    camera.distortion[1] = distortion[1];
                    camera.distortion[2] = distortion[2];
                    break;
                case 5:
                    camera.calibration_type = TANGO_3DR_CALIBRATION_POLYNOMIAL_5_PARAMETERS;
                    camera.distortion[0] = distortion[0];
                    camera.distortion[1] = distortion[1];
                    camera.distortion[2] = distortion[2];
                    camera.distortion[3] = distortion[3];
                    camera.distortion[4] = distortion[4];
                    break;
            }
        } else {
            camera.calibration_type = TANGO_3DR_CALIBRATION_POLYNOMIAL_3_PARAMETERS;
            camera.distortion[0] = 0;
            camera.distortion[1] = 0;
            camera.distortion[2] = 0;
        }
    }

    bool TangoTexturize::ApplyFrames(Dataset* dataset) {
        try {
        if (!UpdatePoses(dataset)) { ResetContext(); event.clear(); return false; }
        std::vector<int> frames;
        for (unsigned int i = 0; i < poses; i++) {
            frames.push_back(i);
        }
        return ApplyFrames(dataset, frames);
        } catch (...) {
            ResetContext();
            event.clear();
            return false;
        }
    }

    bool TangoTexturize::ApplyFrames(Dataset *dataset, const std::vector<int>& frames) {
        // An incomplete frame invalidates the accumulated context; never publish it.
        auto fail = [this]() { ResetContext(); event = ""; return false; };
        try {
        if (!context || !UpdatePoses(dataset) || poses <= 0 || frames.empty()
                || (width & 1) || (height & 1)) return fail();
        Tango3DR_ImageBuffer image = {};
        image.width = (uint32_t) width;
        image.height = (uint32_t) height;
        image.stride = (uint32_t) width;
        image.format = TANGO_3DR_HAL_PIXEL_FORMAT_YCrCb_420_SP;
        const size_t pixels = static_cast<size_t>(width) * height;
        std::unique_ptr<unsigned char[]> buffer(new (std::nothrow) unsigned char[pixels + pixels / 2]);
        if (!buffer) return fail();
        image.data = buffer.get();

        for (int i = 0; i < frames.size(); i++) {
            std::ostringstream ss;
            ss << "IMAGE ";
            ss << i + 1;
            ss << "/";
            ss << frames.size();
            event = ss.str();

            if (frames[i] < 0 || frames[i] >= poses) return fail();
            std::vector<glm::mat4> matrices;
            if (!dataset->ReadPose(frames[i], matrices) || matrices.size() <= COLOR_CAMERA) return fail();
            for (const glm::mat4& matrix : matrices)
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        if (!std::isfinite(matrix[c][r])) return fail();
            const glm::mat4& pose = matrices[COLOR_CAMERA];
            if (std::fabs(pose[3][3] - 1) > 0.01) return fail();
            for (int c = 0; c < 3; ++c) {
                if (std::fabs(pose[c][3]) > 0.01) return fail();
                for (int r = 0; r < 3; ++r)
                    if (std::fabs(glm::dot(glm::vec3(pose[c]), glm::vec3(pose[r])) - (c == r ? 1 : 0)) > 0.02) return fail();
            }
            if (glm::determinant(glm::mat3(pose)) < 0.98f) return fail();
            image.timestamp = 0;
            if (!Image::JPG2YUV(dataset->GetFileName(frames[i], ".jpg"), image.data, width, height)) return fail();
            Tango3DR_Pose t3dr_image_pose = Extract3DRPose(matrices[COLOR_CAMERA]);
            double norm = 0;
            for (double q : t3dr_image_pose.orientation) norm += q * q;
            if (!std::isfinite(norm) || std::fabs(norm - 1) > 0.02) return fail();
            t3dr_image_pose.translation[0] *= scale;
            t3dr_image_pose.translation[1] *= scale;
            t3dr_image_pose.translation[2] *= scale;
            Tango3DR_Status ret = Tango3DR_updateTexture(context, &image, &t3dr_image_pose);
            if (ret != TANGO_3DR_SUCCESS)
                return fail();
        }
        return true;
        } catch (...) {
            return fail();
        }
    }

    bool TangoTexturize::Clear(Dataset* dataset) {
        if (!dataset || !dataset->ResetState()) return false;
        poses = 0;
        return true;
    }

    bool TangoTexturize::DeleteLast(Dataset* dataset) {
        if (!UpdatePoses(dataset)) return false;
        if (poses > 0) {
            if (!dataset->WriteState(poses - 1, width, height,
                                     camera.cx, camera.cy, camera.fx, camera.fy)) return false;
            poses--;
        }
        return true;
    }

    Tango3DR_Pose TangoTexturize::Extract3DRPose(glm::mat4 matrix) {
        Tango3DR_Pose pose;
        glm::quat rotation = glm::quat_cast(matrix);
        pose.translation[0] = matrix[3][0];
        pose.translation[1] = matrix[3][1];
        pose.translation[2] = matrix[3][2];
        pose.orientation[0] = rotation[0];
        pose.orientation[1] = rotation[1];
        pose.orientation[2] = rotation[2];
        pose.orientation[3] = rotation[3];
        return pose;
    }

    int TangoTexturize::GetLatestIndex(Dataset* dataset) {
        return UpdatePoses(dataset) ? poses - 1 : -1;
    }

    double TangoTexturize::GetTimestamp(Dataset* dataset, int index) {
        double timestamp = 0;
        if (!dataset || index < 0) return timestamp;
        FILE* file = fopen(dataset->GetFileName(index, ".tms").c_str(), "r");
        if (file) {
            if (fscanf(file, "%lf", &timestamp) != 1 || !std::isfinite(timestamp)) timestamp = 0;
            fclose(file);
        }
        return timestamp;
    }

    bool TangoTexturize::Init(std::string filename, bool verbose, bool ignoreConfig) {
        event.clear();
        if (!ResetContext()) return false;
        if (verbose)
            event = "MERGE";
        Tango3DR_Mesh mesh = {};
        Tango3DR_Status ret;
        ret = Tango3DR_Mesh_loadFromObj(filename.c_str(), &mesh);
        if (ret != TANGO_3DR_SUCCESS) {
            LOGE("Texturing input OBJ load failed (status=%d); source retained", int(ret));
            Tango3DR_Mesh_destroy(&mesh);
            event = "";
            return false;
        }

        //prevent crash on saving empty model
        if (mesh.num_faces == 0 || !mesh.faces || !mesh.num_vertices || !mesh.vertices) {
            ret = Tango3DR_Mesh_destroy(&mesh);
            if (verbose)
                event = "";
            return false;
        }

        //create texturing context
        ScaleMesh(&mesh, scale);
        bool success = CreateContext(&mesh, verbose, ignoreConfig);
        ret = Tango3DR_Mesh_destroy(&mesh);
        if (!success || ret != TANGO_3DR_SUCCESS) {
            ResetContext();
            event = "";
            return false;
        }
        return true;
    }

    bool TangoTexturize::Process(std::string filename, bool verbose, bool poisson) {
        event.clear();
        if (!context) return false;
        //texturize mesh
        if (verbose)
            event = "UNWRAP";
        Tango3DR_Mesh mesh = {};
        Tango3DR_Status ret;
        ret = Tango3DR_getTexturedMesh(context, &mesh);
        bool success = ret == TANGO_3DR_SUCCESS && mesh.num_vertices && mesh.vertices
                && mesh.num_faces && mesh.faces;

        //save
        if (verbose)
            event = "CONVERT";
        if (success) {
            ScaleMesh(&mesh, 1.0f / scale);
            success = Tango3DR_Mesh_saveToObj(&mesh, filename.c_str()) == TANGO_3DR_SUCCESS;
        }

        //cleanup
        ret = Tango3DR_Mesh_destroy(&mesh);
        success = (ret == TANGO_3DR_SUCCESS) && success;
        success = ResetContext() && success;
        event = "";
        if (!success) return false;

        //add alpha channel to textures
        if (poisson) {
            std::vector<Mesh> data;
            struct ImageGuard {
                std::vector<Mesh>& meshes;
                ~ImageGuard() { for (Mesh& mesh : meshes) mesh.Destroy(); }
            } images{data};
            File3d(filename, false).ReadModel(INT_MAX, data);
            if (data.empty()) return false;
            for (Mesh& m : data) {
                if (m.image && m.imageOwner && m.vertices.size()) {
                    if (!m.image->IsValid()) return false;
                    //get color statistics
                    std::map<int, int> colors;
                    for (unsigned int x = 0; x < m.image->GetWidth(); x++) {
                        for (unsigned int y = 0; y < m.image->GetHeight(); y++) {
                            int c = m.image->GetColor(x, y);
                            if (colors.find(c) == colors.end()) {
                                colors[c] = 0;
                            }
                            colors[c]++;
                        }
                    }

                    //get the most used color
                    int best = 0;
                    int count = 0;
                    for (std::map<int, int>::const_iterator it = colors.begin(); it != colors.end(); ++it) {
                        if (count < it->second) {
                            count = it->second;
                            best = it->first;
                        }
                    }

                    //replace color with transparency
                    glm::ivec4 transparent(128, 128, 128, 0);
                    for (unsigned int x = 0; x < m.image->GetWidth(); x++) {
                        for (unsigned int y = 0; y < m.image->GetHeight(); y++) {
                            int c = m.image->GetColor(x, y);
                            if (c == best) {
                                m.image->DrawPixel(x, y, transparent);
                            }
                        }
                    }

                    //save texture
                    if (!m.image->Write(m.image->GetName())) return false;
                }
            }
        }
        event = "";
        return true;
    }

    bool TangoTexturize::CreateContext(Tango3DR_Mesh* mesh, bool verbose, bool ignoreConfig) {
        event.clear();
        if (!ResetContext() || !mesh) return false;
        const int count = ignoreConfig ? 1 : textureCount;
        const int resolution = ignoreConfig ? 4096 : textureResolution;
#if SCANNER_MODERN
        // The clean-room backend preserves geometry; it implements no decimator.
        const int simplification = 1;
        // Mirror the backend's explicit-page budget. Zero delegates bounded page
        // selection to the backend; never silently clamp an explicit request.
        const uint64_t maxPixels = 16u * 1024u * 1024u;
        if (resolution < 16 || resolution > 4096 || count < 0 || count > 8
                || (count > 0 && uint64_t(count) * uint64_t(resolution) * uint64_t(resolution) > maxPixels)) {
            LOGE("Unsupported texturing resource budget: %d textures at %d pixels; modern limits are size 16..4096, count 0..8, and 64 MiB total RGBA atlas storage", count, resolution);
            return false;
        }
#else
        const int simplification = ignoreConfig ? 5 : meshSimplification;
#endif
        if (verbose)
            event = "SIMPLIFY";
        Tango3DR_Config textureConfig = Tango3DR_Config_create(TANGO_3DR_CONFIG_TEXTURING);
        if (!textureConfig) { event.clear(); return false; }
        bool success = Tango3DR_Config_setInt32(textureConfig, "texturing_backend", TANGO_3DR_CPU_TEXTURING) == TANGO_3DR_SUCCESS
                && Tango3DR_Config_setInt32(textureConfig, "max_num_textures", count) == TANGO_3DR_SUCCESS
                && Tango3DR_Config_setInt32(textureConfig, "mesh_simplification_factor", simplification) == TANGO_3DR_SUCCESS
                && Tango3DR_Config_setInt32(textureConfig, "texture_size", resolution) == TANGO_3DR_SUCCESS;
        if (success && !ignoreConfig)
            success = Tango3DR_Config_setDouble(textureConfig, "min_resolution", 0.001 * scale) == TANGO_3DR_SUCCESS;
        if (success) {
            context = Tango3DR_TexturingContext_create(textureConfig, mesh);
            if (!context) LOGE("Texturing context creation failed: mesh, options, packing, or available memory rejected (vertices=%u, faces=%u, count=%d, size=%d)", mesh->num_vertices, mesh->num_faces, count, resolution);
        } else {
            LOGE("Texturing configuration rejected (count=%d, size=%d, simplification=%d)", count, resolution, simplification);
        }
        success = (Tango3DR_Config_destroy(textureConfig) == TANGO_3DR_SUCCESS) && success && context;
        if (!success) { ResetContext(); event = ""; }
        return success;
    }

    bool TangoTexturize::UpdatePoses(Dataset* dataset) {
        poses = width = height = 0;
        camera = {};
        if (!dataset) return false;
        dataset->ReadState(poses, width, height, cx, cy, fx, fy);
        if (poses < 0 || poses > 100000 || width <= 0 || height <= 0 || width > 8192 || height > 8192
                || !std::isfinite(cx) || !std::isfinite(cy) || !std::isfinite(fx) || !std::isfinite(fy)
                || fx <= 0 || fy <= 0) { poses = 0; return false; }

        camera.width = (uint32_t) width;
        camera.height = (uint32_t) height;
        camera.cx = cx;
        camera.cy = cy;
        camera.fx = fx;
        camera.fy = fy;

        std::vector<float> distortion = dataset->ReadDistortion();
        for (float value : distortion) if (!std::isfinite(value)) return false;
        ApplyDistortion(camera, distortion);

        if (context) {
            return Tango3DR_TexturingContext_setColorCalibration(context, &camera) == TANGO_3DR_SUCCESS;
        }
        return true;
    }

    bool TangoTexturize::SetCalibration(Tango3DR_ReconstructionContext context, Dataset* dataset, int scale) {
        if (!context || !dataset || scale <= 0) return false;
        Tango3DR_CameraCalibration camera = {};

        std::vector<float> distortion = dataset->ReadDistortion();
        ApplyDistortion(camera, distortion);

        int t, w, h;
        dataset->ReadState(t, w, h, camera.cx, camera.cy, camera.fx, camera.fy);
        if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || w / scale <= 0 || h / scale <= 0
                || !std::isfinite(camera.cx) || !std::isfinite(camera.cy)
                || !std::isfinite(camera.fx) || !std::isfinite(camera.fy) || camera.fx <= 0 || camera.fy <= 0) return false;
        camera.width = w / scale;
        camera.height = h / scale;
        camera.cx /= (float)scale;
        camera.cy /= (float)scale;
        camera.fx /= (float)scale;
        camera.fy /= (float)scale;
        return Tango3DR_ReconstructionContext_setColorCalibration(context, &camera) == TANGO_3DR_SUCCESS;
    }

    bool TangoTexturize::Truncate(int count, Dataset* dataset) {
        if (!dataset || count < 0 || !dataset->WriteState(count, width, height, camera.cx, camera.cy, camera.fx, camera.fy)) return false;
        poses = count;
        return true;
    }

    void TangoTexturize::ScaleMesh(Tango3DR_Mesh *mesh, float s) {
        for (unsigned int i = 0; i < mesh->num_vertices; i++) {
            mesh->vertices[i][0] *= s;
            mesh->vertices[i][1] *= s;
            mesh->vertices[i][2] *= s;
        }
    }
}
