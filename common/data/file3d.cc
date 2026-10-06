#include "data/file3d.h"
#include <unistd.h>
#include <cmath>
#include <limits>
#include <sstream>
#include <set>
#include <utility>
#include <cctype>

namespace oc {
    namespace {
        bool Finite(const glm::vec3& v) {
            return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        }

        // Double intermediates avoid float underflow for small, real triangles.
        bool TriangleNormal(const glm::vec3& a, const glm::vec3& b, const glm::vec3& c,
                            glm::vec3* normal = nullptr) {
            if (!Finite(a) || !Finite(b) || !Finite(c)) return false;
            glm::dvec3 n = glm::cross(glm::dvec3(b) - glm::dvec3(a), glm::dvec3(c) - glm::dvec3(a));
            double length = glm::length(n);
            if (!(length > 0) || !std::isfinite(length)) return false;
            if (normal) *normal = glm::vec3(n / length);
            return true;
        }

        bool ReadLine(FILE* file, std::string& line) {
            line.clear();
            char buffer[1024];
            while (fgets(buffer, sizeof(buffer), file)) {
                line += buffer;
                if (!line.empty() && line.back() == '\n') return true;
            }
            return !line.empty();
        }

        size_t FaceIndex(const Mesh& mesh, size_t i) {
            return mesh.indices.empty() ? i : mesh.indices[i];
        }

        bool ObjIndex(const std::string& token, size_t count, size_t& index) {
            std::istringstream input(token);
            long long value;
            char extra;
            if (!(input >> value) || (input >> extra) || value == 0) return false;
            if (value > 0) {
                if (static_cast<unsigned long long>(value) > count) return false;
                index = static_cast<size_t>(value - 1);
            } else {
                if (value < -static_cast<long long>(count)) return false;
                index = static_cast<size_t>(static_cast<long long>(count) + value);
            }
            return true;
        }

        struct ObjCorner {
            size_t vertex = 0, uv = 0, normal = 0;
            bool hasUv = false, hasNormal = false;
        };

        bool ObjFace(const std::string& line, size_t vertices, size_t uvs, size_t normals,
                     std::vector<ObjCorner>& corners) {
            std::istringstream input(line);
            std::string token;
            input >> token;
            while (input >> token) {
                if (token[0] == '#') break;
                if (corners.size() == 4) return false; // Existing triangle/quad contract.
                ObjCorner corner;
                size_t slash = token.find('/');
                if (!ObjIndex(token.substr(0, slash), vertices, corner.vertex)) return false;
                if (slash != std::string::npos) {
                    size_t next = token.find('/', slash + 1);
                    std::string uv = token.substr(slash + 1, next == std::string::npos ? next : next - slash - 1);
                    if (!uv.empty()) {
                        corner.hasUv = true;
                        if (!ObjIndex(uv, uvs, corner.uv)) return false;
                    } else if (next == std::string::npos) return false;
                    if (next != std::string::npos) {
                        corner.hasNormal = true;
                        if (!ObjIndex(token.substr(next + 1), normals, corner.normal)) return false;
                    }
                }
                corners.push_back(corner);
            }
            return corners.size() == 3 || corners.size() == 4;
        }

        bool ValidFace(const Mesh& mesh, size_t i) {
            return TriangleNormal(mesh.vertices[FaceIndex(mesh, i)],
                                  mesh.vertices[FaceIndex(mesh, i + 1)],
                                  mesh.vertices[FaceIndex(mesh, i + 2)]);
        }
    }

    File3d::File3d(std::string filename, bool writeAccess) {
        path = filename;
        writeMode = writeAccess;
        vertexCount = 0;
        faceCount = 0;
        hasColors = hasNormals = false;
        validHeader = true;
        file = nullptr;
        type = OBJ;

        if (writeMode) {
            LOGI("Writing into %s", filename.c_str());
        } else {
            LOGI("Loading from %s", filename.c_str());
        }

        std::string ext = filename.size() >= 3 ? filename.substr(filename.size() - 3) : "";
        if (ext.compare("pcl") == 0)
            type = PCL;
        else if (ext.compare("ply") == 0)
            type = PLY;
        else if (ext.compare("obj") == 0)
            type = OBJ;
        else {
            validHeader = false;
            return;
        }

        if (writeMode)
            file = fopen(filename.c_str(), "w");
        else
            file = fopen(filename.c_str(), "r");
    }

    File3d::~File3d() {
        if (file) fclose(file);
    }

