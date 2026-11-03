# The benchmarks' harnesses that live with their runner.

# What decoding each image costs with the built-in decoders, which ./dev.sh bench image-decode runs.
pr_harness(image_bench prax::image image_bench.cpp)
target_link_libraries(image_bench PRIVATE prax::compress prax::image)

# What an immediate-mode frame costs, declared whole and laid out and painted, which ./dev.sh bench ui runs.
pr_harness(ui_bench prax::layout ui_bench.cpp)
target_link_libraries(ui_bench PRIVATE prax::text)
target_compile_definitions(ui_bench PRIVATE PR_FONTS_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts")

# What editing text costs at each size of document, which ./dev.sh bench text-edit runs.
pr_harness(text_edit_bench prax::text_edit text_edit_bench.cpp)
target_link_libraries(text_edit_bench PRIVATE prax::font prax::layout prax::text)
target_compile_definitions(text_edit_bench PRIVATE PR_FONTS_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts")

# What a windowed view costs at each size of document, which ./dev.sh bench text-view runs.
pr_harness(text_view_bench prax::text_view text_view_bench.cpp)
target_link_libraries(text_view_bench PRIVATE prax::draw prax::font prax::gpu prax::syntax prax::layout prax::text)
target_compile_definitions(text_view_bench PRIVATE PR_FONTS_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts")

# The flex benchmark, which lays the same trees out in lib/layout, Yoga and Taffy (./dev.sh bench flex). Taffy's side
# is a Rust static library, which Cargo builds, optimized whatever the build type. Where Cargo is not installed, the
# harness lays the trees out in lib/layout and Yoga only, with no reference to check their boxes against.
pr_dependency_available(yoga)
file(GLOB_RECURSE PR_YOGA_SOURCES CONFIGURE_DEPENDS ${yoga_SOURCE_DIR}/yoga/*.cpp)
add_library(yoga STATIC ${PR_YOGA_SOURCES})
target_include_directories(yoga SYSTEM PUBLIC ${yoga_SOURCE_DIR})
pr_third_party(yoga)

add_executable(flex_bench
    flex_bench/flex_bench.cpp
    flex_bench/prax_engine.cpp
    flex_bench/yoga_engine.cpp
)
target_include_directories(flex_bench PRIVATE ${PROJECT_SOURCE_DIR})
target_link_libraries(flex_bench PRIVATE prax::layout harness yoga pr_warnings)

find_program(PR_CARGO cargo)
if (PR_CARGO)
    pr_dependency_available(taffy)
    set(FLEX_BENCH_TAFFY ${CMAKE_CURRENT_SOURCE_DIR}/flex_bench/taffy)
    set(FLEX_BENCH_TAFFY_MANIFEST ${CMAKE_CURRENT_BINARY_DIR}/flex_bench_taffy)
    # Cargo names a static library as the platform does: libflex_bench_taffy.a, or flex_bench_taffy.lib with MSVC.
    set(FLEX_BENCH_TAFFY_FILE ${CMAKE_STATIC_LIBRARY_PREFIX}flex_bench_taffy${CMAKE_STATIC_LIBRARY_SUFFIX})
    set(FLEX_BENCH_TAFFY_LIB ${CMAKE_CURRENT_BINARY_DIR}/cargo/release/${FLEX_BENCH_TAFFY_FILE})
    configure_file(${FLEX_BENCH_TAFFY}/Cargo.toml.in ${FLEX_BENCH_TAFFY_MANIFEST}/Cargo.toml @ONLY)
    configure_file(${FLEX_BENCH_TAFFY}/Cargo.lock ${FLEX_BENCH_TAFFY_MANIFEST}/Cargo.lock COPYONLY)
    file(GLOB_RECURSE PR_TAFFY_SOURCES CONFIGURE_DEPENDS ${taffy_SOURCE_DIR}/src/*.rs)
    add_custom_command(OUTPUT ${FLEX_BENCH_TAFFY_LIB}
        COMMAND ${PR_CARGO} build --release --locked --quiet --manifest-path ${FLEX_BENCH_TAFFY_MANIFEST}/Cargo.toml
            --target-dir ${CMAKE_CURRENT_BINARY_DIR}/cargo
        DEPENDS ${FLEX_BENCH_TAFFY_MANIFEST}/Cargo.toml ${FLEX_BENCH_TAFFY}/Cargo.lock ${FLEX_BENCH_TAFFY}/src/lib.rs
            ${PR_TAFFY_SOURCES}
        COMMENT "Building the flex benchmark's Taffy library"
        VERBATIM)
    add_custom_target(flex_bench_taffy_build DEPENDS ${FLEX_BENCH_TAFFY_LIB})
    add_library(flex_bench_taffy STATIC IMPORTED)
    set_target_properties(flex_bench_taffy PROPERTIES IMPORTED_LOCATION ${FLEX_BENCH_TAFFY_LIB})
    # Rust's standard library calls the system libraries that a Rust executable links on Windows.
    if (WIN32)
        set_target_properties(flex_bench_taffy PROPERTIES INTERFACE_LINK_LIBRARIES "ws2_32;userenv;ntdll;bcrypt")
    endif()
    add_dependencies(flex_bench_taffy flex_bench_taffy_build)
    target_sources(flex_bench PRIVATE flex_bench/taffy_engine.cpp)
    target_link_libraries(flex_bench PRIVATE flex_bench_taffy)
endif()
target_compile_definitions(flex_bench PRIVATE PR_FLEX_BENCH_TAFFY=$<BOOL:${PR_CARGO}>)

# A check that Yoga can set Prax text. It links lib/text and no layout, so it fails if the library comes to need one.
add_executable(text_yoga_check flex_bench/text_yoga_check.cpp)
target_link_libraries(text_yoga_check PRIVATE prax::font prax::text yoga pr_warnings)
target_compile_definitions(text_yoga_check PRIVATE PR_FONTS_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts")
add_test(NAME text_yoga_check COMMAND text_yoga_check)

if (PR_JS)
    # What the editor's frames cost, by stage, on the platform's device, over a fixture of the benchmark's own.
    # ./dev.sh bench ui runs it.
    add_executable(editor_bench
        editor_bench/main.cpp
        editor_bench/host.cpp
        editor_bench/editor_state.cpp
        editor_bench/editor_view.cpp
        editor_bench/hud.cpp
    )
    target_include_directories(editor_bench PRIVATE ${PROJECT_SOURCE_DIR})
    target_link_libraries(editor_bench PRIVATE prax::studio_kit prax::app harness prax::text prax::font_bundle
                                               prax::draw prax::gpu prax::layout prax::ui
                                               prax::ui_cpp prax::font prax::color prax::core_cpp
                                               pr_warnings)
    target_compile_definitions(editor_bench PRIVATE PR_FONT_BUNDLE_DIR="${PR_FONT_BUNDLE_DIR}")

    # What a long document costs to load, scroll, hover, navigate and resize. ./dev.sh bench long-doc runs it.
    add_executable(doc_bench doc_bench.cpp)
    target_link_libraries(doc_bench PRIVATE prax::html prax::text prax::draw prax::gpu harness
                                            prax::font_bundle prax::font prax::image
                                            prax::compress prax::css prax::layout
                                            prax::fetch prax::net prax::task pr_warnings)
    target_compile_definitions(doc_bench PRIVATE PR_FONT_BUNDLE_DIR="${PR_FONT_BUNDLE_DIR}")
endif()

if ((APPLE OR WIN32) AND PR_JS)
    # What each drawing costs to record and render with each backend. ./dev.sh bench draw runs it.
    add_executable(draw_bench draw_bench.cpp)
    target_link_libraries(draw_bench PRIVATE viewer_documents harness prax::compress prax::draw prax::gpu
                                             prax::image pr_warnings)
endif()
