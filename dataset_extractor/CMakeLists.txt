cmake_minimum_required(VERSION 3.0.2)

include(../common/sources.cmake)

find_package(OpenCV REQUIRED COMPONENTS core imgproc)

include_directories(
    ../common
    ../third_party/tango_3d_reconstruction/include
    ${OpenCV_INCLUDE_DIRS}
)

add_executable(
    dataset_extractor
    ${SCANNER_DATA_SOURCES}
    ${SCANNER_GL_SOURCES}
    ${SCANNER_EXTRACTOR_SOURCES}
    app.cpp
)

target_link_libraries(
    dataset_extractor
    png
    turbojpeg
    ${OpenCV_LIBS}
)
