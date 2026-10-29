# Named character references, which HTML and Markdown expand alike.
pr_library(html_entity INCLUDE include
    src/html_entity.cpp
)
target_link_libraries(pr_html_entity PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(html_entity
        tests/test_html_entity.cpp
        tests/test_html_entity_c.c
    )
endif()
