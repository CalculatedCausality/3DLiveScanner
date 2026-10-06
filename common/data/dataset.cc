#include "data/dataset.h"
#include "data/image.h"
#include <cerrno>
#include <cmath>
#include <sstream>
#include <memory>
#include <new>
#include <unistd.h>

namespace oc {
    namespace {
        void DestroyPreviewMesh(Tango3DR_Mesh* mesh) {
#ifdef ANDROID
            Tango3DR_Mesh_destroy(mesh);
#else
            delete[] mesh->vertices;
            delete[] mesh->normals;
            delete[] mesh->colors;
            delete[] mesh->faces;
#endif
            delete mesh;
        }
    }

    Dataset::Dataset(std::string path) {
        dataset = path;
    }

    bool Dataset::ValidateCommittedFrames() {
        int count = 0, width = 0, height = 0;
        double cx = 0, cy = 0, fx = 0, fy = 0;
        ReadState(count, width, height, cx, cy, fx, fy);
        if ((count <= 0) || (count > 100000) || (width <= 0) || (height <= 0)
                || (width > 8192) || (height > 8192) || !std::isfinite(cx) || !std::isfinite(cy)
                || !std::isfinite(fx) || !std::isfinite(fy) || (fx <= 0) || (fy <= 0)) return false;

        for (int i = 0; i < count; i++) {
            std::vector<glm::mat4> poses;
            if (!ReadPose(i, poses)) return false;
            for (const glm::mat4& pose : poses)
                for (int column = 0; column < 4; column++)
                    for (int row = 0; row < 4; row++)
                        if (!std::isfinite(pose[column][row])) return false;

            Tango3DR_PointCloud points = ReadPointCloud(i);
            bool pointsValid = points.num_points > 0;
#ifdef ANDROID
            Tango3DR_PointCloud_destroy(&points);
#else
            delete[] points.points;
#endif
            if (!pointsValid) return false;

            bool previewValid = false;
            std::vector<std::pair<GridIndex, Tango3DR_Mesh *>> preview = ReadPreview(i, false, &previewValid);
            for (std::pair<GridIndex, Tango3DR_Mesh *>& p : preview) {
                DestroyPreviewMesh(p.second);
            }
            if (!previewValid) return false;

            double timestamp = 0;
            FILE* timestampFile = fopen(GetFileName(i, ".tms").c_str(), "r");
            bool timestampValid = timestampFile && (fscanf(timestampFile, "%lf", &timestamp) == 1)
                    && std::isfinite(timestamp);
            if (timestampFile) fclose(timestampFile);
            if (!timestampValid) return false;

            Image image(GetFileName(i, ".jpg"));
            if (!image.IsValid() || (image.GetWidth() != width) || (image.GetHeight() != height)) return false;
        }
        return true;
    }

    std::string Dataset::GetFileName(int index, std::string extension) {
        std::ostringstream ss;
        ss << index;
        std::string number = ss.str();
        while(number.size() < 8)
            number = "0" + number;
        return dataset + "/" + number + extension;
    }

    std::vector<float> Dataset::ReadDistortion() {
        std::vector<float> output;

        int size = 0;
        FILE* file = fopen((dataset + "/distortion.txt").c_str(), "r");
        if (file) {
            fscanf(file, "%d\n", &size);
            for (int i = 0; i < size; i++) {
                float value = 0;
                fscanf(file, "%f\n", &value);
                output.push_back(value);
            }
            fclose(file);
        } else {
            for (int i = 0; i < 3; i++) {
                output.push_back(0);
            }
        }

        return output;
    }

    std::vector<glm::mat4> Dataset::ReadPose(int index) {
        std::vector<glm::mat4> output;
        if (!ReadPose(index, output)) output.assign(MAX_CAMERA, glm::mat4(1));
        return output;
    }

