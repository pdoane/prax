# A program that consumes Prax as an application does, which ./dev.sh package builds and runs: from the installed
# package, or from the source tree through FetchContent when PRAX_SOURCE_DIR names it.
cmake_minimum_required(VERSION 3.28)
project(prax_consumer C CXX)
# What README.md says a consumer sets, so that the linker does not warn about a static library that the link line
# repeats.
set(CMAKE_LINK_LIBRARIES_STRATEGY REORDER_FREELY)

set(PRAX_SOURCE_DIR "" CACHE PATH "Prax's source tree, which FetchContent adds in place of the installed package")
if (PRAX_SOURCE_DIR)
    include(FetchContent)
    FetchContent_Declare(prax SOURCE_DIR ${PRAX_SOURCE_DIR})
    FetchContent_MakeAvailable(prax)
else()
    find_package(prax 0.1 REQUIRED COMPONENTS draw text font html_quickjs)
endif()

add_executable(consumer consumer.cpp)
target_link_libraries(consumer PRIVATE prax::draw prax::text prax::font prax::html_quickjs)
