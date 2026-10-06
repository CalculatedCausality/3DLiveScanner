// Image allocation/reference counting is needed by the real File3d OBJ loader.
// GPU and image codecs are outside this numerical alignment suite.
#include <data/image.h>
namespace oc {
Image::Image(unsigned char, unsigned char, unsigned char, unsigned char)
    : instances(1), width(1), height(1), data(nullptr), texture(-1) {}
Image::Image(std::string filename)
    : instances(1), width(1), height(1), data(nullptr), name(filename), texture(-1) {}
Image::~Image() {}
}