    bool Dataset::ReadPose(int index, std::vector<glm::mat4>& output) {
        output.clear();
        FILE* file = fopen(GetFileName(index, ".mat").c_str(), "r");
        if (!file) return false;
        for (int i = 0; i < MAX_CAMERA; i++) {
            glm::mat4 mat(0);
            for (int j = 0; j < 4; j++) {
                if (fscanf(file, "%f %f %f %f\n", &mat[j][0], &mat[j][1], &mat[j][2], &mat[j][3]) != 4) {
                    output.clear();
                    fclose(file);
                    return false;
                }
            }
            output.push_back(mat);
        }
        fclose(file);
        return true;
    }

    void Dataset::ReadState(int &count, int &width, int &height, double& cx, double& cy, double& fx, double& fy) {
        count = width = height = 0;
        cx = cy = fx = fy = 0;
        FILE* file = fopen((dataset + "/state.txt").c_str(), "r");
        if (file) {
            if (fscanf(file, "%d %d %d %lf %lf %lf %lf\n",
                       &count, &width, &height, &cx, &cy, &fx, &fy) != 7) {
                count = width = height = 0;
                cx = cy = fx = fy = 0;
            }
            fclose(file);
        }
    }

    float Dataset::ReadYaw() {
        float yaw = -90;
        FILE* file = fopen((dataset + "/rotation.txt").c_str(), "r");
        if (file) {
            fscanf(file, "%f\n", &yaw);
            fclose(file);
        }
        return yaw;
    }

    bool Dataset::WriteDistortion(const std::vector<float>& data) {
        FILE* file = fopen((dataset + "/distortion.txt").c_str(), "w");
        if (!file) return false;
        bool success = fprintf(file, "%d\n", (int)data.size()) > 0;
        for (int i = 0; i < data.size(); i++) {
            success = success && (fprintf(file, "%f\n", data[i]) > 0);
        }
        success = (fclose(file) == 0) && success;
        return success;
    }

    bool Dataset::ResetState() {
        std::string state = dataset + "/state.txt";
        std::string temporary = state + ".tmp";
        remove(temporary.c_str());
        return (remove(state.c_str()) == 0) || (errno == ENOENT);
    }

    bool Dataset::WritePose(int index, const std::vector<glm::mat4>& pose) {
        FILE* file = fopen(GetFileName(index, ".mat").c_str(), "w");
        if (!file) return false;
        bool success = true;
        for (int k = 0; k < MAX_CAMERA; k++)
            for (int i = 0; i < 4; i++)
                success = success && (fprintf(file, "%f %f %f %f\n", pose[k][i][0], pose[k][i][1],
                                               pose[k][i][2], pose[k][i][3]) > 0);
        success = (fclose(file) == 0) && success;
        return success;
    }

    bool Dataset::WriteState(int count, int width, int height, double cx, double cy, double fx, double fy) {
        std::string path = dataset + "/state.txt";
        std::string temporary = path + ".tmp";
        FILE* file = fopen(temporary.c_str(), "w");
        if (!file) return false;
        bool success = fprintf(file, "%d %d %d %lf %lf %lf %lf\n",
                               count, width, height, cx, cy, fx, fy) > 0;
        success = (fclose(file) == 0) && success;
        if (success) success = rename(temporary.c_str(), path.c_str()) == 0;
        if (!success) remove(temporary.c_str());
        return success;
    }

    void Dataset::WriteYaw(float yaw) {
        FILE* file = fopen((dataset + "/rotation.txt").c_str(), "w");
        fprintf(file, "%f\n", yaw);
        fclose(file);
    }

