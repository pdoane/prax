# The checks of the captured pages.

if (PR_JS)
    # Changes over a captured page, each checked against a full recomputation (tools/pages/invalidation.py).
    add_executable(invalidation invalidation.cpp)
    target_link_libraries(invalidation PRIVATE page_host prax::text prax::html harness prax::dom prax::css prax::layout
                                               pr_warnings)
endif()
