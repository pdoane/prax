# Paths: contours of lines and Bézier curves, and the operations on them.
pr_library(path INCLUDE include
    src/arrangement.cpp
    src/cells.cpp
    src/flatten.cpp
    src/path_measure.cpp
    src/path.cpp
    src/path_data.cpp
    src/path_polyline.cpp
    src/path_rasterizer.cpp
    src/path_sparse.cpp
    src/path_stroke.cpp
    src/path_tessellator.cpp
)
target_link_libraries(pr_path PUBLIC prax::geom PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(path
        tests/test_path_measure.cpp
        tests/test_path.cpp
        tests/test_path_c.c
        tests/test_path_data.cpp
        tests/test_path_polyline.cpp
        tests/test_path_rasterizer.cpp
        tests/test_path_stroke.cpp
        tests/test_path_sparse.cpp
        tests/test_path_tessellator.cpp
    )
endif()