    Tango3DR_PointCloud Dataset::ReadPointCloud(int index) {
        Tango3DR_PointCloud t3dr_depth = {};
#ifdef ANDROID
        Tango3DR_PointCloud_initEmpty(&t3dr_depth);
#endif
        FILE* file = fopen(GetFileName(index, ".pcl").c_str(), "rb");
        if (!file) return t3dr_depth;
        fseek(file, 0, SEEK_END);
        long file_size = ftell(file);
        rewind(file);
        uint32_t count = 0;
        if ((file_size < (long) sizeof(count)) || (fread(&count, sizeof(count), 1, file) != 1)
                || ((uint64_t) count * sizeof(Tango3DR_Vector4)
                    > (uint64_t) file_size - sizeof(count))) {
            fclose(file);
            return t3dr_depth;
        }

#ifdef ANDROID
        if (Tango3DR_PointCloud_init(count, &t3dr_depth) != TANGO_3DR_SUCCESS) {
            fclose(file);
            Tango3DR_PointCloud_initEmpty(&t3dr_depth);
            return t3dr_depth;
        }
#else
        t3dr_depth.num_points = count;
        t3dr_depth.points = new Tango3DR_Vector4[count];
#endif
        if (fread(t3dr_depth.points, sizeof(Tango3DR_Vector4), count, file) != count) {
#ifdef ANDROID
            Tango3DR_PointCloud_destroy(&t3dr_depth);
            Tango3DR_PointCloud_initEmpty(&t3dr_depth);
#else
            delete[] t3dr_depth.points;
            t3dr_depth.points = nullptr;
            t3dr_depth.num_points = 0;
#endif
        }
        fclose(file);
        return t3dr_depth;
    }

    bool Dataset::WritePointCloud(int index, Tango3DR_PointCloud t3dr_depth) {
        FILE* file = fopen(GetFileName(index, ".pcl").c_str(), "wb");
        if (!file) return false;
        bool success = fwrite(&t3dr_depth.num_points, sizeof(uint32_t), 1, file) == 1;
        success = success && (fwrite(t3dr_depth.points, sizeof(Tango3DR_Vector4),
                                     t3dr_depth.num_points, file) == t3dr_depth.num_points);
        success = (fclose(file) == 0) && success;
        return success;
    }

    std::vector<std::pair<GridIndex, Tango3DR_Mesh *> > Dataset::ReadPreview(int index, bool empty,
                                                                              bool* success) {
        if (success) *success = false;
        FILE* file = fopen(GetFileName(index, ".bin").c_str(), "rb");
        std::vector<std::pair<GridIndex, Tango3DR_Mesh *>> output;
        if (!file) return output;
        fseek(file, 0, SEEK_END);
        long file_size = ftell(file);
        rewind(file);
        int count = 0;
        if ((file_size < (long) sizeof(count)) || (fread(&count, sizeof(int), 1, file) != 1)
                || (count < 0)
                || ((uint64_t) count * sizeof(Tango3DR_GridIndex)
                    > (uint64_t) file_size - sizeof(count))) {
            fclose(file);
            return output;
        }
        output.reserve(count);
        for (int i = 0; i < count; i++) {
            std::pair<GridIndex, Tango3DR_Mesh *> p;
            p.second = nullptr;
            if (fread(p.first.indices, sizeof(Tango3DR_GridIndex), 1, file) != 1) {
                output.clear();
                fclose(file);
                return output;
            }
            output.push_back(p);
        }
        bool valid = true;
        for (int i = 0; i < count; i++) {
                uint32_t num_faces;
                uint32_t num_vertices;
                if ((fread(&num_faces, sizeof(uint32_t), 1, file) != 1)
                        || (fread(&num_vertices, sizeof(uint32_t), 1, file) != 1)) {
                    valid = false;
                    break;
                }
                uint64_t required = (uint64_t) num_vertices
                        * (sizeof(Tango3DR_Vector3) * 2 + sizeof(Tango3DR_Color))
                        + (uint64_t) num_faces * sizeof(Tango3DR_Face);
                long position = ftell(file);
                if ((position < 0) || (position > file_size)
                        || (required > (uint64_t) file_size - position)) {
                    valid = false;
                    break;
                }
                if (empty) {
                    if (fseek(file, (long) required, SEEK_CUR) != 0) valid = false;
                    if (!valid) break;
                    continue;
                }

                Tango3DR_Mesh* mesh = new Tango3DR_Mesh();
#ifdef ANDROID
                if (Tango3DR_Mesh_init(num_vertices, num_faces, true, true, false, false,
                                      0, 0, 0, mesh) != TANGO_3DR_SUCCESS) {
                    delete mesh;
                    valid = false;
                    break;
                }
                mesh->num_faces = num_faces;
                mesh->num_vertices = num_vertices;
#else
                mesh->num_faces = mesh->max_num_faces = num_faces;
                mesh->num_vertices = mesh->max_num_vertices = num_vertices;
                mesh->vertices = new Tango3DR_Vector3[num_vertices];
                mesh->normals = new Tango3DR_Vector3[num_vertices];
                mesh->colors = new Tango3DR_Color[num_vertices];
                mesh->faces = new Tango3DR_Face[num_faces];
#endif

                valid = (fread(mesh->vertices, sizeof(Tango3DR_Vector3), num_vertices, file) == num_vertices)
                        && (fread(mesh->normals, sizeof(Tango3DR_Vector3), num_vertices, file) == num_vertices)
                        && (fread(mesh->colors, sizeof(Tango3DR_Color), num_vertices, file) == num_vertices)
                        && (fread(mesh->faces, sizeof(Tango3DR_Face), num_faces, file) == num_faces);
                for (uint32_t face = 0; valid && (face < num_faces); face++) {
                    valid = (mesh->faces[face][0] < num_vertices)
                            && (mesh->faces[face][1] < num_vertices)
                            && (mesh->faces[face][2] < num_vertices);
                }
                if (!valid) {
                    DestroyPreviewMesh(mesh);
                    break;
                }
                output[i].second = mesh;
        }
        fclose(file);
        if (!valid) {
            for (std::pair<GridIndex, Tango3DR_Mesh *>& p : output) {
                if (!p.second) continue;
                DestroyPreviewMesh(p.second);
            }
            output.clear();
        }
        if (success) *success = valid;
        return output;
    }

