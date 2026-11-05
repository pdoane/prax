# The development build's tools, a directory for each concept that dev.sh names: what the harnesses share, the
# conformance suites' harnesses, the shell, the viewer, the benchmarks, the browsers that the engine is compared with,
# and the captured pages' checks.
add_subdirectory(harness)
add_subdirectory(shell)
# The rest run natively, as dev.sh runs them. The web's build has only what the libraries' tests run: the harnesses that
# the tests share, and the shell.
if (EMSCRIPTEN)
    return()
endif()
add_subdirectory(suite)
add_subdirectory(viewer)
add_subdirectory(bench)
add_subdirectory(browsers)
add_subdirectory(pages)
