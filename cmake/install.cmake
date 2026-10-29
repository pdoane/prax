# Installs the libraries, their public headers, and the CMake package that find_package(prax) reads. Each library is the
# target prax::<name>, and a component of the package by its name.

include(CMakePackageConfigHelpers)

set(PR_PACKAGE_DIR ${CMAKE_INSTALL_LIBDIR}/cmake/prax)

# The libraries of the engine, and the third-party libraries that the build compiled for them, which they link
# privately.
get_property(PR_LIBRARIES GLOBAL PROPERTY PR_LIBRARIES)
list(FILTER PR_LIBRARIES EXCLUDE REGEX "^freetype$")
install(TARGETS ${PR_LIBRARIES} EXPORT prax-targets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    FILE_SET HEADERS DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
if (TARGET freetype)
    install(TARGETS freetype EXPORT prax-targets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
if (TARGET pr_core_mimalloc)
    install(TARGETS mimalloc-static EXPORT prax-targets ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
install(EXPORT prax-targets NAMESPACE prax:: DESTINATION ${PR_PACKAGE_DIR})

# What the package needs of the system to link the libraries that this build made.
set(PR_PACKAGE_DEPENDENCIES "")
if (PR_TLS_OPENSSL)
    string(APPEND PR_PACKAGE_DEPENDENCIES "find_dependency(OpenSSL 3...<4)\n")
endif()
if (TARGET pr_quickjs)
    string(APPEND PR_PACKAGE_DEPENDENCIES "find_dependency(Threads)\n")
endif()

configure_package_config_file(${CMAKE_CURRENT_LIST_DIR}/prax-config.cmake.in
    ${PROJECT_BINARY_DIR}/prax-config.cmake INSTALL_DESTINATION ${PR_PACKAGE_DIR})
# Prax is before 1.0, so a minor version may change the API.
write_basic_package_version_file(${PROJECT_BINARY_DIR}/prax-config-version.cmake COMPATIBILITY SameMinorVersion)
install(FILES
    ${PROJECT_BINARY_DIR}/prax-config.cmake
    ${PROJECT_BINARY_DIR}/prax-config-version.cmake
    ${PROJECT_SOURCE_DIR}/lib/font_bundle/font_bundle.cmake
    DESTINATION ${PR_PACKAGE_DIR})

install(FILES
    ${PROJECT_SOURCE_DIR}/LICENSE-MIT
    ${PROJECT_SOURCE_DIR}/LICENSE-APACHE
    ${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md
    DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/prax)