    bool Dataset::WritePreview(int index, const std::vector<std::pair<GridIndex, Tango3DR_Mesh *>>& preview) {
        FILE* file = fopen(GetFileName(index, ".bin").c_str(), "wb");
        if (!file) return false;
        // Mesh segments produce many small headers/arrays. Coalesce them before
        // crossing the filesystem/FUSE boundary, retaining the final fsync and
        // exact existing file format. Allocation failure falls back to stdio.
        const size_t bufferSize = 256 * 1024;
        std::unique_ptr<char[]> buffer(new (std::nothrow) char[bufferSize]);
        if (buffer) setvbuf(file, buffer.get(), _IOFBF, bufferSize);
        int count = preview.size();
        bool success = fwrite(&count, sizeof(int), 1, file) == 1;
        for (const std::pair<GridIndex, Tango3DR_Mesh *>& p : preview) {
            success = success && (fwrite(p.first.indices, sizeof(Tango3DR_GridIndex), 1, file) == 1);
        }
        for (const std::pair<GridIndex, Tango3DR_Mesh *>& p : preview) {
            success = success && (fwrite(&p.second->num_faces, sizeof(uint32_t), 1, file) == 1);
            success = success && (fwrite(&p.second->num_vertices, sizeof(uint32_t), 1, file) == 1);
            success = success && (fwrite(p.second->vertices, sizeof(Tango3DR_Vector3),
                                         p.second->num_vertices, file) == p.second->num_vertices);
            success = success && (fwrite(p.second->normals, sizeof(Tango3DR_Vector3),
                                         p.second->num_vertices, file) == p.second->num_vertices);
            success = success && (fwrite(p.second->colors, sizeof(Tango3DR_Color),
                                         p.second->num_vertices, file) == p.second->num_vertices);
            success = success && (fwrite(p.second->faces, sizeof(Tango3DR_Face),
                                         p.second->num_faces, file) == p.second->num_faces);
        }
        success = success && (fflush(file) == 0) && (fsync(fileno(file)) == 0);
        success = (fclose(file) == 0) && success;
        return success;
    }
}
