# A program's resources (pr/resource.h): the files that it is packaged with, which it reads by name. A target declares
# them, a library's reach each program that links it, and each program's are staged where its platform keeps them when
# the program is built.

# Declares the resources of `target`, each <source>@<name>: the file at <source> is the resource <name>, and each file
# under a directory at <source> is a resource under <name>, by its path in the directory. A library's resources are the
# resources of each program that links it, directly or through other libraries. A source that is missing when the
# program is built is left out, as an optional download is until it is fetched, and the program finds no such resource.
#
#   pr_resources(<target> <source>@<name>...)
function(pr_resources target)
    foreach (resource IN LISTS ARGN)
        if (NOT resource MATCHES "^(.+)@([^@]+)$")
            message(FATAL_ERROR "pr_resources: ${resource} is not <source>@<name>")
        endif()
    endforeach()
    set_property(TARGET ${target} APPEND PROPERTY PR_RESOURCES ${ARGN})
endfunction()

# Fails unless `name`, a resource of `program`, is well formed (prResourceNameIsValid).
function(_pr_check_resource_name program name)
    string(REPLACE "/" ";" parts "${name}")
    if (name MATCHES "^/|//|/$|\\\\" OR "." IN_LIST parts OR ".." IN_LIST parts OR name STREQUAL "")
        message(FATAL_ERROR "the resource ${name} of ${program} is not a well-formed name (pr/resource.h)")
    endif()
endfunction()

# Appends to `out` the targets that `target` links, directly or through other libraries, after resolving aliases.
# `visited` names the targets already walked.
function(_pr_linked_targets target out visited)
    set(found ${${out}})
    set(walked ${${visited}})
    get_target_property(type ${target} TYPE)
    set(properties INTERFACE_LINK_LIBRARIES)
    if (NOT type STREQUAL "INTERFACE_LIBRARY")
        list(APPEND properties LINK_LIBRARIES)
    endif()
    foreach (property IN LISTS properties)
        get_target_property(libraries ${target} ${property})
        if (NOT libraries)
            continue()
        endif()
        foreach (library IN LISTS libraries)
            # A link of the build's own tree, or one only linked, names the target within the expression.
            string(REGEX REPLACE "^\\$<(LINK_ONLY|BUILD_INTERFACE|BUILD_LOCAL_INTERFACE):(.*)>$" "\\2" library
                "${library}")
            if (NOT TARGET ${library})
                continue()
            endif()
            get_target_property(aliased ${library} ALIASED_TARGET)
            if (aliased)
                set(library ${aliased})
            endif()
            get_target_property(imported ${library} IMPORTED)
            if (imported OR library IN_LIST walked)
                continue()
            endif()
            list(APPEND walked ${library})
            list(APPEND found ${library})
            _pr_linked_targets(${library} found walked)
        endforeach()
    endforeach()
    set(${out} ${found} PARENT_SCOPE)
    set(${visited} ${walked} PARENT_SCOPE)
endfunction()

