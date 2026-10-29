# Web IDL's error names, and its QuickJS C++ layer, which binds interfaces as Web IDL specifies.
pr_library(webidl INCLUDE include
    src/webidl.cpp
    src/webidl_error.cpp
)
target_link_libraries(pr_webidl PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_JS)
    pr_library(webidl_quickjs_cpp INCLUDE quickjs_cpp/include
        quickjs_cpp/src/webidl_observable_array_quickjs.cpp
        quickjs_cpp/src/webidl_quickjs.cpp
    )
    target_link_libraries(pr_webidl_quickjs_cpp PUBLIC prax::core_quickjs_cpp PRIVATE prax::webidl)
endif()

if (PR_DEVELOP)
    pr_module_tests(webidl
        tests/test_webidl.cpp
    )
    if (PR_JS)
        pr_module_tests(webidl_quickjs_cpp
            quickjs_cpp/tests/test_webidl_observable_array_quickjs.cpp
        )
        target_link_libraries(webidl_quickjs_cpp_tests PRIVATE prax::core_quickjs_cpp)
    endif()
endif()
