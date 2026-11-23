# PSD and PSB: the flattened composite of Photoshop documents, with the ICC profile, Exif data, resolution and JPEG
# thumbnail of their image resources.
pr_library(psd INCLUDE include
    src/psd.cpp
)
target_link_libraries(pr_psd PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(psd
        tests/test_psd.cpp
        tests/test_psd_c.c
    )
endif()
