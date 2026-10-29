# Images: the interface, and the backend that the build turns on.
pr_library(image INCLUDE include
    src/convert.cpp
    src/image.cpp
    src/image_png.cpp
    src/image_process.cpp
)
target_link_libraries(pr_image PUBLIC prax::core prax::color prax::compress PRIVATE prax::core_cpp)

if (PR_IMAGE_BUILTIN)
    target_sources(pr_image PRIVATE
        backends/builtin/src/image_builtin.cpp
        backends/builtin/src/gif.cpp
        backends/builtin/src/ico.cpp
        backends/builtin/src/jpeg.cpp
        backends/builtin/src/metadata.cpp
        backends/builtin/src/png.cpp
        backends/builtin/src/tiff.cpp
    )
    pr_headers(pr_image PUBLIC backends/builtin/include)
    target_compile_definitions(pr_image PRIVATE PR_IMAGE_HAS_BUILTIN)
endif()

if (PR_DEVELOP)
    pr_module_tests(image
        tests/test_convert.cpp
        tests/test_image.cpp
        tests/test_image_png.cpp
        tests/test_image_process.cpp
        tests/test_image_c.c
    )
    target_link_libraries(image_tests PRIVATE prax::color prax::compress)
    if (PR_IMAGE_BUILTIN)
        target_sources(image_tests PRIVATE
            backends/builtin/tests/test_image_builtin.cpp
            backends/builtin/tests/test_image_builtin_c.c
            backends/builtin/tests/test_gif.cpp
            backends/builtin/tests/test_ico.cpp
            backends/builtin/tests/test_jpeg.cpp
            backends/builtin/tests/test_tiff.cpp
            backends/builtin/tests/test_png.cpp
        )
    endif()
endif()