    void File3d::ReadModel(int subdivision, std::vector<Mesh>& output) {
        assert(!writeMode);
        if (!file) return;
        ReadHeader();
        if (!validHeader) return;
        if (type == PCL)
            ParsePCL(subdivision, output);
        else if (type == PLY)
            ParsePLY(subdivision, output);
        else if (type == OBJ)
            ParseOBJ(subdivision, output);
        else
            assert(false);
    }

    bool File3d::WriteModel(std::vector<Mesh>& model, bool extra) {
        assert(writeMode);
        if (!file) return false;
        // Validate before emitting a header. Mesh arrays remain flat or indexed as supplied.
        vertexCount = 0;
        faceCount = 0;
        hasNormals = hasColors = false;
        const bool faces = type == OBJ || extra;
        for (const Mesh& mesh : model) {
            const size_t count = mesh.vertices.size();
            if (count > std::numeric_limits<unsigned int>::max() - vertexCount) return false;
            if ((!mesh.normals.empty() && mesh.normals.size() != count) ||
                (!mesh.colors.empty() && mesh.colors.size() != count) ||
                (!mesh.uv.empty() && mesh.uv.size() != count)) return false;
            for (const glm::vec3& v : mesh.vertices) if (!Finite(v)) return false;
            for (const glm::vec3& n : mesh.normals) if (!Finite(n)) return false;
            for (const glm::vec2& uv : mesh.uv)
                if (!std::isfinite(uv.x) || !std::isfinite(uv.y)) return false;
            vertexCount += count;
            hasNormals |= !mesh.normals.empty();
            hasColors |= !mesh.colors.empty();
            if (!faces) continue;
            const size_t corners = mesh.indices.empty() ? count : mesh.indices.size();
            if (corners % 3 != 0) return false;
            for (unsigned int index : mesh.indices) if (index >= count) return false;
            for (size_t j = 0; j < corners; j += 3) {
                if (!ValidFace(mesh, j)) continue;
                if (faceCount == std::numeric_limits<unsigned int>::max()) return false;
                ++faceCount;
            }
        }
        //write
        if ((type == PLY) || (type == OBJ)) {
            if (!WriteHeader(model)) return false;
            for (unsigned int i = 0; i < model.size(); i++)
                WritePointCloud(model[i]);
            if ((type == OBJ) || extra) {
                size_t offset = (type == OBJ) ? 1 : 0;
                size_t normalOffset = 1, uvOffset = 1;
                for (unsigned int i = 0; i < model.size(); i++) {
                    if (model[i].vertices.empty())
                        continue;
                    if (type == OBJ) {
                        fprintf(file, "usemtl %u\n", i);
                    }
                    WriteFaces(model[i], offset, normalOffset, uvOffset);
                    offset += model[i].vertices.size();
                    normalOffset += model[i].normals.size();
                    uvOffset += model[i].uv.size();
                }
            }
        } else
            assert(false);
        return (ferror(file) == 0) && (fflush(file) == 0) && (fsync(fileno(file)) == 0);
    }

    void File3d::CleanStr(std::string& str) {
        while(!str.empty()) {
            char c = str[str.size() - 1];
            if (isspace(static_cast<unsigned char>(c)))
                str = str.substr(0, str.size() - 1);
            else
                break;
        }
    }

    unsigned int File3d::CodeColor(glm::ivec3 c) {
        unsigned int output = 0;
        output += glm::clamp(c.r, 0, 255);
        output += glm::clamp(c.g, 0, 255) << 8;
        output += glm::clamp(c.b, 0, 255) << 16;
        return output;
    }

    glm::ivec3 File3d::DecodeColor(unsigned int c) {
        glm::ivec3 output;
        output.r = (c & 0x000000FF);
        output.g = (c & 0x0000FF00) >> 8;
        output.b = (c & 0x00FF0000) >> 16;
        return output;
    }

