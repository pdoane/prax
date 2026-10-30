# cmark-gfm, built with the headers that its own build would configure, and the harness that writes its HTML, which
# tools/suite/markdown/markdown_compare.py compares lib/markdown's with.
pr_dependency_available(cmark_gfm)
set(PR_CMARK_GFM_HEADERS ${CMAKE_CURRENT_BINARY_DIR}/cmark-gfm)
block(SCOPE_FOR VARIABLES)
    # Every compiler has stdbool.h, and where C makes bool a keyword, the header's fallback typedef does not compile.
    set(HAVE_STDBOOL_H 1)
    if (NOT MSVC)
        set(HAVE___BUILTIN_EXPECT 1)
        set(HAVE___ATTRIBUTE__ 1)
    endif()
    configure_file(${cmark_gfm_SOURCE_DIR}/src/config.h.in ${PR_CMARK_GFM_HEADERS}/config.h)
    set(PROJECT_VERSION_MAJOR 0)
    set(PROJECT_VERSION_MINOR 29)
    set(PROJECT_VERSION_PATCH 0)
    set(PROJECT_VERSION_GFM 13)
    configure_file(${cmark_gfm_SOURCE_DIR}/src/cmark-gfm_version.h.in ${PR_CMARK_GFM_HEADERS}/cmark-gfm_version.h)
endblock()
file(WRITE ${PR_CMARK_GFM_HEADERS}/cmark-gfm_export.h
     "#ifndef CMARK_GFM_EXPORT_H\n#define CMARK_GFM_EXPORT_H\n#define CMARK_GFM_EXPORT\n#endif\n")
file(GLOB PR_CMARK_GFM_SOURCES CONFIGURE_DEPENDS ${cmark_gfm_SOURCE_DIR}/src/*.c ${cmark_gfm_SOURCE_DIR}/extensions/*.c)
list(FILTER PR_CMARK_GFM_SOURCES EXCLUDE REGEX "/main\\.c$")
add_library(cmark_gfm STATIC ${PR_CMARK_GFM_SOURCES})
target_include_directories(cmark_gfm SYSTEM PUBLIC ${cmark_gfm_SOURCE_DIR}/src ${cmark_gfm_SOURCE_DIR}/extensions
                                                   ${PR_CMARK_GFM_HEADERS})
pr_third_party(cmark_gfm)
pr_harness(cmark_gfm_reference cmark_gfm cmark_gfm_reference.cpp)
