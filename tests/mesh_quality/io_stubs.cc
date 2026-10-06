// Only external image I/O and the recorded-frame source are replaced. Tests link
// the production File3d, Mesh and ExporterPLY implementations without GL/codecs.
#include <data/image.h>
#include <exporter/exporter.h>

namespace oc {
Image::Image(unsigned char, unsigned char, unsigned char, unsigned char)
    : instances(1), width(1), height(1), data(nullptr), texture(-1) {}
Image::Image(std::string filename)
    : instances(1), width(1), height(1), data(nullptr), name(filename), texture(-1) {}
Image::~Image() {}

Dataset::Dataset(std::string path) : dataset(path) {}
std::string Dataset::GetFileName(int index, std::string extension) {
    return dataset + std::to_string(index) + extension;
}
int Exporter::GetPoseCount(Dataset*) { return 2; }
void Exporter::ConvertFrame(Dataset*, int index, int) {
    Mesh cloud;
    cloud.vertices.push_back(glm::vec3(index, 2, 3));
    cloud.colors.push_back(0x123456);
    std::vector<glm::mat4> poses;
    Process(cloud, poses, index);
}
}
