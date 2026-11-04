# What the harnesses, the tools and the tests of the libraries share.

# The main() of every test binary, which installs the crash handler before doctest runs the tests. It takes core's
# declarations only, so that each binary links the core that it tests.
add_library(test_main STATIC test_main.cpp test.h)
# Every test includes doctest through test.h, by its path under the root.
target_include_directories(test_main PUBLIC ${PROJECT_SOURCE_DIR} PRIVATE ${PROJECT_SOURCE_DIR}/lib/core/include)
target_link_libraries(test_main PUBLIC doctest::doctest PRIVATE pr_warnings)

# Reading files and framed cases, writing JSON, and the CPU's counters.
add_library(harness STATIC cpu_counters.cpp harness.cpp)
target_include_directories(harness PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(harness PUBLIC prax::core prax::core_cpp PRIVATE pr_warnings)

# PNG files that the harnesses write.
add_library(harness_png STATIC harness_png.cpp)
target_include_directories(harness_png PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(harness_png PUBLIC prax::core PRIVATE prax::image prax::compress pr_warnings)

# The standard streams of a harness, and the capture of what a served run writes to them.
add_library(harness_streams STATIC harness_streams.cpp)
target_include_directories(harness_streams PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(harness_streams PUBLIC prax::core PRIVATE pr_warnings)

# The peer of the servers that tools/harness/net_peer.py runs for the tests of lib/net, lib/tls, lib/http, lib/fetch and
# lib/websocket.
add_library(net_peer STATIC net_peer.cpp)
target_include_directories(net_peer PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(net_peer PUBLIC prax::net prax::core_cpp PRIVATE pr_warnings)

# Image comparison as the SVG corpora and WPT compare, shared by the reftest harness, pr_shell, the viewer and lib/draw's
# tests. It blurs whole frames on the CPU, optimized in every build so that a Debug viewer stays interactive, except
# under MSVC, which refuses to optimize a Debug build's runtime checks.
add_library(draw_compare STATIC compare.cpp)
if (NOT (CMAKE_BUILD_TYPE STREQUAL "Debug" AND MSVC))
    target_compile_options(draw_compare PRIVATE -O2)
endif()
target_include_directories(draw_compare PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(draw_compare PUBLIC prax::core PRIVATE prax::core_cpp pr_warnings)

# The fixture of the tests of Studio Kit and of the libraries over it (kit_fixture.h): a kit that sets text in the Noto
# fonts that the build fetched, and the frames that drive it.
add_library(kit_fixture INTERFACE kit_fixture.h)
target_include_directories(kit_fixture INTERFACE ${PROJECT_SOURCE_DIR})
target_compile_definitions(kit_fixture INTERFACE PR_FONT_BUNDLE_DIR="${PR_FONT_BUNDLE_DIR}")
target_link_libraries(kit_fixture INTERFACE prax::studio_kit prax::font_bundle prax::font prax::text prax::draw
                                            prax::layout prax::ui doctest::doctest)