    void File3d::ParseOBJ(int subdivision, std::vector<Mesh> &output) {
        unsigned long meshIndex = 0;
        glm::vec3 v;
        glm::vec3 n;
        glm::vec2 t;
        std::string lastKey;
        std::deque<glm::vec3> vertices;
        std::deque<glm::vec3> normals;
        std::deque<glm::vec2> uvs;
        std::map<std::string, Image*> images;

        //dummy material
        std::string key;
        meshIndex = output.size();
        output.push_back(Mesh());
        images[key] = new Image(255, 255, 255, 255);
        output[meshIndex].imageOwner = true;
        output[meshIndex].image = images[key];
        lastKey = key;

        //parse
        std::string sbuf;
        while (ReadLine(file, sbuf)) {
            while(!sbuf.empty() && isspace(static_cast<unsigned char>(sbuf[0]))) {
                sbuf = sbuf.substr(1);
            }
            if (sbuf.empty()) continue;
            if (StartsWith(sbuf, "usemtl")) {
                key = sbuf.substr(7);
                CleanStr(key);
                if (lastKey.empty() || (lastKey.compare(key) != 0)) {
                    meshIndex = output.size();
                    output.push_back(Mesh());
                    if (images.find(key) == images.end()) {
                        std::string imagefile = keyToFile[key];
                        if (imagefile.empty())
                        {
                            glm::vec3 color = keyToColor.count(key) ? keyToColor[key] : glm::vec3(1);
                            color = glm::clamp(color, 0.0f, 1.0f);
                            unsigned char r = (unsigned char) (255 * color.r);
                            unsigned char g = (unsigned char) (255 * color.g);
                            unsigned char b = (unsigned char) (255 * color.b);
                            images[key] = new Image(r, g, b, 255);
                        }
                        else
                          images[key] = new Image(imagefile);
                        output[meshIndex].imageOwner = true;
                    } else {
                        output[meshIndex].imageOwner = false;
                        images[key]->AddInstance();
                    }
                    output[meshIndex].image = images[key];
                    lastKey = key;
                }
            } else if (StartsWith(sbuf, "v")) {
                v = glm::vec3(std::numeric_limits<float>::quiet_NaN());
                sscanf(sbuf.c_str(), "v %f %f %f", &v.x, &v.y, &v.z);
                vertices.push_back(v);
            } else if (StartsWith(sbuf, "vt")) {
                t = glm::vec2(std::numeric_limits<float>::quiet_NaN());
                sscanf(sbuf.c_str(), "vt %f %f", &t.x, &t.y);
                uvs.push_back(t);
            } else if (StartsWith(sbuf, "vn")) {
                n = glm::vec3(std::numeric_limits<float>::quiet_NaN());
                sscanf(sbuf.c_str(), "vn %f %f %f", &n.x, &n.y, &n.z);
                normals.push_back(n);
            } else if (StartsWith(sbuf, "f")) {
                std::vector<ObjCorner> corners;
                if (!ObjFace(sbuf, vertices.size(), uvs.size(), normals.size(), corners)) continue;
                bool valid = true;
                for (const ObjCorner& corner : corners) {
                    if (!Finite(vertices[corner.vertex]) ||
                        (corner.hasNormal && !Finite(normals[corner.normal])) ||
                        (corner.hasUv && (!std::isfinite(uvs[corner.uv].x) || !std::isfinite(uvs[corner.uv].y))))
                        valid = false;
                }
                if (!valid) continue;
                const size_t order[] = {0, 1, 2, 3, 0, 2};
                for (size_t face = 0; face < (corners.size() == 4 ? 2u : 1u); ++face) {
                    glm::vec3 normal;
                    if (!TriangleNormal(vertices[corners[order[face * 3]].vertex],
                                        vertices[corners[order[face * 3 + 1]].vertex],
                                        vertices[corners[order[face * 3 + 2]].vertex], &normal)) continue;
                    if (subdivision > 0 && output[meshIndex].vertices.size() / 3 >= static_cast<size_t>(subdivision)) {
                        meshIndex = output.size();
                        output.push_back(Mesh());
                        output[meshIndex].image = images[lastKey];
                        output[meshIndex].image->AddInstance();
                        output[meshIndex].imageOwner = false;
                    }
                    Mesh& mesh = output[meshIndex];
                    for (size_t j = 0; j < 3; ++j) {
                        const ObjCorner& corner = corners[order[face * 3 + j]];
                        mesh.vertices.push_back(vertices[corner.vertex]);
                        mesh.normals.push_back(corner.hasNormal ? normals[corner.normal] : normal);
                        mesh.uv.push_back(corner.hasUv ? uvs[corner.uv] : glm::vec2(0));
                        mesh.colors.push_back(0);
                    }
                }
            }
        }
    }

    void File3d::ParsePCL(int subdivision, std::vector<Mesh> &output) {
        assert(!writeMode);
        glm::vec3 a;
        float w;
        //load vertices
        Mesh m;
        for (unsigned int i = 0; i < vertexCount; i++) {
            fscanf(file, "%f %f %f %f\n", &a.x, &a.y, &a.z, &w);
            m.vertices.push_back(a);
            m.colors.push_back(256 * (int)(w * 255));
        }
        output.push_back(m);
    }

