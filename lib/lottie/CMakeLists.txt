# Lottie animations, drawn into draw lists. Their expressions run over QuickJS where the build runs JavaScript.
pr_library(lottie INCLUDE include
    src/lottie_archive.cpp
    src/lottie.cpp
    src/lottie_effect.cpp
    src/lottie_eval.cpp
    src/lottie_modifier.cpp
    src/lottie_parse.cpp
    src/lottie_render.cpp
    src/lottie_shape.cpp
    src/lottie_text.cpp
)
target_link_libraries(pr_lottie PUBLIC prax::draw prax::text prax::compress prax::font prax::image
                                PRIVATE prax::core_cpp prax::path prax::geom prax::unicode prax::json prax::color
                                        prax::locale)
if (PR_JS)
    target_sources(pr_lottie PRIVATE src/lottie_expression.cpp)
    target_link_libraries(pr_lottie PRIVATE prax::quickjs)
else()
    target_sources(pr_lottie PRIVATE src/lottie_expression_none.cpp)
endif()

if (PR_DEVELOP)
    pr_module_tests(lottie
        tests/test_lottie.cpp
        tests/test_lottie_archive.cpp
        tests/test_lottie_c.c
    )
    target_link_libraries(lottie_tests PRIVATE prax::image prax::compress prax::text prax::draw
                                               prax::font prax::path)
    target_compile_definitions(lottie_tests PRIVATE PR_FONTS_DIR="${PR_TEST_FONTS_DIR}")
    if (PR_JS)
        target_sources(lottie_tests PRIVATE tests/test_lottie_expression.cpp)
    endif()
endif()
