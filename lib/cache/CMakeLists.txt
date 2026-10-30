# Caches: values asked for by key, which load on demand.
pr_library(cache INCLUDE include
    src/cache.cpp
)
target_link_libraries(pr_cache PUBLIC prax::core PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(cache
        tests/test_cache.cpp
        tests/test_cache_c.c
    )
    target_link_libraries(cache_tests PRIVATE prax::task)
endif()
