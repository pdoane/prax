# A small project: a library, its tests, and an option that turns on a backend.
cmake_minimum_required(VERSION 3.25)
project(ring VERSION 1.2.0 LANGUAGES C CXX)

option(RING_WITH_METAL "Build the Metal backend" OFF)
set(CMAKE_CXX_STANDARD 20)
set(RING_SOURCES src/ring.c src/ring_stats.cpp)

add_library(ring STATIC ${RING_SOURCES})
target_include_directories(ring PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_definitions(ring PRIVATE RING_VERSION="${PROJECT_VERSION}")

if (RING_WITH_METAL AND APPLE)
    target_sources(ring PRIVATE src/ring_metal.mm)
    target_link_libraries(ring PRIVATE "-framework Metal")
elseif (WIN32)
    message(STATUS "Metal is not available on Windows")
endif()

# Adds a test executable for each source named.
function(ring_tests)
    foreach (source IN LISTS ARGN)
        get_filename_component(name ${source} NAME_WE)
        add_executable(${name} ${source})
        target_link_libraries(${name} PRIVATE ring)
        add_test(NAME ${name} COMMAND ${name})
    endforeach()
endfunction()

enable_testing()
ring_tests(tests/test_ring.c tests/test_stats.cpp)
