// Codec operations are out of scope and removed by --gc-sections. Only Image's
// process-wide codec initializers run. Compile against the bundled real header.
#include <turbojpeg.h>
extern "C" tjhandle tjInitCompress() { return nullptr; }
extern "C" tjhandle tjInitDecompress() { return nullptr; }
