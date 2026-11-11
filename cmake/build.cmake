# The functions that the build's CMakeLists.txt files declare targets with.

# One warning set for every target of the engine. Under MSVC, some warnings are off because they fire on the C API's
# design: a C function returning a record that has C++ constructors under __cplusplus (C4190), a record's anonymous
# union or struct (C4201), and a record that ends in a flexible array (C4200). Others are off because the other
# compilers' warnings below check the same code more precisely: conversions and comparisons between integer types
# (C4244, C4245, C4267, C4018, C4389), which MSVC reports where a closed set's enum constant converts to the set's
# integer type; shadowing (C4456, C4457, C4458); and the flow analysis that guesses at uninitialized variables and
# unreachable code (C4701, C4703, C4702). Negating an unsigned value (C4146), truncating a constant in a cast (C4310),
# adding constants of different enums (C5054), and an array rvalue decaying in C (C4223) are deliberate.
#
# A development build makes every warning an error, the linker's too. A build that includes Prax as a dependency keeps
# the warnings, because a newer compiler's new warnings must not break a consumer's build.
add_library(pr_warnings INTERFACE)
target_compile_options(pr_warnings INTERFACE
    $<$<C_COMPILER_ID:MSVC>:/W4 /wd4190 /wd4201 /wd4200 /wd4244 /wd4245 /wd4267 /wd4018 /wd4389 /wd4456 /wd4457
        /wd4458 /wd4701 /wd4703 /wd4702 /wd4146 /wd4310 /wd5054 /wd4223>
    $<$<AND:$<NOT:$<C_COMPILER_ID:MSVC>>,$<COMPILE_LANGUAGE:C,CXX,OBJCXX>>:-Wall -Wshadow -Wconversion
        -Wsign-conversion -Wimplicit-fallthrough -Wcast-align -Wcast-qual -Wundef -Wdouble-promotion
        -Wmissing-prototypes -Wunreachable-code -Wformat=2>
    $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-Wzero-as-null-pointer-constant>
    $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-Wnullability-completeness -Wcomma -Wextra-semi>
    $<$<COMPILE_LANG_AND_ID:C,Clang,AppleClang>:-Wnullability-completeness -Wcomma>
)
if (PR_DEVELOP)
    target_compile_options(pr_warnings INTERFACE $<$<C_COMPILER_ID:MSVC>:/WX>
        $<$<AND:$<NOT:$<C_COMPILER_ID:MSVC>>,$<COMPILE_LANGUAGE:C,CXX,OBJCXX>>:-Werror>)
    target_link_options(pr_warnings INTERFACE
        $<$<C_COMPILER_ID:MSVC>:/WX>
        $<$<AND:$<NOT:$<C_COMPILER_ID:MSVC>>,$<PLATFORM_ID:Darwin,iOS,tvOS,visionOS>>:LINKER:-fatal_warnings>
        $<$<AND:$<NOT:$<C_COMPILER_ID:MSVC>>,$<NOT:$<PLATFORM_ID:Darwin,iOS,tvOS,visionOS>>>:LINKER:--fatal-warnings>
    )
endif()

# Links the JavaScript library `file` (Emscripten's --js-library) into every program that links the library `target`,
# from the source tree in a build and from the package once installed.
#
#   pr_js_library(<target> <file>)
function(pr_js_library target file)
    get_filename_component(name ${file} NAME)
    set(source ${CMAKE_CURRENT_SOURCE_DIR}/${file})
    target_link_options(${target} INTERFACE
        "SHELL:--js-library $<BUILD_INTERFACE:${source}>$<INSTALL_INTERFACE:$<INSTALL_PREFIX>/${CMAKE_INSTALL_DATADIR}/prax/${name}>")
    set_property(TARGET ${target} APPEND PROPERTY INTERFACE_LINK_DEPENDS $<BUILD_INTERFACE:${source}>)
    if (PR_INSTALL)
        install(FILES ${source} DESTINATION ${CMAKE_INSTALL_DATADIR}/prax)
    endif()
