# Shared source lists for the desktop tools.
set(SCANNER_DATA_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/data/dataset.cc"
    "${CMAKE_CURRENT_LIST_DIR}/data/file3d.cc"
    "${CMAKE_CURRENT_LIST_DIR}/data/image.cc"
    "${CMAKE_CURRENT_LIST_DIR}/data/mesh.cc"
)

set(SCANNER_GL_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/gl/camera.cc"
)

set(SCANNER_VIEWER_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/gl/glsl.cc"
)

set(SCANNER_EXTRACTOR_SOURCES
    "${CMAKE_CURRENT_LIST_DIR}/data/depthmap.cc"
    "${CMAKE_CURRENT_LIST_DIR}/editor/rasterizer.cc"
    "${CMAKE_CURRENT_LIST_DIR}/exporter/csvposes.cc"
    "${CMAKE_CURRENT_LIST_DIR}/exporter/depthmaps.cc"
    "${CMAKE_CURRENT_LIST_DIR}/exporter/exporter.cc"
    "${CMAKE_CURRENT_LIST_DIR}/exporter/floorpln.cc"
    "${CMAKE_CURRENT_LIST_DIR}/exporter/ply.cc"
    "${CMAKE_CURRENT_LIST_DIR}/postproc/texturize.cc"
)
