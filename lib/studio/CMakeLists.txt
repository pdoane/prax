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
    kit/src/kit_panel.cpp
    kit/src/kit_text.cpp
    kit/src/kit_theme.cpp
    kit/src/kit_title_bar.cpp
)
target_link_libraries(pr_studio_kit PUBLIC prax::app prax::ui prax::layout prax::path prax::draw prax::text prax::text_edit
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
        kit/tests/test_kit_title_bar.cpp
    )
    target_link_libraries(studio_kit_tests PRIVATE kit_fixture prax::app prax::text_edit prax::text prax::draw
                                                   prax::layout prax::ui prax::ui_cpp prax::color)
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

# Studio's terminal: runs a program on a pseudoterminal, and draws its screen in cells of a monospaced font, with
# input, selection and scrollback.
pr_library(studio_terminal INCLUDE terminal/include
    terminal/src/terminal.cpp
    terminal/src/terminal_draw.cpp
    terminal/src/terminal_link.cpp
    terminal/src/terminal_shell.cpp
)
pr_embed(pr_studio_terminal terminal/shell VARIABLE kTerminalShellScripts FILES
    zsh/.zshenv
    zsh/prax.zsh
    bash/prax.bash
    fish/vendor_conf.d/prax-shell-integration.fish
)
target_link_libraries(pr_studio_terminal PUBLIC prax::app prax::color prax::layout prax::process prax::studio_kit
                                                prax::text prax::vterm
                                         PRIVATE prax::core_cpp prax::compress prax::draw prax::image prax::ui
                                                 prax::url)
# The version that the terminal sets in TERM_PROGRAM_VERSION.
target_compile_definitions(pr_studio_terminal PRIVATE PR_TERMINAL_VERSION="${PROJECT_VERSION}")

# Its tests run programs, so they build where lib/process starts them.
if (PR_DEVELOP AND (PR_PROCESS_POSIX OR PR_PROCESS_WINDOWS))
    pr_module_tests(studio_terminal
        terminal/tests/test_terminal.cpp
        terminal/tests/test_terminal_c.c
    )
    target_link_libraries(studio_terminal_tests PRIVATE kit_fixture prax::app prax::draw prax::layout prax::text prax::ui
                                                prax::process prax::url prax::vterm prax::studio_kit)
endif()

# Studio's applications and their windows: an application's kits in windows of the platform, rendered with lib/draw on
# one GPU device.
pr_library(studio_app INCLUDE app/include
    app/src/studio_app.cpp
    app/src/studio_app_shared.cpp
    app/src/studio_window.cpp
)
target_link_libraries(pr_studio_app PUBLIC prax::app prax::color prax::draw prax::font prax::gpu
                                           prax::layout prax::studio_kit prax::text prax::ui
                                    PRIVATE prax::core_cpp prax::compress prax::image)

if (PR_DEVELOP)
    pr_module_tests(studio_app
        app/tests/test_studio_app.cpp
        app/tests/test_studio_app_c.c
    )
endif()