endfunction()

# Declares PR_<MODULE>_<BACKEND>, the option that builds a backend into its library. It is ON by default where
# `default` is true, and in a development build or the examples' or the apps' wherever `supported` is true.
#
#   pr_backend_option(<option> <doc> <default> <supported>)
function(pr_backend_option option doc default supported)
    if (default OR ((PR_DEVELOP OR PR_EXAMPLES OR PR_APPS) AND supported))
        option(${option} "${doc}" ON)
    else()
        option(${option} "${doc}" OFF)
    endif()
endfunction()

# Adds a library of the engine, `name`: the target pr_<name>, which other targets and consumers name as prax::<name>.
# INCLUDE names the directory whose pr/ holds the library's public headers, which are installed with it and included as
# "pr/<header>". A library without sources is an INTERFACE library. Every library of the engine is installed and
# exported.
#
#   pr_library(<name> [INCLUDE <directory>] <source>...)
function(pr_library name)
    cmake_parse_arguments(PARSE_ARGV 1 LIBRARY "" "INCLUDE" "")
    set(target pr_${name})
    if (LIBRARY_UNPARSED_ARGUMENTS)
        add_library(${target} STATIC ${LIBRARY_UNPARSED_ARGUMENTS})
        target_link_libraries(${target} PRIVATE $<BUILD_LOCAL_INTERFACE:pr_warnings>)
        set(scope PUBLIC)
    else()
        add_library(${target} INTERFACE)
        set(scope INTERFACE)
    endif()
    add_library(prax::${name} ALIAS ${target})
    set_target_properties(${target} PROPERTIES EXPORT_NAME ${name})
    if (LIBRARY_INCLUDE)
        pr_headers(${target} ${scope} ${LIBRARY_INCLUDE})
    endif()
    set_property(GLOBAL APPEND PROPERTY PR_LIBRARIES ${target})
endfunction()