    void File3d::ParsePLY(int subdivision, std::vector<Mesh> &output) {
        assert(!writeMode);
        Mesh source;
        std::string line;
        // Follow declared scalar property order, including unknown scalar properties.
        for (unsigned int i = 0; i < vertexCount; ++i) {
            if (!ReadLine(file, line)) return;
            std::istringstream input(line);
            glm::vec3 v(0), n(0);
            glm::ivec3 color(0);
            for (const std::string& property : vertexProperties) {
                double value;
                if (!(input >> value) || !std::isfinite(value) ||
                    std::abs(value) > std::numeric_limits<float>::max()) return;
                if (property == "x") v.x = value;
                else if (property == "y") v.y = value;
                else if (property == "z") v.z = value;
                else if (property == "nx") n.x = value;
                else if (property == "ny") n.y = value;
                else if (property == "nz") n.z = value;
                else if (property == "red") color.r = glm::clamp(value, 0.0, 255.0);
                else if (property == "green") color.g = glm::clamp(value, 0.0, 255.0);
                else if (property == "blue") color.b = glm::clamp(value, 0.0, 255.0);
            }
            source.vertices.push_back(v);
            if (hasNormals) source.normals.push_back(n);
            if (hasColors) source.colors.push_back(CodeColor(color));
        }
        if (subdivision == -1 || faceCount == 0) {
            output.push_back(std::move(source));
            return;
        }

        // Accumulate only across shared source indices, never rounded positions.
        // Two sequential face passes avoid a string map and a per-corner remap array.
        fpos_t facesStart;
        if (fgetpos(file, &facesStart) != 0) return;
        std::vector<glm::dvec3> sums;
        if (!hasNormals) sums.resize(source.vertices.size(), glm::dvec3(0));
        size_t meshIndex = output.size();
        for (int pass = hasNormals ? 1 : 0; pass < 2; ++pass) {
            for (unsigned int face = 0; face < faceCount; ++face) {
                if (!ReadLine(file, line)) break;
                std::istringstream input(line);
                long long count, a, b, c;
                if (!(input >> count) || count != 3 || !(input >> a >> b >> c)) continue;
                if (a < 0 || b < 0 || c < 0 || static_cast<unsigned long long>(a) >= source.vertices.size() ||
                    static_cast<unsigned long long>(b) >= source.vertices.size() ||
                    static_cast<unsigned long long>(c) >= source.vertices.size()) continue;
                glm::vec3 normal;
                if (!TriangleNormal(source.vertices[a], source.vertices[b], source.vertices[c], &normal)) continue;
                const size_t indices[] = {static_cast<size_t>(a), static_cast<size_t>(b), static_cast<size_t>(c)};
                if (pass == 0) {
                    for (size_t i : indices) sums[i] += glm::dvec3(normal);
                    continue;
                }
                if (meshIndex == output.size() || (subdivision > 0 &&
                    output[meshIndex].vertices.size() / 3 >= static_cast<size_t>(subdivision))) {
                    meshIndex = output.size();
                    output.push_back(Mesh());
                }
                Mesh& mesh = output[meshIndex];
                for (size_t i : indices) {
                    mesh.vertices.push_back(source.vertices[i]);
                    if (hasNormals) mesh.normals.push_back(source.normals[i]);
                    else {
                        double length = glm::length(sums[i]);
                        mesh.normals.push_back(length > 0 ? glm::vec3(sums[i] / length) : normal);
                    }
                    mesh.colors.push_back(hasColors ? source.colors[i] : 0);
                    mesh.uv.push_back(glm::vec2(0));
                }
            }
            if (pass == 0 && fsetpos(file, &facesStart) != 0) return;
        }
    }

