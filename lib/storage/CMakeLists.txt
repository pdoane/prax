# Storage: the interface, and its stores in memory and on disk.
pr_library(storage INCLUDE include
    src/storage.cpp
    src/storage_disk.cpp
    src/storage_index.cpp
    src/storage_memory.cpp
)
target_link_libraries(pr_storage PUBLIC prax::core prax::task PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(storage
        tests/test_storage.cpp
        tests/test_storage_c.c
    )
    target_link_libraries(storage_tests PRIVATE prax::task)
    target_compile_definitions(storage_tests PRIVATE PR_STORAGE_TEST_DIR="${CMAKE_CURRENT_BINARY_DIR}/storage_tests_files")
endif()
