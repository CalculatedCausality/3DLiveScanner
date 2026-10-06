LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)
LOCAL_MODULE := scanner_reconstruction
LOCAL_SRC_FILES := core.cc texturing.cc dataset_texturing.cc
LOCAL_STATIC_LIBRARIES := jpeg-turbo png
LOCAL_C_INCLUDES := $(LOCAL_PATH)/../common \
                    $(LOCAL_PATH)/../third_party/glm \
                    $(LOCAL_PATH)/../third_party/tango_3d_reconstruction/include
LOCAL_EXPORT_C_INCLUDES := $(LOCAL_PATH) \
                           $(LOCAL_PATH)/../third_party/tango_3d_reconstruction/include
LOCAL_CPPFLAGS := -std=c++11 -fexceptions -frtti -DSCANNER_MODERN=1
# Codec modules export turbojpeg.h/png.h and libpng's zlib link requirement.
# File3d/Image references intentionally resolve from the parent lib3dscanner.
include $(BUILD_STATIC_LIBRARY)