# Appends the executables of `directory` and of the directories under it to `out`.
function(_pr_executables directory out)
    set(found ${${out}})
    get_directory_property(targets DIRECTORY ${directory} BUILDSYSTEM_TARGETS)
    foreach (target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if (type STREQUAL "EXECUTABLE")
            list(APPEND found ${target})
        endif()
    endforeach()
    get_directory_property(subdirectories DIRECTORY ${directory} SUBDIRECTORIES)
    foreach (subdirectory IN LISTS subdirectories)
        _pr_executables(${subdirectory} found)
    endforeach()
    set(${out} ${found} PARENT_SCOPE)
endfunction()

# Stages the resources of each program of the build, which the top-level list calls once every target is declared.
#
# A web page packages its resources into its file system, under /resources. Any other program's are staged before it is
# linked: an application bundle's in its resources directory, the bundle itself on iOS and Contents/Resources on macOS,
# and any other program's in <executable>.resources beside its executable, as a program under Node.js does beside its
# script. Each resource is a hard link to its source where the file system allows, so that staging copies nothing, and
# a copy elsewhere. The resources are staged again when a source changes, and the program is then linked again, so that
# what its build does after the link, such as signing a bundle (pr_sign_bundle), sees them.
function(pr_stage_resources)
    _pr_executables(${PROJECT_SOURCE_DIR} programs)
    foreach (program IN LISTS programs)
        get_target_property(resources ${program} PR_RESOURCES)
        if (NOT resources)
            set(resources)
        endif()
        set(linked)
        set(visited)
        _pr_linked_targets(${program} linked visited)
        foreach (library IN LISTS linked)
            get_target_property(carried ${library} PR_RESOURCES)
            if (carried)
                list(APPEND resources ${carried})
            endif()
        endforeach()
        if (NOT resources)
            continue()
        endif()
        list(REMOVE_DUPLICATES resources)

        # The files of the sources, which the program's resources are staged again for when they change. A source that
        # is missing, as an optional download is until it is fetched, and a directory that gains a file, configure the
        # build again, which finds the file.
        set(manifest "")
        set(sources)
        set(preload)
        set(names)
        foreach (resource IN LISTS resources)
            string(REGEX MATCH "^(.+)@([^@]+)$" _ "${resource}")
            set(source "${CMAKE_MATCH_1}")
            set(name "${CMAKE_MATCH_2}")
            _pr_check_resource_name(${program} "${name}")
            # Two sources of one name, or of a name within another's directory, would stage over each other.
            foreach (other IN LISTS names)
                string(FIND "${name}/" "${other}/" nameInOther)
                string(FIND "${other}/" "${name}/" otherInName)
                if (nameInOther EQUAL 0 OR otherInName EQUAL 0)
                    message(FATAL_ERROR "the resources of ${program} name ${name} and ${other}, which stage over each "
                        "other")
                endif()
            endforeach()
            list(APPEND names "${name}")
            string(APPEND manifest "${source}|${name}\n")
            if (IS_DIRECTORY "${source}")
                file(GLOB_RECURSE files LIST_DIRECTORIES false CONFIGURE_DEPENDS "${source}/*")
                list(APPEND sources ${files})
                list(APPEND preload "${source}@/resources/${name}")
            elseif (EXISTS "${source}")
                list(APPEND sources "${source}")
                list(APPEND preload "${source}@/resources/${name}")
            else()
                get_filename_component(parent "${source}" DIRECTORY)
                while (NOT IS_DIRECTORY "${parent}")
                    get_filename_component(parent "${parent}" DIRECTORY)
                endwhile()
                file(GLOB _ CONFIGURE_DEPENDS "${parent}/*")
            endif()
        endforeach()

        get_target_property(page ${program} PR_WEB_PAGE)
        if (page)
            pr_web_preload(${program} ${preload})
            set_property(TARGET ${program} APPEND PROPERTY LINK_DEPENDS ${sources})
            continue()
        endif()

        get_target_property(bundle ${program} MACOSX_BUNDLE)
        if (bundle AND PR_UIKIT)
            set(destination $<TARGET_BUNDLE_DIR:${program}>)
        elseif (bundle)
            set(destination $<TARGET_BUNDLE_CONTENT_DIR:${program}>/Resources)
        else()
            set(destination $<TARGET_FILE_DIR:${program}>/$<TARGET_FILE_BASE_NAME:${program}>.resources)
        endif()
        set(staging ${PROJECT_BINARY_DIR}/resources/${program})
        file(WRITE ${staging}/manifest.txt.in "${manifest}")
        configure_file(${staging}/manifest.txt.in ${staging}/manifest.txt COPYONLY)
        add_custom_command(OUTPUT ${staging}/staged.txt
            COMMAND ${CMAKE_COMMAND} -DMANIFEST=${staging}/manifest.txt -DDESTINATION=${destination}
                -DSTAGED=${staging}/staged.txt -P ${PROJECT_SOURCE_DIR}/cmake/stage_resources.cmake
            DEPENDS ${staging}/manifest.txt ${sources} ${PROJECT_SOURCE_DIR}/cmake/stage_resources.cmake
            COMMENT "Staging the resources of ${program}"
            VERBATIM)
        add_custom_target(${program}_resources DEPENDS ${staging}/staged.txt)
        add_dependencies(${program} ${program}_resources)
        set_property(TARGET ${program} APPEND PROPERTY LINK_DEPENDS ${staging}/staged.txt)
    endforeach()
endfunction()
