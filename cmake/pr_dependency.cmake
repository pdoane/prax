# Fetches dependencies for the build, and for pr_font_bundle_fetch in a consumer's project.

include(FetchContent)

# The download cache, which an empty PR_DOWNLOAD_DIR disables. A machine downloads each file once, across build
# directories and projects. FetchContent has no shared cache, and the policies of the calling project decide where it
# downloads. So this file downloads each dependency and passes FetchContent the local copy.
if (DEFINED ENV{PR_DOWNLOAD_DIR})
    set(_pr_download_dir "$ENV{PR_DOWNLOAD_DIR}")
elseif (CMAKE_HOST_WIN32 AND DEFINED ENV{LOCALAPPDATA})
    set(_pr_download_dir "$ENV{LOCALAPPDATA}/prax/downloads")
elseif (CMAKE_HOST_APPLE AND DEFINED ENV{HOME})
    set(_pr_download_dir "$ENV{HOME}/Library/Caches/prax/downloads")
elseif (DEFINED ENV{XDG_CACHE_HOME})
    set(_pr_download_dir "$ENV{XDG_CACHE_HOME}/prax/downloads")
elseif (DEFINED ENV{HOME})
    set(_pr_download_dir "$ENV{HOME}/.cache/prax/downloads")
else()
    set(_pr_download_dir "")
endif()
file(TO_CMAKE_PATH "${_pr_download_dir}" _pr_download_dir)
set(PR_DOWNLOAD_DIR "${_pr_download_dir}" CACHE PATH "The download cache that builds share")

# Records a dependency, with FetchContent_Declare's arguments. Nothing is downloaded until pr_dependency_available names
# the dependency.
function(pr_dependency name)
    set_property(GLOBAL PROPERTY PR_DEPENDENCY_${name} "${ARGN}")
endfunction()

function(_pr_dependency_declare)
    foreach (name IN LISTS ARGN)
        get_property(arguments GLOBAL PROPERTY PR_DEPENDENCY_${name})
        cmake_parse_arguments(dependency "" "URL;URL_HASH;DOWNLOAD_NAME" "" ${arguments})
        set(url ${dependency_URL})
        string(TOUPPER ${name} upper)
        if (PR_DOWNLOAD_DIR AND NOT FETCHCONTENT_SOURCE_DIR_${upper})
            if (dependency_DOWNLOAD_NAME)
                set(file ${dependency_DOWNLOAD_NAME})
            else()
                get_filename_component(file ${dependency_URL} NAME)
            endif()
            set(url ${PR_DOWNLOAD_DIR}/${name}/${file})
            # A file that is already there with the right hash is not fetched again.
            file(DOWNLOAD ${dependency_URL} ${url} EXPECTED_HASH ${dependency_URL_HASH})
        endif()
        if (dependency_DOWNLOAD_NAME)
            set(download_name DOWNLOAD_NAME ${dependency_DOWNLOAD_NAME})
        else()
            set(download_name "")
        endif()
        # CMake refuses this option on a file that it does not extract, such as a font.
        if (DOWNLOAD_NO_EXTRACT IN_LIST arguments)
            set(timestamp "")
        else()
            set(timestamp DOWNLOAD_EXTRACT_TIMESTAMP ON)
        endif()
        FetchContent_Declare(${name} URL ${url} URL_HASH ${dependency_URL_HASH} ${download_name}
            ${dependency_UNPARSED_ARGUMENTS} ${timestamp})
    endforeach()
endfunction()

# Downloads the dependencies named and makes them available. This is a macro because FetchContent_MakeAvailable sets each
# dependency's variables in its caller's scope.
macro(pr_dependency_available)
    _pr_dependency_declare(${ARGN})
    FetchContent_MakeAvailable(${ARGN})
endmacro()
