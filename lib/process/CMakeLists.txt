# Processes: the interface, and the backends that the build turns on.
pr_library(process INCLUDE include
    src/process.cpp
)
target_link_libraries(pr_process PUBLIC prax::core PRIVATE prax::core_cpp)

# The backends that start the system's processes, over the pipes that a thread of each process serves.
if (PR_PROCESS_POSIX OR PR_PROCESS_WINDOWS)
    target_sources(pr_process PRIVATE backends/shared/src/process_shared_piped.cpp)
endif()

if (PR_PROCESS_POSIX)
    target_sources(pr_process PRIVATE backends/posix/src/process_posix.cpp)
    pr_headers(pr_process PUBLIC backends/posix/include)
    target_compile_definitions(pr_process PRIVATE PR_PROCESS_HAS_POSIX)
endif()

if (PR_PROCESS_WINDOWS)
    target_sources(pr_process PRIVATE backends/windows/src/process_windows.cpp)
    pr_headers(pr_process PUBLIC backends/windows/include)
    target_compile_definitions(pr_process PRIVATE PR_PROCESS_HAS_WINDOWS)
    target_link_libraries(pr_process PRIVATE prax::core_windows)
endif()

# Its tests start the system's processes.
if (PR_DEVELOP AND (PR_PROCESS_POSIX OR PR_PROCESS_WINDOWS))
    pr_module_tests(process
        tests/test_process.cpp
        tests/test_process_c.c
    )
endif()
