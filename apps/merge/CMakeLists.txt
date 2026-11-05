# Prax Merge: comparisons and three-way merges of files, as Git's difftool and mergetool and Perforce's P4DIFF and
# P4MERGE run them, in Studio's window. Its command line and the merge of its files are a library of their own, which
# the application and the tests link.
add_library(merge_model STATIC merge.cpp)
target_include_directories(merge_model PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(merge_model PUBLIC prax::core_cpp PRIVATE prax::text_diff pr_warnings)

pr_app(merge main.cpp)
target_link_libraries(merge PRIVATE merge_model prax::layout prax::studio_command prax::studio_kit
                                    prax::studio_text_editor prax::studio_window prax::syntax prax::text prax::ui)
if (PR_SYNTAX_TREESITTER)
    target_compile_definitions(merge PRIVATE MERGE_HAS_TREESITTER)
endif()

if (PR_DEVELOP)
    pr_module_tests(merge_model tests/test_merge.cpp)
endif()
