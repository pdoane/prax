# Task pools: the interface, and the backend that the build turns on.
pr_library(task INCLUDE include
    src/task.cpp
)
target_link_libraries(pr_task PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_TASK_BUILTIN)
    target_sources(pr_task PRIVATE backends/builtin/src/task_builtin.cpp)
    pr_headers(pr_task PUBLIC backends/builtin/include)
    target_compile_definitions(pr_task PRIVATE PR_TASK_HAS_BUILTIN)
endif()

if (PR_DEVELOP)
    pr_module_tests(task
        tests/test_task.cpp
        tests/test_task_c.c
    )
    if (PR_TASK_BUILTIN)
        target_sources(task_tests PRIVATE backends/builtin/tests/test_task_builtin.cpp)
    endif()
endif()
