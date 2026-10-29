# Locales: languages, scripts and regions, and the language tags that name them.
pr_library(locale INCLUDE include
    src/locale.cpp
    src/locale_data.cpp
)
target_link_libraries(pr_locale PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(locale
        tests/test_locale.cpp
        tests/test_locale_c.c
    )
endif()
