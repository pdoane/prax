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
#   pr_example(<name> <source>... [FONT_BUNDLES <bundle>...] [FONT_FILES <file>...] [DATA <path>...]
#       [INFO_PLIST <file>] [MULTIPLE_SCENES])
#
# The example's resources (pr/resource.h) are the files of FONT_BUNDLES and the FONT_FILES of PR_FONT_BUNDLE_DIR, each
# font_bundle/<file>, and each DATA path, a file or a directory, under its last part. On the web, the example is a page,
# <name>.html, which packages its resources. On macOS, it is an application bundle, org.prax.<name>, which INFO_PLIST
# describes if it is given. On a UIKit platform, it is an application bundle of the same identifier (pr_uikit_bundle),
# which shows several scenes with MULTIPLE_SCENES.
function(pr_example name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "MULTIPLE_SCENES" "INFO_PLIST" "FONT_BUNDLES;FONT_FILES;DATA")
    add_executable(${name} ${arg_UNPARSED_ARGUMENTS})
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR})
    target_link_libraries(${name} PRIVATE pr_warnings)
    if (EMSCRIPTEN)
        set_target_properties(${name} PROPERTIES SUFFIX ".html" PR_WEB_PAGE TRUE)
        target_link_options(${name} PRIVATE "--shell-file=${CMAKE_CURRENT_SOURCE_DIR}/shell.html")
        set_property(TARGET ${name} APPEND PROPERTY LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shell.html)
    endif()
    if (arg_FONT_BUNDLES)
        pr_font_bundle_resources(${name} ${arg_FONT_BUNDLES})
    endif()
    set(resources)
    foreach (file IN LISTS arg_FONT_FILES)
        list(APPEND resources "${PR_FONT_BUNDLE_DIR}/${file}@font_bundle/${file}")
    endforeach()
    foreach (path IN LISTS arg_DATA)
        get_filename_component(packaged "${path}" NAME)
        list(APPEND resources "${path}@${packaged}")
    endforeach()
    if (resources)
        pr_resources(${name} ${resources})
    endif()
    if (PR_UIKIT AND arg_MULTIPLE_SCENES)
        pr_uikit_bundle(${name} MULTIPLE_SCENES)
    elseif (PR_UIKIT)
        pr_uikit_bundle(${name})
    elseif (APPLE)
        set_target_properties(${name} PROPERTIES MACOSX_BUNDLE TRUE MACOSX_BUNDLE_GUI_IDENTIFIER org.prax.${name})
        if (arg_INFO_PLIST)
            set_target_properties(${name} PROPERTIES MACOSX_BUNDLE_INFO_PLIST ${arg_INFO_PLIST})
        endif()
        pr_sign_bundle(${name})
    endif()
endfunction()

# The draw gallery: animated scenes drawn with lib/draw alone, rendered on the GPU and mapped to the display.
pr_example(draw_gallery
    draw_gallery/main.cpp
    draw_gallery/scenes.cpp
    FONT_FILES "NotoSans[wdth,wght].ttf" "NotoSerif[wdth,wght].ttf" "NotoSansJP-VF.otf"
)
target_link_libraries(draw_gallery PRIVATE prax::app prax::draw prax::gpu prax::task
                                   prax::font prax::locale prax::text prax::core_cpp)

# The video example: a video that lib/media decodes and lib/draw draws. The device imports each frame without a copy
# where it can. ./dev.sh fetch videos fetches the example's video into third_party/videos.
pr_example(video
    video/main.cpp
    DATA ${PROJECT_SOURCE_DIR}/third_party/videos/cat.mp4
)
target_link_libraries(video PRIVATE prax::app prax::draw prax::gpu prax::media prax::task prax::core_cpp)

# The feature drawings: a captioned drawing of each feature of lib/draw's list, and dense workloads that measure its
# renderers. They are a library of their own, which the application and draw_bench link. Their fonts are the files of
# third_party/fonts, which are the library's resources under fonts.
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
pr_resources(draw_features_drawings ${PROJECT_SOURCE_DIR}/third_party/fonts@fonts)
target_link_libraries(draw_features_drawings PUBLIC prax::app prax::draw prax::font prax::text prax::core_cpp
                                             PRIVATE prax::color prax::compress prax::image prax::path prax::svg
                                                     pr_warnings)

# The feature drawings in a window, rendered on the GPU and mapped to the display, with the glyphs that are pictures
# decoded by lib/svg's decoder.
pr_example(draw_features draw_features/main.cpp)
target_link_libraries(draw_features PRIVATE draw_features_drawings prax::app prax::compress prax::draw prax::gpu
                                            prax::image prax::svg prax::task prax::text prax::core_cpp)

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
    MULTIPLE_SCENES
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
    DATA ${CMAKE_CURRENT_SOURCE_DIR}/perspective/picture.png
)
pr_slang(perspective perspective/perspective_gpu.slang VARIABLE kPerspectiveShaders)
target_link_libraries(perspective PRIVATE prax::app prax::studio_kit prax::draw
                                          prax::gpu prax::layout prax::task prax::text prax::font_bundle
                                          prax::image prax::compress prax::core_cpp)

# The incremental example: Markdown and HTML rendered as they arrive in pieces, beside Kit's controls for the stream.
pr_example(incremental
    incremental/incremental.cpp
    FONT_BUNDLES Complete
)
target_link_libraries(incremental PRIVATE prax::studio_app prax::font_bundle prax::app prax::studio_kit prax::draw prax::layout
                                          prax::text prax::html prax::dom prax::css prax::markdown prax::core_cpp)

# The word guess game: six guesses at a hidden five-letter word, built with lib/ui's elements (pr/ui_element.h) in the
# system's fonts, or on the web in a font that it packages. Its rules are a library of their own, which the game and
# its tests link.
add_library(word_guess_game STATIC word_guess/word_guess_game.cpp)
target_include_directories(word_guess_game PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(word_guess_game PUBLIC prax::core_cpp PRIVATE pr_warnings)
pr_example(word_guess
    word_guess/main.cpp
    word_guess/word_guess_view.cpp
    FONT_FILES "NotoSans[wdth,wght].ttf"
)
target_link_libraries(word_guess PRIVATE word_guess_game prax::studio_app prax::font prax::ui_cpp prax::core_cpp)
if (PR_DEVELOP)
    pr_module_tests(word_guess_game word_guess/tests/test_word_guess_game.cpp)
endif()

# The crossword: clued answers to fill in a grid, built with lib/ui's elements (pr/ui_element.h) with Studio Kit's
# icons, in the system's fonts, or on the web in a font that it packages. Its rules and its puzzles are a library of
# their own, which the game and its tests link.
add_library(crossword_game STATIC crossword/crossword_game.cpp crossword/puzzles.cpp)
target_include_directories(crossword_game PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(crossword_game PUBLIC prax::core_cpp PRIVATE pr_warnings)
pr_example(crossword
    crossword/main.cpp
    crossword/crossword_view.cpp
    FONT_FILES "NotoSans[wdth,wght].ttf"
)
target_link_libraries(crossword PRIVATE crossword_game prax::studio_app prax::studio_icons prax::font prax::ui_cpp
                                        prax::app prax::core_cpp)
if (PR_DEVELOP)
    pr_module_tests(crossword_game crossword/tests/test_crossword_game.cpp)
endif()
