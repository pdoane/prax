# Immediate-mode interaction over layout trees.
pr_library(ui INCLUDE include
    src/ui.cpp
    src/ui_text_edit.cpp
    src/ui_text_view.cpp
)
target_link_libraries(pr_ui PUBLIC prax::app prax::color prax::layout prax::text_edit prax::text_view
    PRIVATE prax::core_cpp)

# lib/ui's C++ layer: group recipes as types, for the component libraries written in C++.
pr_library(ui_cpp INCLUDE cpp/include)
target_link_libraries(pr_ui_cpp INTERFACE prax::ui prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(ui
        tests/test_ui.cpp
        tests/test_ui_c.c
        tests/test_ui_text_edit.cpp
    )
    target_link_libraries(ui_tests PRIVATE prax::font prax::layout prax::text prax::text_edit)
    target_compile_definitions(ui_tests PRIVATE PR_FONTS_DIR="${PR_TEST_FONTS_DIR}")
endif()
