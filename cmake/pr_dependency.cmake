# Fetches dependencies for the build, and for pr_font_bundle_fetch in a consumer's project.

include(FetchContent)

# FetchContent populates dependencies without a sub-build where CMake supports it. A sub-build's paths can exceed
# Windows' 260-character limit.
if (POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
endif()

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
            # A file that is already there with the right hash is not fetched again. One that is fetched is named as
            # its download starts, so that a slow download shows what the configure waits for.
            if (NOT EXISTS ${url})
                message(STATUS "Downloading ${name}: ${dependency_URL}")
            endif()
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

# Makes files of dependencies available in `directory`, each linked there under its own name, and only those: a file of
# the directory that no entry names is removed, so that what reads the directory reads only what the list gives it.
# Each file is named by its dependency, then its path in that dependency's content, as in
# font_NotoSans/NotoSans[wdth,wght].ttf. A file that two entries name is linked once.
function(pr_dependency_link directory)
    set(files ${ARGN})
    list(REMOVE_DUPLICATES files)
    file(MAKE_DIRECTORY "${directory}")
    set(names "")
    foreach (file IN LISTS files)
        get_filename_component(name "${file}" NAME)
        list(APPEND names "${name}")
    endforeach()
    file(GLOB present LIST_DIRECTORIES false RELATIVE "${directory}" "${directory}/*")
    foreach (name IN LISTS present)
        if (NOT name IN_LIST names)
            file(REMOVE "${directory}/${name}")
        endif()
    endforeach()
    foreach (file IN LISTS files)
        string(FIND "${file}" "/" slash)
        string(SUBSTRING "${file}" 0 ${slash} content)
        math(EXPR slash "${slash} + 1")
        string(SUBSTRING "${file}" ${slash} -1 path)
        pr_dependency_available(${content})
        string(TOLOWER "${content}" content)
        get_filename_component(name "${path}" NAME)
        # Windows makes a symbolic link only with Developer Mode or elevation, and copies otherwise.
        file(CREATE_LINK "${${content}_SOURCE_DIR}/${path}" "${directory}/${name}" SYMBOLIC COPY_ON_ERROR)
    endforeach()
endfunction()
