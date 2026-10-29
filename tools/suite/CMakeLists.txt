# The conformance suites' harnesses that live with their runners, and what the suites render with.

# How a suite renders, as its harness and the viewer read it (suites/<suite>/render.txt).
add_library(suite_render STATIC suite_render.cpp)
target_include_directories(suite_render PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(suite_render PUBLIC prax::font PRIVATE prax::core_cpp prax::color pr_warnings)

# The reftest harness, which the runners of every suite that compares renders drive: tools/suite/wpt/wpt.py and
# tools/suite/reftest.py.
pr_harness(reftest prax::svg reftest.cpp)
target_compile_definitions(reftest PRIVATE PR_ROOT_DIR="${PROJECT_SOURCE_DIR}")
target_link_libraries(reftest PRIVATE harness_png prax::dom prax::draw draw_compare
                                      suite_render prax::image prax::compress prax::lottie
                                      prax::font prax::text prax::url)

add_subdirectory(markdown)
add_subdirectory(lottie)
