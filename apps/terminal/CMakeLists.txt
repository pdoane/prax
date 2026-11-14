# Terminal: the terminals of lib/studio/terminal in tabs, in Studio's window. The app's interface is a separate library,
# which the application and its tests share.
add_library(terminal_app STATIC terminal_app.cpp)
target_include_directories(terminal_app PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(terminal_app PUBLIC prax::app prax::color prax::font prax::process prax::studio_command
                                          prax::studio_kit prax::studio_terminal prax::ui prax::core_cpp
                                   PRIVATE prax::layout pr_warnings)

pr_app(terminal main.cpp)
target_link_libraries(terminal PRIVATE terminal_app prax::studio_app prax::font_bundle prax::vterm)

if (PR_DEVELOP)
    pr_module_tests(terminal_app tests/test_terminal_app.cpp)
    target_link_libraries(terminal_app_tests PRIVATE kit_fixture prax::color prax::layout prax::ui prax::url)
endif()
