LOCAL_PATH := $(call my-dir)
PROJECT_ROOT:= $(call my-dir)/../../../../..
SCANNER_MODERN ?= 1
SCANNER_QUALITY ?= 0

ifeq ($(SCANNER_QUALITY),1)
include $(CLEAR_VARS)
LOCAL_MODULE := scanner_quality_bridge
LOCAL_SRC_FILES := ../../quality/jni/bridge.cc
LOCAL_CPPFLAGS := -std=c++11 -fexceptions -O2
LOCAL_LDLIBS := -ldl -llog
LOCAL_LDFLAGS := -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384
include $(BUILD_SHARED_LIBRARY)
else

include $(CLEAR_VARS)
LOCAL_MODULE           := lib3dscanner
LOCAL_CFLAGS           := -DCERES_FOUND=1 -DSCANNER_MODERN=$(SCANNER_MODERN)
LOCAL_SHARED_LIBRARIES := arcore
LOCAL_STATIC_LIBRARIES := jpeg-turbo png poisson opencv_features2d opencv_imgproc opencv_core
# Both LOAD alignment and the end of GNU_RELRO must support 16 KiB pages.
LOCAL_LDFLAGS += -Wl,-z,max-page-size=16384 -Wl,-z,common-page-size=16384 -Wl,-z,relro,-z,now

LOCAL_C_INCLUDES := \
                    $(PROJECT_ROOT)/third_party/delaunay/ \
                    $(PROJECT_ROOT)/third_party/glm/ \
                    $(PROJECT_ROOT)/third_party/libjpeg-turbo/include/ \
                    $(PROJECT_ROOT)/third_party/libpng/include/ \
                    $(PROJECT_ROOT)/third_party/opencv/include/ \
                    $(PROJECT_ROOT)/common/

LOCAL_SRC_FILES := ../../../../../common/arcore/arcore.cc \
                   ../../../../../common/arcore/camera.cc \
                   ../../../../../common/arcore/service.cc \
                   ../../../../../common/data/dataset.cc \
                   ../../../../../common/data/depthmap.cc \
                   ../../../../../common/data/file3d.cc \
                   ../../../../../common/data/image.cc \
                   ../../../../../common/data/mesh.cc \
                   ../../../../../common/editor/effector.cc \
                   ../../../../../common/editor/rasterizer.cc \
                   ../../../../../common/editor/selector.cc \
                   ../../../../../common/exporter/csvposes.cc \
                   ../../../../../common/exporter/exporter.cc \
                   ../../../../../common/exporter/floorpln.cc \
                   ../../../../../common/gl/camera.cc \
                   ../../../../../common/gl/glsl.cc \
                   ../../../../../common/gl/renderer.cc \
                   ../../../../../common/gl/scene.cc \
                   ../../../../../common/postproc/optimizer.cc \
                   ../../../../../common/postproc/poisson.cc \
                   ../../../../../common/postproc/texturize.cc \
                   ../../../../../common/tango/retango.cc \
                   ../../../../../common/tango/scan.cc \
                   ../../../../../common/tango/texturize.cc \
                   ../../../../../common/thread/reconstr.cc \
                   ../../../../../common/thread/scene.cc \
                   app.cc \
                   renderer.cc

ifeq ($(SCANNER_MODERN),0)
LOCAL_SHARED_LIBRARIES += arengine tango_3d_reconstruction
LOCAL_SRC_FILES += ../../../../../common/arcore/arengine.cc
else
LOCAL_STATIC_LIBRARIES += scanner_reconstruction
LOCAL_SRC_FILES += ../../../../../common/depth/experimental.cc
endif

LOCAL_LDLIBS    := -llog -lGLESv2 -L$(SYSROOT)/usr/lib -lz -landroid -lmediandk -ldl
LOCAL_DISABLE_FATAL_LINKER_WARNINGS := true


include $(BUILD_SHARED_LIBRARY)

$(call import-add-path, $(PROJECT_ROOT))
$(call import-add-path, $(PROJECT_ROOT)/third_party)
$(call import-module,arcore)
ifeq ($(SCANNER_MODERN),0)
$(call import-module,arengine)
$(call import-module,tango_3d_reconstruction)
else
$(call import-module,reconstruction)
endif
$(call import-module,libjpeg-turbo)
$(call import-module,libpng)
$(call import-module,opencv)
$(call import-module,poisson)
endif
