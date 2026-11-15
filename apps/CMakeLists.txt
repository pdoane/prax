# The apps: applications for daily use, each over a library of its own that holds its model and that its tests link,
# and composed in its own main(). They link the libraries as a consumer does, by their prax:: names. An app includes its
# own headers from the repository's root, as apps/<name>/<header>. An app's target is named for its directory, which
# `./dev.sh app <name>` builds and runs.

# Adds the app `name`, built from its sources, whose resources (pr/resource.h) are the files of FONT_BUNDLES:
#
#   pr_app(<name> <source>... [FONT_BUNDLES <bundle>...])
function(pr_app name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "FONT_BUNDLES")
    add_executable(${name} ${arg_UNPARSED_ARGUMENTS})
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR})
    target_link_libraries(${name} PRIVATE pr_warnings)
    if (arg_FONT_BUNDLES)
        pr_font_bundle_resources(${name} ${arg_FONT_BUNDLES})
    endif()
    if (PR_IOS)
        pr_ios_bundle(${name})
    endif()
endfunction()

add_subdirectory(alchemy)
add_subdirectory(browser)
add_subdirectory(scribe)
add_subdirectory(merge)
add_subdirectory(terminal)
