# The browser: a tabbed web browser, whose pages' scripts run, rendered through lib/layout, inside chrome built with
# Studio Kit. Its addresses, favorites and history are a library of their own, which the application and the tests link.
add_library(browser_model STATIC address.cpp favorites.cpp history.cpp profile.cpp)
target_include_directories(browser_model PUBLIC ${PROJECT_SOURCE_DIR})
target_link_libraries(browser_model PUBLIC prax::json prax::core_cpp PRIVATE prax::url pr_warnings)

pr_app(browser main.cpp browser.cpp chrome.cpp commands.cpp icons.cpp tab.cpp)
target_link_libraries(browser PRIVATE browser_model prax::app prax::html_quickjs prax::url_quickjs prax::xhr_quickjs
                                      prax::charset_quickjs prax::svg_quickjs prax::mathml_quickjs
                                      prax::studio_kit prax::studio_command prax::ui
                                      prax::text prax::font
                                      prax::audio prax::draw prax::gpu prax::image prax::media
                                      prax::compress prax::fetch prax::html prax::css prax::dom prax::layout
                                      prax::net prax::http prax::tls prax::storage prax::cache
                                      prax::task prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(browser_model tests/test_address.cpp tests/test_favorites.cpp tests/test_history.cpp)
    # The tests' own directory, as an absolute path: __FILE__ is relative under a compiler cache with a base directory,
    # as CI's is.
    target_compile_definitions(browser_model_tests PRIVATE BROWSER_TESTS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests")
endif()
