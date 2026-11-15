# Stages a program's resources (pr_stage_resources in cmake/resources.cmake), as a step of building the program.
#
#   cmake -DMANIFEST=<file> -DDESTINATION=<directory> -DSTAGED=<file> -P stage_resources.cmake
#
# MANIFEST lists the resources, one <source>|<name> a line. Each file is staged in DESTINATION under its name: a hard
# link to its source where the file system allows, and else a copy that is made again only when the source differs. A
# source that is missing is left out. STAGED lists what the last staging made, so that a resource that is no longer
# declared is removed, with the directories that it leaves empty, and it is written last, as the step's output.

file(STRINGS "${MANIFEST}" entries)
# The names of the files to stage, and their sources, in the same order.
set(staged)
set(sources)
foreach (entry IN LISTS entries)
    string(REGEX MATCH "^(.+)\\|([^|]+)$" _ "${entry}")
    set(source "${CMAKE_MATCH_1}")
    set(name "${CMAKE_MATCH_2}")
    if (IS_DIRECTORY "${source}")
        file(GLOB_RECURSE files LIST_DIRECTORIES false RELATIVE "${source}" "${source}/*")
        foreach (file IN LISTS files)
            list(APPEND staged "${name}/${file}")
            list(APPEND sources "${source}/${file}")
        endforeach()
    elseif (EXISTS "${source}")
        list(APPEND staged "${name}")
        list(APPEND sources "${source}")
    endif()
endforeach()

list(LENGTH staged count)
if (count GREATER 0)
    math(EXPR last "${count} - 1")
    foreach (i RANGE ${last})
        list(GET staged ${i} name)
        list(GET sources ${i} source)
        set(target "${DESTINATION}/${name}")
        get_filename_component(parent "${target}" DIRECTORY)
        file(MAKE_DIRECTORY "${parent}")
        # A link of a link links its target, which is the file that the link names. A link replaces the file there.
        file(REAL_PATH "${source}" source)
        file(CREATE_LINK "${source}" "${target}" RESULT linked)
        if (NOT linked EQUAL 0)
            file(COPY_FILE "${source}" "${target}" ONLY_IF_DIFFERENT)
        endif()
    endforeach()
endif()

# What the last staging made and this one did not is no longer a resource.
if (EXISTS "${STAGED}")
    file(STRINGS "${STAGED}" previous)
    foreach (name IN LISTS previous)
        if (NOT name IN_LIST staged)
            file(REMOVE "${DESTINATION}/${name}")
            # The directories that held only what is no longer a resource go with it.
            get_filename_component(parent "${name}" DIRECTORY)
            while (parent)
                file(GLOB left "${DESTINATION}/${parent}/*")
                if (left)
                    break()
                endif()
                file(REMOVE_RECURSE "${DESTINATION}/${parent}")
                get_filename_component(parent "${parent}" DIRECTORY)
            endwhile()
        endif()
    endforeach()
endif()
list(JOIN staged "\n" listed)
file(WRITE "${STAGED}" "${listed}\n")
