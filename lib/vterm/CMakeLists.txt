# Virtual terminals: the screen of a terminal emulator, the parser for the escape sequences that change it, and the
# input that the terminal sends to its program.
pr_library(vterm INCLUDE include
    src/vterm.cpp
    src/vterm_graphics.cpp
    src/vterm_input.cpp
    src/vterm_link.cpp
    src/vterm_notification.cpp
    src/vterm_parser.cpp
    src/vterm_report.cpp
    src/vterm_screen.cpp
)
target_link_libraries(pr_vterm PUBLIC prax::core prax::app prax::color prax::compress prax::image
                               PRIVATE prax::core_cpp prax::unicode)

if (PR_DEVELOP)
    pr_module_tests(vterm
        tests/test_vterm.cpp
        tests/test_vterm_c.c
        tests/test_vterm_captures.cpp
        tests/test_vterm_graphics.cpp
        tests/test_vterm_input.cpp
        tests/test_vterm_link.cpp
        tests/test_vterm_margins.cpp
        tests/test_vterm_notification.cpp
        tests/test_vterm_marks.cpp
        tests/test_vterm_parser.cpp
        tests/test_vterm_report.cpp
        tests/test_vterm_scrollback.cpp
    )
    target_link_libraries(vterm_tests PRIVATE prax::compress prax::image prax::unicode)
    target_compile_definitions(vterm_tests PRIVATE PR_VTERM_CAPTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/captures")

    # The esctest harness: a terminal with no window that runs esctest on its pseudoterminal. tools/suite/esctest.py
    # drives it.
    pr_harness(vterm_esctest prax::vterm tests/vterm_esctest.cpp)
    target_link_libraries(vterm_esctest PRIVATE prax::process)
endif()
