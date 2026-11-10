# The examples: applications over the public headers, each a window on the platform's backends, composed in its own
# main(). They link the libraries as a consumer does, by their prax:: names. An example includes its own headers from
# the repository's root, as examples/<name>/<header>. An example's target is named for its directory, which
# `./dev.sh example <name>` builds and runs.

# A page reads only the files that it packages, not the host's files as the tests under Node.js do.
if (EMSCRIPTEN)
    get_directory_property(PR_EXAMPLE_LINK_OPTIONS LINK_OPTIONS)
    list(REMOVE_ITEM PR_EXAMPLE_LINK_OPTIONS -sNODERAWFS=1)
    set_directory_properties(PROPERTIES LINK_OPTIONS "${PR_EXAMPLE_LINK_OPTIONS}")
endif()

# Adds the example `name` for every platform:
#
#   pr_example(<name> <source>... [FONT_BUNDLES <bundle>...] [FONT_FILES <file>...] [DATA <macro> <path>...]
#       [INFO_PLIST <file>])
#
# The example reads the font bundles' files from PR_FONT_BUNDLE_DIR, and each DATA path, a file or a directory, from
# its macro. On the web, the example is a page, <name>.html, which packages the files of FONT_BUNDLES, FONT_FILES and
# DATA. On macOS, it is an application bundle, org.prax.<name>, which INFO_PLIST describes if it is given.
function(pr_example name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "INFO_PLIST" "FONT_BUNDLES;FONT_FILES;DATA")
    add_executable(${name} ${arg_UNPARSED_ARGUMENTS})
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR})
    target_link_libraries(${name} PRIVATE pr_warnings)
    if (EMSCRIPTEN)
        target_compile_definitions(${name} PRIVATE PR_FONT_BUNDLE_DIR="/font_bundle")
        set_target_properties(${name} PROPERTIES SUFFIX ".html")
        target_link_options(${name} PRIVATE "--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/shell.html")
        set_property(TARGET ${name} APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shell.html)
        set(files ${arg_FONT_FILES})
        foreach (bundle IN LISTS arg_FONT_BUNDLES)
            foreach (file IN LISTS PR_FONT_BUNDLE_${bundle})
                get_filename_component(file "${file}" NAME)
                list(APPEND files "${file}")
            endforeach()
        endforeach()
        list(REMOVE_DUPLICATES files)
        foreach (file IN LISTS files)
            target_link_options(${name} PRIVATE
                "SHELL:--preload-file \"${PR_FONT_BUNDLE_DIR}/${file}@/font_bundle/${file}\"")
        endforeach()
    else()
        target_compile_definitions(${name} PRIVATE PR_FONT_BUNDLE_DIR="${PR_FONT_BUNDLE_DIR}")
    endif()
    while (arg_DATA)
        list(POP_FRONT arg_DATA macro path)
        get_filename_component(packaged "${path}" NAME)
        if (EMSCRIPTEN)
            target_compile_definitions(${name} PRIVATE ${macro}="/data/${packaged}")
            target_link_options(${name} PRIVATE "SHELL:--preload-file \"${path}@/data/${packaged}\"")
        else()
            target_compile_definitions(${name} PRIVATE ${macro}="${path}")
        endif()
    endwhile()
    if (APPLE AND NOT IOS)
        set_target_properties(${name} PROPERTIES MACOSX_BUNDLE TRUE MACOSX_BUNDLE_GUI_IDENTIFIER org.prax.${name})
        if (arg_INFO_PLIST)
            set_target_properties(${name} PROPERTIES MACOSX_BUNDLE_INFO_PLIST ${arg_INFO_PLIST})
        endif()
        # The linker signs only the executable. A signature of the whole bundle binds its Info.plist and so its
        # identifier, which notifications need.
        add_custom_command(TARGET ${name} POST_BUILD COMMAND codesign --force --sign - $<TARGET_BUNDLE_DIR:${name}>
            VERBATIM)
    endif()
endfunction()

# The draw gallery: animated scenes drawn with lib/draw alone, rendered on the GPU and mapped to the display.
pr_example(draw_gallery
    draw_gallery/main.cpp
    draw_gallery/scenes.cpp
    FONT_FILES "NotoSans[wdth,wght].ttf" "NotoSerif[wdth,wght].ttf" "NotoSansJP-VF.otf"
)
target_link_libraries(draw_gallery PRIVATE prax::app prax::draw prax::gpu
                                   prax::font prax::locale prax::text prax::core_cpp)

# The video example: a video that lib/media decodes and lib/draw draws. The device imports each frame without a copy
# where it can. ./dev.sh fetch videos fetches the example's video into third_party/videos.
pr_example(video
    video/main.cpp
    DATA PR_VIDEO_FILE ${PROJECT_SOURCE_DIR}/third_party/videos/cat.mp4
)
target_link_libraries(video PRIVATE prax::app prax::draw prax::gpu prax::media prax::core_cpp)

