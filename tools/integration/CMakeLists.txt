# The integration tests: applications that check how the libraries serve a host, each of which prints its outcome and
# ends. ./dev.sh integration builds and runs them (tools/integration/commands.py).

# The Swift host: an application of SwiftUI's own, without lib/app, whose immersive space lib/surface and lib/gpu draw.
if (PR_VISIONOS)
    add_executable(swift_host swift_host/swift_host.swift swift_host/swift_host.mm)
    target_include_directories(swift_host PRIVATE ${PROJECT_SOURCE_DIR})
    set_source_files_properties(swift_host/swift_host.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    target_compile_options(swift_host PRIVATE
        $<$<COMPILE_LANGUAGE:Swift>:-parse-as-library -import-objc-header ${CMAKE_CURRENT_SOURCE_DIR}/swift_host/swift_host.h>)
    target_link_libraries(swift_host PRIVATE pr_warnings prax::gpu prax::surface prax::core_cpp
                                             "-framework SwiftUI" "-framework CompositorServices")
    pr_uikit_bundle(swift_host MULTIPLE_SCENES)
endif()

# The app host: lib/app opens its immersive space, lib/surface and lib/gpu draw it, and lib/app closes it.
if (PR_VISIONOS)
    add_executable(app_host app_host/app_host.cpp)
    target_include_directories(app_host PRIVATE ${PROJECT_SOURCE_DIR})
    target_link_libraries(app_host PRIVATE pr_warnings prax::app prax::gpu prax::surface prax::core_cpp)
    pr_uikit_bundle(app_host MULTIPLE_SCENES)
endif()
