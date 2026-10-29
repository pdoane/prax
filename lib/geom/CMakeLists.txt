# Geometry: rectangles and transforms in two dimensions.
pr_library(geom INCLUDE include
    src/geom.cpp
)
target_link_libraries(pr_geom PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(geom
        tests/test_geom.cpp
        tests/test_geom_c.c
    )
endif()