# The feature drawings: a captioned drawing of each feature of lib/draw's list, and dense workloads that measure its
# renderers. They are a library of their own, which the application and draw_bench link. Their fonts are the files of
# third_party/fonts, which the application's page packages at /data/fonts.
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
if (EMSCRIPTEN)
    target_compile_definitions(draw_features_drawings PRIVATE PR_FONTS_DIR="/data/fonts")
else()
    target_compile_definitions(draw_features_drawings PRIVATE PR_FONTS_DIR="${PROJECT_SOURCE_DIR}/third_party/fonts")
endif()
target_link_libraries(draw_features_drawings PUBLIC prax::app prax::draw prax::font prax::text prax::core_cpp
                                             PRIVATE prax::color prax::compress prax::image prax::path prax::svg
                                                     pr_warnings)

# The feature drawings in a window, rendered on the GPU and mapped to the display.
pr_example(draw_features draw_features/main.cpp DATA PR_FONTS_DIR ${PROJECT_SOURCE_DIR}/third_party/fonts)
target_link_libraries(draw_features PRIVATE draw_features_drawings prax::app prax::draw prax::gpu prax::text
                                            prax::core_cpp)

# The editor example: Studio Kit's widgets and a HUD library of its own over an editor's model, composed in one tree.
pr_example(editor
    editor/main.cpp
    editor/editor_state.cpp
    editor/editor_view.cpp
    editor/hud.cpp
    FONT_BUNDLES Complete
)
target_link_libraries(editor PRIVATE prax::studio_app prax::font_bundle prax::studio_kit prax::draw prax::layout prax::ui
                                     prax::ui_cpp prax::text prax::font prax::color prax::core_cpp)

# The application features: each feature of lib/app in Studio's windows, with a log of their events, to try by hand.
# Its bundle on macOS opens documents and a URL scheme.
pr_example(app_features
    app_features/main.cpp
    app_features/features.cpp
    app_features/pages_application.cpp
    app_features/pages_window.cpp
    app_features/pages_input.cpp
    app_features/pages_data.cpp
    app_features/pages_menus.cpp
    app_features/pages_device.cpp
    FONT_BUNDLES Standard14
    INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/app_features/Info.plist.in
)
target_link_libraries(app_features PRIVATE prax::studio_app prax::font_bundle prax::font prax::studio_kit prax::layout
                                           prax::ui prax::app prax::color prax::core_cpp)

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
    FONT_BUNDLES Complete
)
target_link_libraries(studio_gallery PRIVATE prax::studio_app prax::font_bundle prax::text_edit prax::studio_kit prax::draw
                                             prax::layout prax::ui prax::color prax::core_cpp)

# The perspective example: Kit panels standing in 3D over a background that the application draws with its own
# pipeline, their backdrops reading its pixels through the projection.
pr_example(perspective
    perspective/perspective.cpp
    perspective/perspective_gpu.cpp
    FONT_BUNDLES Complete
    DATA PR_PERSPECTIVE_PICTURE ${CMAKE_CURRENT_SOURCE_DIR}/perspective/picture.png
)
pr_slang(perspective perspective/perspective_gpu.slang VARIABLE kPerspectiveShaders)
target_link_libraries(perspective PRIVATE prax::app prax::studio_kit prax::draw
                                          prax::gpu prax::layout prax::text prax::font_bundle
                                          prax::image prax::compress prax::core_cpp)

# The agent harness example: an agent harness's shape, on the session transcripts of Claude Code.
pr_example(agent_harness
    agent_harness/agent_harness.cpp
    agent_harness/conversation.cpp
    agent_harness/transcript.cpp
    FONT_BUNDLES Complete
)
target_link_libraries(agent_harness PRIVATE prax::studio_app prax::font_bundle prax::app prax::studio_kit prax::ui prax::json
                                      prax::html prax::html_quickjs prax::svg_quickjs prax::mathml_quickjs
                                      prax::url_quickjs prax::charset_quickjs prax::xhr_quickjs prax::quickjs
                                      prax::fetch prax::net prax::http prax::tls prax::storage prax::dom prax::css
                                      prax::markdown prax::layout prax::gpu prax::text prax::draw
                                      prax::image prax::compress prax::core prax::core_cpp)

# The incremental example: Markdown and HTML rendered as they arrive in pieces, beside Kit's controls for the stream.
pr_example(incremental
    incremental/incremental.cpp
    FONT_BUNDLES Complete
)
target_link_libraries(incremental PRIVATE prax::studio_app prax::font_bundle prax::app prax::studio_kit prax::draw prax::layout
                                          prax::text prax::html prax::dom prax::css prax::markdown prax::core_cpp)
