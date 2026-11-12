# The DOM: documents, their nodes and events.
pr_library(dom INCLUDE include
    src/dom.cpp
    src/dom_geometry.cpp
    src/dom_class.cpp
    src/dom_agent.cpp
    src/dom_attribute.cpp
    src/dom_collection.cpp
    src/dom_control.cpp
    src/dom_custom.cpp
    src/dom_element_state.cpp
    src/dom_abort.cpp
    src/dom_event.cpp
    src/dom_event_handler.cpp
    src/dom_flat.cpp
    src/dom_hooks.cpp
    src/dom_microsyntax.cpp
    src/dom_mutation.cpp
    src/dom_node.cpp
    src/dom_range.cpp
    src/dom_select.cpp
    src/dom_selection.cpp
    src/dom_slot.cpp
    src/dom_tag.cpp
    src/dom_traversal.cpp
    src/dom_use.cpp
    src/dom_resource.cpp
    src/dom_xml.cpp
)
target_link_libraries(pr_dom PUBLIC prax::core prax::url prax::xml prax::layout prax::draw
                                    prax::task prax::color prax::text_edit
                             PRIVATE prax::core_cpp prax::charset prax::unicode prax::compress prax::image)

if (PR_JS)
    pr_library(dom_quickjs INCLUDE quickjs/include
        quickjs/src/dom_geometry_quickjs.cpp
        quickjs/src/dom_quickjs.cpp
    )
    target_link_libraries(pr_dom_quickjs PUBLIC prax::dom prax::quickjs PRIVATE prax::core_quickjs_cpp
                                         prax::webidl_quickjs_cpp)
endif()

if (PR_DEVELOP)
    pr_module_tests(dom
        tests/test_collection.cpp
        tests/test_abort.cpp
        tests/test_agent.cpp
        tests/test_event.cpp
        tests/test_mutation.cpp
        tests/test_traversal.cpp
        tests/test_node.cpp
        tests/test_select.cpp
        tests/test_control.cpp
        tests/test_dom_resource.cpp
        tests/test_dom_use.cpp
        tests/test_dom_xml.cpp
    )
    target_link_libraries(dom_tests PRIVATE prax::url)

    if (PR_JS)
        pr_module_tests(dom_quickjs
            quickjs/tests/test_exception_quickjs.cpp
        )
        target_link_libraries(dom_quickjs_tests PRIVATE prax::css_quickjs prax::core_quickjs_cpp prax::webidl_quickjs_cpp)
        pr_add_test(dom_js_callbacks COMMAND pr_shell --html ${CMAKE_CURRENT_SOURCE_DIR}/tests/callback.html)
    endif()
endif()
