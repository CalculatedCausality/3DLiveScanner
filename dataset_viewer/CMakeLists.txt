cmake_minimum_required(VERSION 3.0.2)

include(../common/sources.cmake)

include_directories(
    ../common
    ../third_party/tango_3d_reconstruction/include
)

add_executable(
    dataset_viewer
    ${SCANNER_DATA_SOURCES}
    ${SCANNER_VIEWER_SOURCES}
    app.cpp
)

target_link_libraries(
    dataset_viewer
    glut
    GL
    png
    turbojpeg
    X11
)