    void File3d::ReadHeader() {
        char buffer[1024];
        if (type == PCL) {
            if (fscanf(file, "%u\n", &vertexCount) != 1) validHeader = false;
        } else if (type == PLY) {
            faceCount = 0;
            hasColors = hasNormals = false;
            vertexProperties.clear();
            validHeader = false;
            bool ascii = false, vertices = false, faces = false, faceIndices = false;
            std::string line, element;
            if (!ReadLine(file, line)) return;
            CleanStr(line);
            if (line != "ply") return;
            while (ReadLine(file, line)) {
                std::istringstream input(line);
                std::string tag;
                input >> tag;
                if (tag == "format") {
                    std::string format, version;
                    input >> format >> version;
                    ascii = format == "ascii" && version == "1.0";
                } else if (tag == "element") {
                    long long count;
                    if (!(input >> element >> count) || count < 0 ||
                        static_cast<unsigned long long>(count) > std::numeric_limits<unsigned int>::max()) return;
                    if (element == "vertex" && !vertices && !faces) {
                        vertexCount = count;
                        vertices = true;
                    } else if (element == "face" && vertices && !faces) {
                        faceCount = count;
                        faces = true;
                    } else return; // Unsupported element layout; never reinterpret its data.
                } else if (tag == "property") {
                    std::string type, name;
                    if (!(input >> type >> name)) return;
                    if (element == "vertex" && type != "list") vertexProperties.push_back(name);
                    else if (element == "face" && type == "list" && !faceIndices) {
                        std::string indexType, property;
                        if (!(input >> indexType >> property) ||
                            (property != "vertex_indices" && property != "vertex_index")) return;
                        faceIndices = true;
                    } else if (element != "face" || type == "list" || !faceIndices) return;
                } else if (tag == "end_header") {
                    validHeader = ascii && vertices && (faceCount == 0 || faceIndices);
                    break;
                }
            }
            std::set<std::string> properties(vertexProperties.begin(), vertexProperties.end());
            validHeader = validHeader && properties.size() == vertexProperties.size() &&
                          properties.count("x") && properties.count("y") && properties.count("z");
            const size_t normals = properties.count("nx") + properties.count("ny") + properties.count("nz");
            const size_t colors = properties.count("red") + properties.count("green") + properties.count("blue");
            if ((normals != 0 && normals != 3) || (colors != 0 && colors != 3)) validHeader = false;
            hasNormals = normals == 3;
            hasColors = colors == 3;
        } else if (type == OBJ) {
            std::string mtlFile;
            while (true) {
                if (!fgets(buffer, 1024, file))
                    break;
                std::string sbuf = buffer;
                while(!sbuf.empty() && isspace(static_cast<unsigned char>(sbuf[0]))) {
                    sbuf = sbuf.substr(1);
                }
                if (StartsWith(sbuf, "mtllib")) {
                    mtlFile = sbuf.substr(7);
                    CleanStr(mtlFile);
                    break;
                }
            }
            size_t index = path.find_last_of('/');
            std::string key, imgFile;
            std::string data = index == std::string::npos ? "" : path.substr(0, index + 1);
            std::string filepath = mtlFile;
            if (!mtlFile.empty())
            {
                if (mtlFile[0] != '/') {
                    filepath = data + mtlFile;
                }
                LOGI("Loading material %s", filepath.c_str());
                FILE* mtl = fopen(filepath.c_str(), "r");
                while (mtl) {
                    if (!fgets(buffer, 1024, mtl))
                        break;
                    std::string sbuf = buffer;
                    while(!sbuf.empty() && isspace(static_cast<unsigned char>(sbuf[0]))) {
                        sbuf = sbuf.substr(1);
                    }
                    if (StartsWith(sbuf, "newmtl")) {
                        key = sbuf.substr(7);
                        CleanStr(key);
                    }
                    if (StartsWith(sbuf, "Kd")) {
                        glm::vec3 color;
                        if (sscanf(sbuf.c_str(), "Kd %f %f %f", &color.r, &color.g, &color.b) == 3 && Finite(color))
                            keyToColor[key] = color;
                    }
                    if (StartsWith(sbuf, "map_Kd")) {
                        imgFile = sbuf.substr(7);
                        if (imgFile.empty()) continue;
                        if (imgFile[0] != '/') {
                            imgFile = data + imgFile;
                        }
                        CleanStr(imgFile);
                        keyToFile[key] = imgFile;
                    }
              }
              if (mtl) fclose(mtl);
            }
            // mtllib is allowed after vertex records. Always read geometry from the start.
            rewind(file);
        } else
            assert(false);
    }

    bool File3d::StartsWith(const std::string& s, const std::string& e) {
        return s.size() > e.size() && s.compare(0, e.size(), e) == 0 &&
               isspace(static_cast<unsigned char>(s[e.size()]));
    }

