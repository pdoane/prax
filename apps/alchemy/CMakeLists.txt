# Alchemy: a studio for generating images with a ComfyUI server. Its model, the server's protocol and the
# graphs that it submits are a library of their own, which the application and the tests link.
add_library(alchemy_studio STATIC
    comfy_client.cpp
    comfy_graph.cpp
    dimensions.cpp
    gallery.cpp
    image_cache.cpp
    models.cpp
    prompt_text.cpp
    provenance.cpp
    recipes.cpp
    state.cpp
)
target_include_directories(alchemy_studio PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(alchemy_studio PUBLIC prax::json prax::image prax::compress prax::net prax::http prax::websocket
                                            prax::draw prax::task prax::cache prax::studio_kit prax::core_cpp
                                     PRIVATE pr_warnings)

# The application: alchemy's interface over the library, composed in main().
pr_app(alchemy
    main.cpp
    commands.cpp
    view.cpp
)
target_link_libraries(alchemy PRIVATE alchemy_studio prax::studio_window prax::app prax::studio_command
                                      prax::studio_kit prax::draw prax::ui prax::net prax::http prax::tls prax::websocket
                                      prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(alchemy_studio
        tests/test_dimensions.cpp
        tests/test_models.cpp
        tests/test_prompt_text.cpp
        tests/test_provenance.cpp
        tests/test_recipes.cpp
        tests/test_state.cpp
    )
    target_link_libraries(alchemy_studio_tests PRIVATE prax::json prax::image prax::compress)

    # The client's tests, which talk to the fake ComfyUI server of tools/harness/net_peer.py.
    pr_net_tests(alchemy_comfy_tests tests/test_comfy_client.cpp)
    target_link_libraries(alchemy_comfy_tests PRIVATE alchemy_studio prax::http prax::websocket)
endif()
