# The examples: applications over the public headers, each a window on the platform's backends, composed in its own
# main(). They link the libraries as a consumer does, by their prax:: names. An example includes its own headers from
# the repository's root, as examples/<name>/<header>. An example's target is named for its directory, which
# `./dev.sh example <name>` builds and runs.

# Adds the example `name`, built from `sources`, which reads the font bundles' files from the build directory. On the
# web, the example is a page, <name>.html, and reads the files that pr_example_fonts packages under /font_bundle.
function(pr_example name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR})
    target_link_libraries(${name} PRIVATE pr_warnings)
    if (EMSCRIPTEN)
        target_compile_definitions(${name} PRIVATE PR_FONT_BUNDLE_DIR="/font_bundle")
        set_target_properties(${name} PROPERTIES SUFFIX ".html")
        target_link_options(${name} PRIVATE "--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/shell.html")
        set_property(TARGET ${name} APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shell.html)
    else()
        target_compile_definitions(${name} PRIVATE PR_FONT_BUNDLE_DIR="${PR_FONT_BUNDLE_DIR}")
    endif()
endfunction()

# Packages the font bundles' files that the example `name` reads into its page, under /font_bundle. A page downloads
# everything that it packages, so only the files that the example reads are packaged.
function(pr_example_fonts name)
    foreach (file IN LISTS ARGN)
        target_link_options(${name} PRIVATE "SHELL:--preload-file \"${PR_FONT_BUNDLE_DIR}/${file}@/font_bundle/${file}\"")
    endforeach()
endfunction()

# The web's build has only the draw gallery and video examples. They run in a browser, so they read files packaged into their
# pages, not the host's files as the tests under Node.js do.
if (EMSCRIPTEN)
    get_directory_property(PR_EXAMPLE_LINK_OPTIONS LINK_OPTIONS)
    list(REMOVE_ITEM PR_EXAMPLE_LINK_OPTIONS -sNODERAWFS=1)
    set_directory_properties(PROPERTIES LINK_OPTIONS "${PR_EXAMPLE_LINK_OPTIONS}")
    pr_example(draw_gallery draw_gallery/main.cpp draw_gallery/scenes.cpp)
    target_link_libraries(draw_gallery PRIVATE prax::app prax::draw prax::gpu
                                               prax::font prax::locale prax::text prax::core_cpp)
    pr_example_fonts(draw_gallery "NotoSans[wdth,wght].ttf" "NotoSerif[wdth,wght].ttf" "NotoSansJP-VF.otf")
    pr_example(video video/main.cpp)
    target_link_libraries(video PRIVATE prax::app prax::draw prax::gpu prax::media prax::core_cpp)
    target_compile_definitions(video PRIVATE PR_VIDEO_FILE="/videos/cat.mp4")
    target_link_options(video PRIVATE
        "SHELL:--preload-file \"${PROJECT_SOURCE_DIR}/third_party/videos/cat.mp4@/videos/cat.mp4\"")
    return()
endif()

# The draw gallery: animated scenes drawn with lib/draw alone, rendered on the GPU and mapped to the display.
pr_example(draw_gallery draw_gallery/main.cpp draw_gallery/scenes.cpp)
target_link_libraries(draw_gallery PRIVATE prax::app prax::draw prax::gpu
                                   prax::font prax::locale prax::text prax::core_cpp)

# The video example: a video that lib/media decodes and lib/draw draws. The device imports each frame without a copy
# where it can. ./dev.sh fetch videos fetches the example's video into third_party/videos.
pr_example(video video/main.cpp)
target_link_libraries(video PRIVATE prax::app prax::draw prax::gpu prax::media prax::core_cpp)
target_compile_definitions(video PRIVATE PR_VIDEO_FILE="${PROJECT_SOURCE_DIR}/third_party/videos/cat.mp4")
# The feature drawings: a captioned drawing of each feature of lib/draw's list, and dense workloads that measure its
# renderers. They are a library of their own, which the application and draw_bench link. Their fonts are the files of
# third_party/fonts.
add_library(draw_features_drawings STATIC
    draw_features/drawing.cpp
    draw_features/features.cpp
    draw_features/features_boxes.cpp
    draw_features/features_compose.cpp
    draw_features/features_filters.cpp
    draw_features/features_fonts.cpp
    draw_features/features_paint.cpp
    draw_features/features_shapes.cpp
    draw_features/features_text.cpp
    draw_features/workloads.cpp
)
target_include_directories(draw_features_drawings PUBLIC ${PROJECT_SOURCE_DIR})
target_compile_definitions(draw_features_drawings PRIVATE PR_FONTS_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts")
target_link_libraries(draw_features_drawings PUBLIC prax::app prax::draw prax::font prax::text prax::core_cpp
                                             PRIVATE prax::color prax::compress prax::image prax::path prax::svg
                                                     pr_warnings)

# The feature drawings in a window, rendered on the GPU and mapped to the display.
pr_example(draw_features draw_features/main.cpp)
target_link_libraries(draw_features PRIVATE draw_features_drawings prax::app prax::draw prax::gpu prax::text
                                            prax::core_cpp)

# The editor example: Studio Kit's widgets and a HUD library of its own over an editor's model, composed in one tree.
pr_example(editor
    editor/main.cpp
    editor/editor_state.cpp
    editor/editor_view.cpp
    editor/hud.cpp
)
target_link_libraries(editor PRIVATE prax::app prax::studio_kit prax::draw
                                     prax::gpu prax::layout prax::ui prax::ui_cpp prax::text
                                     prax::font prax::font_bundle prax::color prax::core_cpp)

# The Studio Kit gallery: every widget of the kit, live, over a model of its own.
pr_example(studio_gallery
    studio_gallery/main.cpp
    studio_gallery/gallery_state.cpp
    studio_gallery/gallery_parts.cpp
    studio_gallery/gallery_view.cpp
    studio_gallery/section_foundations.cpp
    studio_gallery/section_controls.cpp
    studio_gallery/section_components.cpp
    studio_gallery/section_feedback.cpp
    studio_gallery/section_data.cpp
)
target_link_libraries(studio_gallery PRIVATE prax::app prax::text_edit prax::studio_kit prax::draw prax::gpu prax::font prax::layout
                                             prax::ui prax::text prax::font_bundle prax::color prax::core_cpp)

# The perspective example: Kit panels standing in 3D over a background that the application draws with its own
# pipeline, their backdrops reading its pixels through the projection.
pr_example(perspective perspective/perspective.cpp perspective/perspective_gpu.cpp)
pr_slang(perspective perspective/perspective_gpu.slang VARIABLE kPerspectiveShaders)
target_link_libraries(perspective PRIVATE prax::app prax::studio_kit prax::draw
                                          prax::gpu prax::layout prax::text prax::font_bundle
                                          prax::image prax::compress prax::core_cpp)
target_compile_definitions(perspective PRIVATE
    PR_PERSPECTIVE_PICTURE="${CMAKE_CURRENT_SOURCE_DIR}/perspective/picture.png")

# The incremental example: Markdown and HTML rendered as they arrive in pieces, beside Kit's controls for the stream.
pr_example(incremental incremental/incremental.cpp)
target_link_libraries(incremental PRIVATE prax::app prax::studio_kit prax::draw
                                          prax::gpu prax::layout prax::text prax::font_bundle prax::html
                                          prax::dom prax::css prax::markdown prax::core_cpp)
