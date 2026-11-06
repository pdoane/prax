# Scribe: Studio's text editor in a window, whose operations are commands of lib/studio/command.
# Its interface is a library of its own, which the application and the tests of its interface share.
add_library(scribe_editor STATIC scribe.cpp)
target_include_directories(scribe_editor PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(scribe_editor PUBLIC prax::app prax::process prax::syntax prax::studio_command prax::studio_kit
                                           prax::studio_terminal prax::studio_text_editor prax::layout prax::ui
                                           prax::core_cpp
                                    PRIVATE pr_warnings prax::color)
if (PR_SYNTAX_TREESITTER)
    target_compile_definitions(scribe_editor PUBLIC SCRIBE_HAS_TREESITTER)
endif()

pr_app(scribe main.cpp)
target_link_libraries(scribe PRIVATE scribe_editor prax::studio_window prax::font_bundle prax::text)

if (PR_DEVELOP)
    pr_module_tests(scribe_editor tests/test_scribe.cpp)
    target_link_libraries(scribe_editor_tests PRIVATE kit_fixture prax::font prax::layout prax::text prax::ui prax::url)
endif()
