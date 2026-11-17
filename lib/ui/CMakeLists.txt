# Immediate-mode interaction over layout trees.
pr_library(ui INCLUDE include
    src/ui.cpp
    src/ui_text_edit.cpp
    src/ui_text_view.cpp
)
target_link_libraries(pr_ui PUBLIC prax::app prax::color prax::layout prax::text_edit prax::text_view
    PRIVATE prax::core_cpp)

# lib/ui's C++ layer: group recipes as types, for the component libraries written in C++, and elements styled by
# chained calls.
pr_library(ui_cpp INCLUDE cpp/include
    cpp/src/ui_element.cpp
)
target_link_libraries(pr_ui_cpp PUBLIC prax::ui prax::app prax::color prax::font prax::layout prax::path
                                        prax::core_cpp
    PRIVATE prax::text)

if (PR_DEVELOP)
    pr_module_tests(ui
        tests/test_ui.cpp
        tests/test_ui_c.c
        tests/test_ui_text_edit.cpp
        tests/test_ui_element.cpp
    )
    target_link_libraries(ui_tests PRIVATE prax::font prax::layout prax::text prax::text_edit prax::ui_cpp)
    target_compile_definitions(ui_tests PRIVATE PR_FONTS_DIR="${PR_TEST_FONTS_DIR}")
endif()
