# XML, with XML namespaces.
pr_library(xml INCLUDE include
    src/xml.cpp
)
target_link_libraries(pr_xml PUBLIC prax::core prax::charset PRIVATE prax::core_cpp)

if (PR_DEVELOP)
    pr_module_tests(xml
        tests/test_xml.cpp
    )
endif()