    bool File3d::WriteHeader(std::vector<Mesh>& model) {
        if (type == PLY) {
            fprintf(file, "ply\nformat ascii 1.0\ncomment ---\n");
            fprintf(file, "element vertex %u\n", vertexCount);
            fprintf(file, "property float x\n");
            fprintf(file, "property float y\n");
            fprintf(file, "property float z\n");
            if (hasNormals) {
                fprintf(file, "property float nx\n");
                fprintf(file, "property float ny\n");
                fprintf(file, "property float nz\n");
            }
            if (hasColors) {
                fprintf(file, "property uchar red\n");
                fprintf(file, "property uchar green\n");
                fprintf(file, "property uchar blue\n");
            }
            fprintf(file, "element face %u\n", faceCount);
            fprintf(file, "property list uchar uint vertex_indices\n");
            fprintf(file, "end_header\n");
        } else if (type == OBJ) {
            std::string base = (path.substr(0, path.length() - 4));
            size_t index = base.find_last_of('/');
            std::string shortBase = base.substr(index == std::string::npos ? 0 : index + 1);
            fprintf(file, "mtllib %s.mtl\n", shortBase.c_str());
            FILE* mtl = fopen((base + ".mtl").c_str(), "w");
            if (!mtl) return false;
            for (unsigned int i = 0; i < model.size(); i++) {
                if (model[i].vertices.empty())
                    continue;

                fprintf(mtl, "newmtl %u\n", i);
                fprintf(mtl, "Ns 96.078431\n");
                fprintf(mtl, "Ka 1.000000 1.000000 1.000000\n");
                fprintf(mtl, "Kd 0.640000 0.640000 0.640000\n");
                fprintf(mtl, "Ks 0.500000 0.500000 0.500000\n");
                fprintf(mtl, "Ke 0.000000 0.000000 0.000000\n");
                fprintf(mtl, "Ni 1.000000\n");
                fprintf(mtl, "d 1.000000\n");
                fprintf(mtl, "illum 2\n");
                //write texture name
                std::string name = model[i].image ? model[i].image->GetName() : "";


                if (!name.empty()) {
                    size_t slash = name.find_last_of('/');
                    fprintf(mtl, "map_Kd %s\n\n",
                            name.substr(slash == std::string::npos ? 0 : slash + 1).c_str());
                }
            }
            bool ok = ferror(mtl) == 0;
            if (fclose(mtl) != 0) ok = false;
            if (!ok) return false;
        }
        return ferror(file) == 0;
    }


    void File3d::WritePointCloud(Mesh& mesh) {
        for(size_t j = 0; j < mesh.vertices.size(); j++) {
            const glm::vec3& v = mesh.vertices[j];
            if (type == PLY) {
                fprintf(file, "%.9g %.9g %.9g", v.x, v.y, v.z);
                if (hasNormals) {
                    glm::vec3 n = mesh.normals.empty() ? glm::vec3(0) : mesh.normals[j];
                    fprintf(file, " %.9g %.9g %.9g", n.x, n.y, n.z);
                }
                if (hasColors) {
                    glm::ivec3 c = mesh.colors.empty() ? glm::ivec3(0) : DecodeColor(mesh.colors[j]);
                    fprintf(file, " %d %d %d", c.r, c.g, c.b);
                }
                fprintf(file, "\n");
            } else if (type == OBJ) {
                fprintf(file, "v %.9g %.9g %.9g\n", v.x, v.y, v.z);
                if (!mesh.normals.empty())
                    fprintf(file, "vn %.9g %.9g %.9g\n", mesh.normals[j].x, mesh.normals[j].y, mesh.normals[j].z);
                if (!mesh.uv.empty())
                    fprintf(file, "vt %.9g %.9g\n", mesh.uv[j].x, mesh.uv[j].y);
            }
        }
    }


    void File3d::WriteFaces(Mesh& mesh, size_t offset, size_t normalOffset, size_t uvOffset) {
        const size_t count = mesh.indices.empty() ? mesh.vertices.size() : mesh.indices.size();
        for (size_t j = 0; j < count; j += 3) {
            if (!ValidFace(mesh, j)) continue;
            fprintf(file, type == PLY ? "3" : "f");
            for (size_t k = 0; k < 3; ++k) {
                const size_t i = FaceIndex(mesh, j + k);
                fprintf(file, " %zu", i + offset);
                if (type != OBJ) continue;
                if (!mesh.uv.empty()) fprintf(file, "/%zu", i + uvOffset);
                else if (!mesh.normals.empty()) fprintf(file, "/");
                if (!mesh.normals.empty()) fprintf(file, "/%zu", i + normalOffset);
            }
            fprintf(file, "\n");
        }
    }
}
