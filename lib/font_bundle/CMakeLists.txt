# Font bundles: generic families and fallback bound to fonts that pr_font_bundle_fetch fetches (font_bundle.cmake).
pr_library(font_bundle INCLUDE include
    src/font_bundle.cpp
)
target_link_libraries(pr_font_bundle PUBLIC prax::font PRIVATE prax::core_cpp prax::locale)

if (PR_DEVELOP)
    pr_module_tests(font_bundle
        tests/test_font_bundle.cpp
    )
    target_link_libraries(font_bundle_tests PRIVATE prax::locale prax::text)
    # The tests install the bundles' files that the test data holds, and compare each bundle's files with its list.
    target_compile_definitions(font_bundle_tests PRIVATE PR_FONT_BUNDLE_DIR="${PR_TEST_FONTS_DIR}")
    foreach (bundle Complete NotoMono Standard14 Plex NerdFontSymbols)
        list(LENGTH PR_FONT_BUNDLE_${bundle} files)
        target_compile_definitions(font_bundle_tests PRIVATE PR_FONT_BUNDLE_FILES_${bundle}=${files})
    endforeach()
endif()
