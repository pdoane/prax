# The apps: applications for daily use, each over a library of its own that holds its model and that its tests link,
# and composed in its own main(). They link the libraries as a consumer does, by their prax:: names. An app includes its
# own headers from the repository's root, as apps/<name>/<header>. An app's target is named for its directory, which
# `./dev.sh app <name>` builds and runs.

# Adds the app `name`, built from `sources`, which reads the font bundles' files from the build directory.
function(pr_app name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE ${PROJECT_SOURCE_DIR})
    target_link_libraries(${name} PRIVATE pr_warnings)
    target_compile_definitions(${name} PRIVATE PR_FONT_BUNDLE_DIR="${PR_FONT_BUNDLE_DIR}")
    if (PR_UIKIT)
        pr_uikit_bundle(${name})
    endif()
endfunction()

add_subdirectory(alchemy)
add_subdirectory(browser)
add_subdirectory(scribe)
add_subdirectory(merge)
add_subdirectory(terminal)
