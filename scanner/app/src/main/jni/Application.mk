APP_STL      := c++_shared
APP_CPPFLAGS += -std=c++11 -fexceptions -frtti
# Capture is CPU-heavy even in test APKs. Override ndk-build's debug -O0 for
# both reconstruction C++ and image-codec C while retaining debug assertions
# and symbols. Do not use fast-math: geometry validation depends on IEEE values.
APP_CFLAGS += -O2
