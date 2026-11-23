# Images: the interface, its sets of codecs, the conversions between pixel formats, and the codecs that adapt the image
# format libraries, one backend for each. A program carries only the codecs that it creates, and so only their format
# libraries.
pr_library(image INCLUDE include
    src/convert.cpp
    src/image.cpp
    src/image_exif.cpp
    src/image_process.cpp
    src/image_texture_source.cpp
)
target_link_libraries(pr_image PUBLIC prax::core prax::color prax::compress prax::texture
                               PRIVATE prax::core_cpp)

# What the codecs share: an image's input in memory, the meaning of color code points, and Exif metadata.
target_sources(pr_image PRIVATE
    backends/shared/src/image_shared.cpp
    backends/shared/src/metadata.cpp
)

# Prax's own codecs, one for each format, which the build includes together.
pr_backend(image builtin INCLUDE backends/png/include SOURCES
    backends/bmp/src/image_bmp.cpp
    backends/dds/src/image_dds.cpp
    backends/emjc/src/image_emjc.cpp
    backends/exr/src/image_exr.cpp
    backends/gif/src/image_gif.cpp
    backends/hdr/src/image_hdr.cpp
    backends/ico/src/image_ico.cpp
    backends/jpeg/src/image_jpeg.cpp
    backends/ktx/src/image_ktx.cpp
    backends/png/src/image_png.cpp
    backends/psd/src/image_psd.cpp
    backends/qoi/src/image_qoi.cpp
    backends/tga/src/image_tga.cpp
    backends/tiff/src/image_tiff.cpp
    backends/webp/src/image_webp.cpp
)
if (PR_IMAGE_BUILTIN)
    pr_headers(pr_image PUBLIC backends/bmp/include)
    pr_headers(pr_image PUBLIC backends/dds/include)
    pr_headers(pr_image PUBLIC backends/emjc/include)
    pr_headers(pr_image PUBLIC backends/exr/include)
    pr_headers(pr_image PUBLIC backends/gif/include)
    pr_headers(pr_image PUBLIC backends/hdr/include)
    pr_headers(pr_image PUBLIC backends/ico/include)
    pr_headers(pr_image PUBLIC backends/jpeg/include)
    pr_headers(pr_image PUBLIC backends/ktx/include)
    pr_headers(pr_image PUBLIC backends/psd/include)
    pr_headers(pr_image PUBLIC backends/qoi/include)
    pr_headers(pr_image PUBLIC backends/tga/include)
    pr_headers(pr_image PUBLIC backends/tiff/include)
    pr_headers(pr_image PUBLIC backends/webp/include)
    target_link_libraries(pr_image PRIVATE prax::bmp prax::dds prax::emjc prax::exr prax::gif prax::hdr prax::ico
                                           prax::jpeg prax::ktx prax::png prax::psd prax::qoi prax::tga prax::tiff prax::webp)
endif()

# The codec of Apple's ImageIO, which a program chooses over Prax's own for a format.
pr_backend(image imageio INCLUDE backends/imageio/include SOURCES backends/imageio/src/image_imageio.cpp)
if (PR_IMAGE_IMAGEIO)
    target_link_libraries(pr_image PRIVATE "-framework ImageIO" "-framework CoreGraphics" "-framework CoreFoundation")
endif()

# The selector: the choice of the backend of each format, and a set of every format.
pr_select(image INCLUDE select/include SOURCES select/src/image_select.cpp LINK prax::compress)

if (PR_DEVELOP)
    pr_module_tests(image
        tests/test_convert.cpp
        tests/test_image.cpp
        tests/test_image_process.cpp
        tests/test_image_c.c
        backends/shared/tests/test_image_codecs.cpp
        backends/shared/tests/test_image_codecs_c.c
        backends/bmp/tests/test_image_bmp.cpp
        backends/dds/tests/test_image_dds.cpp
        backends/emjc/tests/test_image_emjc.cpp
        backends/exr/tests/test_image_exr.cpp
        backends/gif/tests/test_image_gif.cpp
        backends/hdr/tests/test_image_hdr.cpp
        backends/ico/tests/test_image_ico.cpp
        backends/jpeg/tests/test_image_jpeg.cpp
        backends/ktx/tests/test_image_ktx.cpp
        backends/png/tests/test_image_png.cpp
        backends/psd/tests/test_image_psd.cpp
        backends/qoi/tests/test_image_qoi.cpp
        backends/tga/tests/test_image_tga.cpp
        backends/tiff/tests/test_image_tiff.cpp
        backends/webp/tests/test_image_webp.cpp
    )
    target_sources(image_tests PRIVATE tests/test_image_texture_source.cpp)
    target_link_libraries(image_tests PRIVATE harness_codecs prax::color prax::compress prax::compress_select
                                              prax::gpu prax::image_select prax::texture)
    if (PR_IMAGE_IMAGEIO)
        target_sources(image_tests PRIVATE backends/imageio/tests/test_image_imageio.cpp)
        target_link_libraries(image_tests PRIVATE "-framework ImageIO" "-framework CoreGraphics"
                                                  "-framework CoreFoundation")
    endif()
endif()
