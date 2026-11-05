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

# The web's build has only the draw and video examples. They run in a browser, so they read files packaged into their
# pages, not the host's files as the tests under Node.js do.
if (EMSCRIPTEN)
    get_directory_property(PR_EXAMPLE_LINK_OPTIONS LINK_OPTIONS)
    list(REMOVE_ITEM PR_EXAMPLE_LINK_OPTIONS -sNODERAWFS=1)
    set_directory_properties(PROPERTIES LINK_OPTIONS "${PR_EXAMPLE_LINK_OPTIONS}")
    pr_example(draw draw/main.cpp draw/scenes.cpp)
    target_link_libraries(draw PRIVATE prax::app prax::draw prax::gpu
                                       prax::font prax::locale prax::text prax::core_cpp)
    pr_example_fonts(draw "NotoSans[wdth,wght].ttf" "NotoSerif[wdth,wght].ttf" "NotoSansJP-VF.otf")
    pr_example(video video/main.cpp)
    target_link_libraries(video PRIVATE prax::app prax::draw prax::gpu prax::media prax::core_cpp)
    target_compile_definitions(video PRIVATE PR_VIDEO_FILE="/videos/cat.mp4")
    target_link_options(video PRIVATE
        "SHELL:--preload-file \"${PROJECT_SOURCE_DIR}/third_party/videos/cat.mp4@/videos/cat.mp4\"")
    return()
endif()

# Scribe: Studio's text editor in a window, whose operations are commands of lib/studio/command.
# Its interface is a library of its own, which the application and the tests of its interface share.
add_library(scribe_editor STATIC scribe/scribe.cpp)
target_include_directories(scribe_editor PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(scribe_editor PUBLIC prax::app prax::process prax::syntax prax::studio_command prax::studio_kit
                                           prax::studio_text_editor prax::layout prax::ui prax::core_cpp
                                    PRIVATE pr_warnings)
if (PR_SYNTAX_TREESITTER)
    target_compile_definitions(scribe_editor PUBLIC SCRIBE_HAS_TREESITTER)
endif()

pr_example(scribe scribe/main.cpp)
target_link_libraries(scribe PRIVATE scribe_editor prax::app prax::draw prax::gpu prax::font prax::font_bundle
                                     prax::layout prax::studio_kit prax::text prax::ui)

if (PR_DEVELOP)
    pr_module_tests(scribe_editor scribe/tests/test_scribe.cpp)
    target_link_libraries(scribe_editor_tests PRIVATE kit_fixture prax::font prax::layout prax::text prax::ui)
endif()

# The draw example: animated scenes drawn with lib/draw alone, rendered on the GPU and mapped to the display.
pr_example(draw draw/main.cpp draw/scenes.cpp)
target_link_libraries(draw PRIVATE prax::app prax::draw prax::gpu
                                   prax::font prax::locale prax::text prax::core_cpp)

# The video example: a video that lib/media decodes and lib/draw draws. The device imports each frame without a copy
# where it can. ./dev.sh fetch videos fetches the example's video into third_party/videos.
pr_example(video video/main.cpp)
target_link_libraries(video PRIVATE prax::app prax::draw prax::gpu prax::media prax::core_cpp)
target_compile_definitions(video PRIVATE PR_VIDEO_FILE="${PROJECT_SOURCE_DIR}/third_party/videos/cat.mp4")

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

# The alchemy example: a studio for generating images with a ComfyUI server. Its model, the server's protocol and the
# graphs that it submits are a library of their own, which the application and the tests link.
add_library(alchemy_studio STATIC
    alchemy/comfy_client.cpp
    alchemy/comfy_graph.cpp
    alchemy/dimensions.cpp
    alchemy/gallery.cpp
    alchemy/image_cache.cpp
    alchemy/models.cpp
    alchemy/prompt_text.cpp
    alchemy/provenance.cpp
    alchemy/recipes.cpp
    alchemy/state.cpp
)
target_include_directories(alchemy_studio PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(alchemy_studio PUBLIC prax::json prax::image prax::compress prax::net prax::http prax::websocket
                                            prax::draw prax::task prax::cache prax::studio_kit prax::core_cpp
                                     PRIVATE pr_warnings)

# The application: alchemy's interface over the library, composed in main().
pr_example(alchemy
    alchemy/main.cpp
    alchemy/commands.cpp
    alchemy/view.cpp
)
target_link_libraries(alchemy PRIVATE alchemy_studio prax::app prax::studio_command prax::studio_kit prax::draw
                                      prax::gpu prax::layout prax::ui prax::text prax::font prax::font_bundle
                                      prax::net prax::http prax::tls prax::websocket prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(alchemy_studio
        alchemy/tests/test_dimensions.cpp
        alchemy/tests/test_models.cpp
        alchemy/tests/test_prompt_text.cpp
        alchemy/tests/test_provenance.cpp
        alchemy/tests/test_recipes.cpp
        alchemy/tests/test_state.cpp
    )
    target_link_libraries(alchemy_studio_tests PRIVATE prax::json prax::image prax::compress)

    # The client's tests, which talk to the fake ComfyUI server of tools/harness/net_peer.py.
    pr_net_tests(alchemy_comfy_tests alchemy/tests/test_comfy_client.cpp)
    target_link_libraries(alchemy_comfy_tests PRIVATE alchemy_studio prax::http prax::websocket)
endif()

# The browser example: a tabbed web browser, whose pages' scripts run, rendered through lib/layout, inside chrome built
# with Studio Kit. Its addresses, favorites and history are a library of their own, which the application and the tests
# link.
add_library(browser_model STATIC browser/address.cpp browser/favorites.cpp browser/history.cpp browser/profile.cpp)
target_include_directories(browser_model PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(browser_model PUBLIC prax::json prax::core_cpp PRIVATE pr_warnings)

pr_example(browser browser/main.cpp browser/browser.cpp browser/chrome.cpp browser/commands.cpp browser/icons.cpp
    browser/tab.cpp)
target_link_libraries(browser PRIVATE browser_model prax::app prax::html_quickjs prax::url_quickjs prax::xhr_quickjs
                                      prax::charset_quickjs prax::svg_quickjs prax::mathml_quickjs
                                      prax::studio_kit prax::studio_command prax::ui
                                      prax::text prax::font_bundle prax::font
                                      prax::audio prax::draw prax::gpu prax::image prax::media
                                      prax::compress prax::fetch prax::html prax::css prax::dom prax::layout
                                      prax::net prax::http prax::tls prax::storage prax::cache
                                      prax::task prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(browser_model browser/tests/test_address.cpp browser/tests/test_favorites.cpp
        browser/tests/test_history.cpp)
endif()