# Adds the headers under `directory`'s pr/ to the target's public headers, which it installs.
function(pr_headers target scope directory)
    cmake_path(ABSOLUTE_PATH directory NORMALIZE)
    file(GLOB headers CONFIGURE_DEPENDS ${directory}/pr/*)
    target_sources(${target} ${scope} FILE_SET HEADERS BASE_DIRS ${directory} FILES ${headers})
endfunction()

# Builds third-party targets with their warnings off. Their warnings are their authors' to act on, and a compiler that
# warns about more than theirs did would otherwise break the build.
set(PR_NO_WARNINGS $<IF:$<C_COMPILER_ID:MSVC>,/w,-w>)
function(pr_third_party)
    foreach (target IN LISTS ARGN)
        set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR OFF)
        target_compile_options(${target} PRIVATE ${PR_NO_WARNINGS})
    endforeach()
endfunction()

# Builds third-party sources that a target of the engine compiles beside its own.
function(pr_third_party_sources)
    set_source_files_properties(${ARGN} PROPERTIES COMPILE_OPTIONS ${PR_NO_WARNINGS})
endfunction()

# Builds every target of the directory `dir`, and of its subdirectories, as third-party code.
function(pr_third_party_directory dir)
    get_property(targets DIRECTORY ${dir} PROPERTY BUILDSYSTEM_TARGETS)
    foreach (target IN LISTS targets)
        get_target_property(type ${target} TYPE)
        if (NOT type STREQUAL "INTERFACE_LIBRARY" AND NOT type STREQUAL "UTILITY")
            pr_third_party(${target})
        endif()
    endforeach()
    get_property(subdirectories DIRECTORY ${dir} PROPERTY SUBDIRECTORIES)
    foreach (subdirectory IN LISTS subdirectories)
        pr_third_party_directory(${subdirectory})
    endforeach()
endfunction()

# Shaders: Slang, compiled at build time into a shader library for each backend that the platform builds (Metal on Apple
# platforms, Direct3D 12 on Windows, and Vulkan where the build has it), by shader_library.cmake. `source` compiles into
# a header that holds the library's bytes as `const unsigned char <VARIABLE>[]`, which a source includes by the path of
# the shader's directory under lib, or under the root outside lib: "<module>/.../<VARIABLE>.h". A shader includes
# pr/gpu.slang, and INCLUDE names other directories that it includes from, and DEPENDS the files that it includes that
# the build generates.
#
#   pr_slang(<target> <source> VARIABLE <name> [INCLUDE <directory>...] [DEPENDS <file>...])
function(pr_slang target source)
    cmake_parse_arguments(PARSE_ARGV 2 SLANG "" "VARIABLE" "INCLUDE;DEPENDS")
    cmake_path(ABSOLUTE_PATH source NORMALIZE)
    cmake_path(RELATIVE_PATH source BASE_DIRECTORY ${PROJECT_SOURCE_DIR} OUTPUT_VARIABLE relative)
    cmake_path(GET relative PARENT_PATH directory)
    string(REGEX REPLACE "^lib/" "" directory "${directory}")
    set(header ${PROJECT_BINARY_DIR}/slang/${directory}/${SLANG_VARIABLE}.h)
    list(JOIN PR_SHADER_TARGETS "|" targets)
    set(includes ${PROJECT_SOURCE_DIR}/lib/gpu/include ${SLANG_INCLUDE})
    list(JOIN includes "|" includes)
    add_custom_command(OUTPUT ${header}
        COMMAND ${CMAKE_COMMAND} -DSLANGC=${PR_SLANGC} -DTARGETS=${targets} -DVARIABLE=${SLANG_VARIABLE}
            -DMETAL_SDK=${PR_METAL_SDK} -DINCLUDES=${includes} -DSOURCE=${source} -DHEADER=${header}
            -DDEPFILE=${header}.d
            -DSCRATCH=${PROJECT_BINARY_DIR}/slang/${directory}/${SLANG_VARIABLE} -DROOT=${PROJECT_SOURCE_DIR}
            -P ${PROJECT_SOURCE_DIR}/cmake/shader_library.cmake
        DEPENDS ${source} ${PROJECT_SOURCE_DIR}/cmake/shader_library.cmake
            ${PROJECT_SOURCE_DIR}/lib/gpu/include/pr/gpu.slang ${SLANG_DEPENDS}
        DEPFILE ${header}.d
        COMMENT "Compiling ${relative}"
        VERBATIM)
    target_sources(${target} PRIVATE ${header})
    target_include_directories(${target} PRIVATE ${PROJECT_BINARY_DIR}/slang)
endfunction()

# Embeds files in a target's binary. FILES are paths under `directory`. Their contents go into a generated header, which
# a source of the target includes as "<path>/<VARIABLE>.h". The path is that of `directory` under lib, or under the root
# if `directory` is outside lib. The header defines two arrays of PrStringView in pr_impl, with one element per file in
# the order of FILES. <VARIABLE>Paths holds the paths under `directory`, and <VARIABLE>Bytes holds the contents. A
# change to one of the files reconfigures the build.
#
#   pr_embed(<target> <directory> VARIABLE <name> FILES <file>...)
function(pr_embed target directory)
    cmake_parse_arguments(PARSE_ARGV 2 EMBED "" "VARIABLE" "FILES")
    cmake_path(ABSOLUTE_PATH directory NORMALIZE)
    cmake_path(RELATIVE_PATH directory BASE_DIRECTORY ${PROJECT_SOURCE_DIR} OUTPUT_VARIABLE relative)
    string(REGEX REPLACE "^lib/" "" relative "${relative}")
    set(paths "")
    set(contents "")
    foreach (file IN LISTS EMBED_FILES)
        set(path ${directory}/${file})
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${path})
        file(READ ${path} hex HEX)
        file(SIZE ${path} size)
        # Each byte is a separate literal. Otherwise a hexadecimal escape would absorb the hex digits that follow it.
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "\\\\x\\1\"\"" bytes "${hex}")
        string(APPEND paths "    PrStringView{\"${file}\", sizeof(\"${file}\") - 1},\n")
        string(APPEND contents "    PrStringView{\"${bytes}\", ${size}},\n")
    endforeach()
    set(content "// Generated by pr_embed (cmake/build.cmake) from ${relative}. Do not edit.\n\n#pragma once\n\n")
    string(APPEND content "#include \"pr/string_view.h\"\n\nnamespace pr_impl {\n\n")
    string(APPEND content "constexpr PrStringView ${EMBED_VARIABLE}Paths[] = {\n${paths}};\n\n")
    string(APPEND content "constexpr PrStringView ${EMBED_VARIABLE}Bytes[] = {\n${contents}};\n\n} // namespace pr_impl\n")
    file(CONFIGURE OUTPUT ${PROJECT_BINARY_DIR}/embed/${relative}/${EMBED_VARIABLE}.h CONTENT "${content}" @ONLY)
    target_include_directories(${target} PRIVATE ${PROJECT_BINARY_DIR}/embed)
endfunction()

# Sets `out` to `text` as a JSON string.
function(pr_json_string out text)
    string(REPLACE "\\" "\\\\" text "${text}")
    string(REPLACE "\"" "\\\"" text "${text}")
    set(${out} "\"${text}\"" PARENT_SCOPE)
endfunction()

# Adds a test that ./dev.sh runs (tools/build/unit_tests.py), which the build lists in tests.json in its directory
# (pr_write_tests). COMMAND runs in WORKING_DIRECTORY, the current binary directory by default, with the ENVIRONMENT
# variables set, each as NAME=value. A command whose first word is a target runs the target's program, under the
# build's emulator if it has one. The test passes if the command exits 0 and, with PASS, prints a match of that regular
# expression. TIMEOUT, in seconds, ends a command that takes longer. A test of KIND doctest is a binary of doctest's
# cases, which takes doctest's options and times each case (tools/harness/test_main.cpp).
#
#   pr_add_test(<name> COMMAND <word>... [WORKING_DIRECTORY <directory>] [ENVIRONMENT <NAME=value>...]
#               [PASS <regex>] [TIMEOUT <seconds>] [KIND doctest])
function(pr_add_test name)
    cmake_parse_arguments(PARSE_ARGV 1 TEST "" "WORKING_DIRECTORY;PASS;TIMEOUT;KIND" "COMMAND;ENVIRONMENT")
    if (NOT TEST_WORKING_DIRECTORY)
        set(TEST_WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    endif()
    if (NOT TEST_KIND)
        set(TEST_KIND command)
    endif()
    if (NOT TEST_TIMEOUT)
        set(TEST_TIMEOUT 0)
    endif()
    # The test is written once every target exists (pr_write_tests), since its command may name a target that a later
    # directory adds.
    set_property(GLOBAL APPEND PROPERTY PR_TESTS ${name})
    foreach (field COMMAND ENVIRONMENT WORKING_DIRECTORY PASS TIMEOUT KIND)
        set_property(GLOBAL PROPERTY PR_TEST_${name}_${field} "${TEST_${field}}")
    endforeach()
endfunction()

# Writes the tests that pr_add_test added to tests.json in the build directory. It runs once every directory has added
# its targets and its tests.
function(pr_write_tests)
    get_property(names GLOBAL PROPERTY PR_TESTS)
    set(entries "")
    foreach (name IN LISTS names)
        foreach (field COMMAND ENVIRONMENT WORKING_DIRECTORY PASS TIMEOUT KIND)
            get_property(${field} GLOBAL PROPERTY PR_TEST_${name}_${field})
        endforeach()
        list(GET COMMAND 0 first)
        if (TARGET ${first})
            list(REMOVE_AT COMMAND 0)
            list(PREPEND COMMAND "$<TARGET_FILE:${first}>")
            if (CMAKE_CROSSCOMPILING_EMULATOR)
                list(PREPEND COMMAND ${CMAKE_CROSSCOMPILING_EMULATOR})
            endif()
        endif()
        set(words "")
        foreach (word IN LISTS COMMAND)
            pr_json_string(quoted "${word}")
            list(APPEND words "${quoted}")
        endforeach()
        list(JOIN words ", " words)
        set(variables "")
        foreach (variable IN LISTS ENVIRONMENT)
            string(FIND "${variable}" "=" equals)
            string(SUBSTRING "${variable}" 0 ${equals} key)
            math(EXPR start "${equals} + 1")
            string(SUBSTRING "${variable}" ${start} -1 value)
            pr_json_string(key "${key}")
            pr_json_string(value "${value}")
            list(APPEND variables "${key}: ${value}")
        endforeach()
        list(JOIN variables ", " variables)
        pr_json_string(quotedName "${name}")
        pr_json_string(directory "${WORKING_DIRECTORY}")
        pr_json_string(pass "${PASS}")
        string(CONCAT entry "  {\"name\": ${quotedName}, \"kind\": \"${KIND}\", \"command\": [${words}], "
            "\"directory\": ${directory}, \"environment\": {${variables}}, \"pass\": ${pass}, \"timeout\": ${TIMEOUT}}")
        list(APPEND entries "${entry}")
    endforeach()
    list(JOIN entries ",\n" entries)
    file(GENERATE OUTPUT ${PROJECT_BINARY_DIR}/tests.json CONTENT "[\n${entries}\n]\n")
endfunction()

# Adds the doctest binary `target` as a test (pr_add_test), if the host runs it or the build's emulator does, as the
# simulator's runner does for the build for a UIKit platform's simulator. A build for a UIKit platform's devices only
# builds it.
#
#   pr_add_tests(<target>)
function(pr_add_tests target)
    if (NOT PR_UIKIT OR CMAKE_CROSSCOMPILING_EMULATOR)
        pr_add_test(${target} KIND doctest COMMAND ${target})
    endif()
endfunction()

# One test binary per module, named after it, which links the module, the toolkit and the tests' main(). The module is
# a library of the engine, or a library of test support. It runs as pr_add_tests registers it.
#
#   pr_module_tests(<module> <source>...)
function(pr_module_tests module)
    add_executable(${module}_tests ${ARGN})
    if (TARGET prax::${module})
        set(tested prax::${module})
    else()
        set(tested ${module})
    endif()
    target_link_libraries(${module}_tests PRIVATE ${tested} prax::core_cpp test_main pr_warnings)
    pr_add_tests(${module}_tests)
endfunction()

# A module's tests that need a web page, such as those that draw through a page's canvases: on the web, a page,
# <module>_page_tests.html, that `./dev.sh test wasm` runs in headless Chrome. Each PRELOAD packages a file into the
# page as <host path>@<page path>, since a page reads no files of the host. Elsewhere, nothing is built.
#
#   pr_page_tests(<module> <source>... [PRELOAD <host path>@<page path>...])
function(pr_page_tests module)
    if (NOT EMSCRIPTEN)
        return()
    endif()
    cmake_parse_arguments(PARSE_ARGV 1 PAGE "" "" "PRELOAD")
    set(name ${module}_page_tests)
    add_executable(${name} ${PAGE_UNPARSED_ARGUMENTS})
    if (TARGET prax::${module})
        set(tested prax::${module})
    else()
        set(tested ${module})
    endif()
    target_link_libraries(${name} PRIVATE ${tested} prax::core_cpp test_main pr_warnings)
    # The page has no file system of the host's, which the tests under Node.js read and write.
    get_target_property(options ${name} LINK_OPTIONS)
    if (options)
        list(REMOVE_ITEM options -sNODERAWFS=1)
        set_target_properties(${name} PROPERTIES LINK_OPTIONS "${options}")
    endif()
    set(shell ${PROJECT_SOURCE_DIR}/tools/harness/page_tests.html)
    set_target_properties(${name} PROPERTIES SUFFIX ".html")
    target_link_options(${name} PRIVATE "--shell-file=${shell}" -sEXIT_RUNTIME=1)
    set_property(TARGET ${name} APPEND PROPERTY LINK_DEPENDS ${shell})
    foreach (file IN LISTS PAGE_PRELOAD)
        target_link_options(${name} PRIVATE "SHELL:--preload-file \"${file}\"")
    endforeach()
endfunction()

# A program that a tools/ runner drives, built on the module it exercises and the harnesses' support.
#
#   pr_harness(<name> <module> <source>...)
function(pr_harness name module)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE ${module} harness pr_warnings)
endfunction()

# Makes `target`, an example or an app, an application bundle for the UIKit platform that the build targets,
# org.prax.<target>, which the simulator installs and launches (tools/build/simulator.py). Its Info.plist is
# cmake/uikit_info.plist.in, with the platform's own keys. With MULTIPLE_SCENES, the application may show several scenes
# where the device does, as an iPad does, which lib/app's Multiple window model needs. The bundle is signed whole, as
# the simulator requires of what it installs.
#
#   pr_uikit_bundle(<target> [MULTIPLE_SCENES])
function(pr_uikit_bundle target)
    cmake_parse_arguments(PARSE_ARGV 1 arg "MULTIPLE_SCENES" "" "")
    set(PR_UIKIT_MULTIPLE_SCENES false)
    if (arg_MULTIPLE_SCENES)
        set(PR_UIKIT_MULTIPLE_SCENES true)
    endif()
    set(PR_UIKIT_SCENE_KEYS "")
    set(PR_UIKIT_PLATFORM_KEYS "")
    if (PR_IOS)
        set(PR_UIKIT_DEVICE_FAMILIES "        <integer>1</integer>\n        <integer>2</integer>")
        string(CONCAT PR_UIKIT_PLATFORM_KEYS
            "    <key>LSRequiresIPhoneOS</key>\n    <true/>\n"
            "    <key>UILaunchScreen</key>\n    <dict/>\n"
            "    <key>UISupportedInterfaceOrientations</key>\n    <array>\n"
            "        <string>UIInterfaceOrientationPortrait</string>\n"
            "        <string>UIInterfaceOrientationPortraitUpsideDown</string>\n"
            "        <string>UIInterfaceOrientationLandscapeLeft</string>\n"
            "        <string>UIInterfaceOrientationLandscapeRight</string>\n    </array>\n"
            "    <key>CADisableMinimumFrameDurationOnPhone</key>\n    <true/>")
    elseif (PR_TVOS)
        set(PR_UIKIT_DEVICE_FAMILIES "        <integer>3</integer>")
    else()
        # visionOS opens the application in a window, rather than in a space of its own.
        set(PR_UIKIT_DEVICE_FAMILIES "        <integer>7</integer>")
        string(CONCAT PR_UIKIT_SCENE_KEYS
            "        <key>UIApplicationPreferredDefaultSceneSessionRole</key>\n"
            "        <string>UIWindowSceneSessionRoleApplication</string>")
    endif()
    # The bundle's properties fill in the rest of the template when CMake writes the bundle.
    set(plist ${CMAKE_CURRENT_BINARY_DIR}/${target}_info.plist.in)
    configure_file(${PROJECT_SOURCE_DIR}/cmake/uikit_info.plist.in ${plist} @ONLY)
    set_target_properties(${target} PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_GUI_IDENTIFIER org.prax.${target}
        MACOSX_BUNDLE_BUNDLE_NAME ${target}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_INFO_PLIST ${plist})
    add_custom_command(TARGET ${target} POST_BUILD COMMAND codesign --force --sign - $<TARGET_BUNDLE_DIR:${target}>
        VERBATIM)
endfunction()
