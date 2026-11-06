# Studio Kit: the widgets of the Studio design system, over lib/ui and lib/layout.
pr_library(studio_kit INCLUDE kit/include
    kit/src/kit.cpp
    kit/src/kit_box.cpp
    kit/src/kit_button.cpp
    kit/src/kit_choice.cpp
    kit/src/kit_collection.cpp
    kit/src/kit_color.cpp
    kit/src/kit_data.cpp
    kit/src/kit_image.cpp
    kit/src/kit_timeline.cpp
    kit/src/kit_dock.cpp
    kit/src/kit_graph.cpp
    kit/src/kit_icon.cpp
    kit/src/kit_input.cpp
    kit/src/kit_menu.cpp
    kit/src/kit_surface.cpp
    kit/src/kit_text.cpp
    kit/src/kit_theme.cpp
)
target_link_libraries(pr_studio_kit PUBLIC prax::ui prax::layout prax::path prax::draw prax::text prax::text_edit
                                           prax::geom prax::font prax::color
                                    PRIVATE prax::core_cpp prax::ui_cpp)

if (PR_DEVELOP)
    pr_module_tests(studio_kit
        kit/tests/test_kit.cpp
        kit/tests/test_kit_c.c
        kit/tests/test_kit_choice.cpp
        kit/tests/test_kit_collection.cpp
        kit/tests/test_kit_color.cpp
        kit/tests/test_kit_data.cpp
        kit/tests/test_kit_image.cpp
        kit/tests/test_kit_timeline.cpp
        kit/tests/test_kit_dock.cpp
        kit/tests/test_kit_graph.cpp
        kit/tests/test_kit_input.cpp
        kit/tests/test_kit_text.cpp
        kit/tests/test_kit_menu.cpp
    )
    target_link_libraries(studio_kit_tests PRIVATE kit_fixture prax::text_edit prax::text prax::draw prax::layout prax::ui
                                                   prax::ui_cpp prax::color)
endif()

# Studio's commands: an application's verbs, their key bindings, and the scopes, menu items and command palette that
# run them.
pr_library(studio_command INCLUDE command/include
    command/src/command.cpp
    command/src/command_keymap.cpp
    command/src/command_kit.cpp
)
target_link_libraries(pr_studio_command PUBLIC prax::json prax::app prax::ui prax::studio_kit
                                        PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(studio_command
        command/tests/test_command.cpp
        command/tests/test_command_c.c
        command/tests/test_command_kit.cpp
    )
    target_link_libraries(studio_command_tests PRIVATE kit_fixture prax::json prax::studio_kit)
endif()

# Studio's text editor: documents of files in windowed views, colored by their syntax, with the commands of a code
# editor and the language servers of their languages.
pr_library(studio_text_editor INCLUDE text_editor/include
    text_editor/src/text_editor.cpp
    text_editor/src/text_editor_debug.cpp
    text_editor/src/text_editor_diff.cpp
    text_editor/src/text_editor_server.cpp
    text_editor/src/text_editor_style.cpp
)
target_link_libraries(pr_studio_text_editor PUBLIC prax::app prax::layout prax::process prax::studio_command
                                                   prax::studio_kit prax::syntax prax::text prax::text_diff
                                                   prax::text_edit prax::text_view
                                            PRIVATE prax::core_cpp prax::color prax::dap prax::lsp prax::ui)

if (PR_DEVELOP)
    pr_module_tests(studio_text_editor
        text_editor/tests/test_text_editor.cpp
        text_editor/tests/test_text_editor_c.c
    )
    target_link_libraries(studio_text_editor_tests PRIVATE kit_fixture prax::layout prax::text prax::ui
                                                           prax::studio_command prax::studio_kit prax::syntax prax::process)
    target_compile_definitions(studio_text_editor_tests PRIVATE
        TEXT_EDITOR_TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/text_editor/tests"
        TEXT_EDITOR_TESTS_PYTHON="${Python3_EXECUTABLE}")
    if (PR_SYNTAX_TREESITTER)
        target_compile_definitions(studio_text_editor_tests PRIVATE TEXT_EDITOR_TESTS_HAVE_TREESITTER)
    endif()
endif()

# Studio's window: an application's kit in a window of the platform, rendered with lib/draw on the platform's GPU.
pr_library(studio_window INCLUDE window/include
    window/src/studio_window.cpp
)
target_link_libraries(pr_studio_window PUBLIC prax::app prax::color prax::draw prax::font prax::gpu
                                              prax::layout prax::studio_kit prax::text prax::ui
                                       PRIVATE prax::core_cpp prax::compress prax::image)

if (PR_DEVELOP)
    pr_module_tests(studio_window
        window/tests/test_studio_window.cpp
        window/tests/test_studio_window_c.c
    )
endif()
