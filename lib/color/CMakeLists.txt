# Color: color values and spaces, conversion between them, HDR resolution, gamut mapping and ICC profiles.
pr_library(color INCLUDE include
    src/color.cpp
    src/color_profile.cpp
    src/color_space.cpp
)
target_link_libraries(pr_color PUBLIC prax::core PRIVATE prax::core_cpp prax::geom)

if (PR_DEVELOP)
    pr_module_tests(color
        tests/test_color_c.c
        tests/test_color_profile.cpp
        tests/test_color_space.cpp
    )
endif()
