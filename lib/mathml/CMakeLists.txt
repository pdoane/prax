# MathML elements.
pr_library(mathml INCLUDE include
    src/mathml.cpp
)
target_link_libraries(pr_mathml PUBLIC prax::dom PRIVATE prax::core_cpp)

if (PR_JS)
    pr_library(mathml_quickjs INCLUDE quickjs/include
        quickjs/src/mathml_quickjs.cpp
    )
    target_link_libraries(pr_mathml_quickjs PUBLIC prax::mathml prax::quickjs prax::css_quickjs prax::dom_quickjs
                                            PRIVATE prax::core_quickjs_cpp prax::webidl_quickjs_cpp)
endif()
