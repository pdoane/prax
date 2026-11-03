# What the build fetches: the libraries that Prax builds on, the programs that its build runs, and what the development
# build compares the engine with. Each is an archive at a pinned release or commit, checked against its hash. A build
# that has its own copy of one points FETCHCONTENT_SOURCE_DIR_<NAME> at it, as FetchContent provides.
#
# A dependency is declared here and made available where a target needs it, so a build fetches only what its options
# build. Moving a pin means changing its URL and its hash together.

include(${CMAKE_CURRENT_LIST_DIR}/pr_dependency.cmake)

# The FetchContent arguments that apply the patches in `directory` to the fetched source tree of the dependency `name`.
# A tree holds one set of patches, and git apply cannot apply a set to a tree that holds another. So the tree's directory
# is named for the patches' hash, and any change to the patches, in either direction, extracts a new tree. Adding or
# editing a patch configures the build again. The command runs Git outside the repository that holds the build directory,
# which `git apply` would otherwise patch instead.
function(pr_patch_arguments variable name directory)
    file(GLOB patches CONFIGURE_DEPENDS ${directory}/*.patch)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${patches})
    set(contents "")
    foreach (patch IN LISTS patches)
        file(READ ${patch} text)
        string(APPEND contents "${text}")
    endforeach()
    string(SHA256 hash "${contents}")
    string(SUBSTRING ${hash} 0 12 hash)
    # The trees of other patches are out of date, and extracting one again is cheap.
    file(GLOB trees ${FETCHCONTENT_BASE_DIR}/${name}-src ${FETCHCONTENT_BASE_DIR}/${name}-src-*)
    list(REMOVE_ITEM trees ${FETCHCONTENT_BASE_DIR}/${name}-src-${hash})
    if (trees)
        file(REMOVE_RECURSE ${trees})
    endif()
    find_package(Git REQUIRED)
    set(${variable} SOURCE_DIR ${FETCHCONTENT_BASE_DIR}/${name}-src-${hash}
        PATCH_COMMAND ${CMAKE_COMMAND} -E env GIT_CEILING_DIRECTORIES=${FETCHCONTENT_BASE_DIR}
        ${GIT_EXECUTABLE} apply ${patches} PARENT_SCOPE)
endfunction()

# A dependency whose sources a target of the engine compiles, or whose headers it reads, is fetched without its own
# build: SOURCE_SUBDIR names a directory that has no CMakeLists.txt.

# QuickJS, with the changes that patches/quickjs makes, for whatever is built to run JavaScript.
pr_patch_arguments(PR_QUICKJS_PATCH quickjs ${PROJECT_SOURCE_DIR}/patches/quickjs)
pr_dependency(quickjs
    URL https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.9.0.tar.gz
    URL_HASH SHA256=77f9e79b42e2e7cff9517bae612431af47e120730286cb1dcfad0753bc160f10
    ${PR_QUICKJS_PATCH}
    SOURCE_SUBDIR none
)

# HarfBuzz, which lib/text/backends/harfbuzz is built on, built as its single-source amalgamation.
pr_dependency(harfbuzz
    URL https://github.com/harfbuzz/harfbuzz/releases/download/14.5.0/harfbuzz-14.5.0.tar.xz
    URL_HASH SHA256=b7132e148358a45185c9feafd049dbaf243649d3c44414b3534d9c95d18592b9
    SOURCE_SUBDIR none
)

# kb, the header-only shaper that lib/text/backends/kb is built on.
pr_dependency(kb
    URL https://github.com/JimmyLefevre/kb/archive/ed1eed87307c4c5915d3b7918dc3750e2683f36d.tar.gz
    URL_HASH SHA256=b37bddee04757f0cdb3abfa650829c51ed6461626c05683e6538437b36e908e9
    SOURCE_SUBDIR none
)

# tree-sitter, which lib/syntax's tree-sitter languages are built on, built from its one-source library, and the
# grammars of the languages that the backend builds in.
pr_dependency(tree_sitter
    URL https://github.com/tree-sitter/tree-sitter/archive/refs/tags/v0.27.1.tar.gz
    URL_HASH SHA256=982cd3d4d9eb7be18c243240a622423fd2e4ecb4bec2cd98832c2f3fa1f0f333
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_c
    URL https://github.com/tree-sitter/tree-sitter-c/archive/refs/tags/v0.24.2.tar.gz
    URL_HASH SHA256=2eeb4db31f8fa0865e45488503d13403923bcb485a1bdb637abff8c42dd97364
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_cpp
    URL https://github.com/tree-sitter/tree-sitter-cpp/archive/refs/tags/v0.23.4.tar.gz
    URL_HASH SHA256=7a2c55afe3028f4105f25762ea58cc16537d1f5a1dcd9cca90410b3cd5d46051
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_css
    URL https://github.com/tree-sitter/tree-sitter-css/archive/refs/tags/v0.25.0.tar.gz
    URL_HASH SHA256=03965344d8c0435dc54fb45b281578420bb7db8b99df4d34e7e74105a274cb79
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_html
    URL https://github.com/tree-sitter/tree-sitter-html/archive/refs/tags/v0.23.2.tar.gz
    URL_HASH SHA256=21fa4f2d4dcb890ef12d09f4979a0007814f67f1c7294a9b17b0108a09e45ef7
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_javascript
    URL https://github.com/tree-sitter/tree-sitter-javascript/archive/refs/tags/v0.25.0.tar.gz
    URL_HASH SHA256=9712fc283d3dc01d996d20b6392143445d05867a7aad76fdd723824468428b86
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_markdown
    URL https://github.com/tree-sitter-grammars/tree-sitter-markdown/archive/refs/tags/v0.5.4.tar.gz
    URL_HASH SHA256=481ce2c1cce7b1130bc8455c186052bd0fa461f1ff488666994c6a3da7fca2ac
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_python
    URL https://github.com/tree-sitter/tree-sitter-python/archive/refs/tags/v0.25.0.tar.gz
    URL_HASH SHA256=4609a3665a620e117acf795ff01b9e965880f81745f287a16336f4ca86cf270c
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_xml
    URL https://github.com/tree-sitter-grammars/tree-sitter-xml/archive/refs/tags/v0.7.0.tar.gz
    URL_HASH SHA256=4330a6b3685c2f66d108e1df0448eb40c468518c3a66f2c1607a924c262a3eb9
    SOURCE_SUBDIR none
)
pr_dependency(tree_sitter_json
    URL https://github.com/tree-sitter/tree-sitter-json/archive/refs/tags/v0.24.8.tar.gz
    URL_HASH SHA256=acf6e8362457e819ed8b613f2ad9a0e1b621a77556c296f3abea58f7880a9213
    SOURCE_SUBDIR none
)

# The Vulkan API's headers, which lib/gpu's Vulkan backend is built against. The backend loads the system's Vulkan
# loader when it creates a device, so nothing links to an SDK.
pr_dependency(vulkan_headers
    URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/v1.4.366.tar.gz
    URL_HASH SHA256=577b90c208e6fd39ec9a1460e48772f63c76f6f5afb441df2a977a0fdb3f618c
    SOURCE_SUBDIR none
)

# FreeType, which the FreeType hinter is built on, without any of the optional libraries that it can use.
pr_dependency(freetype
    URL https://github.com/freetype/freetype/archive/refs/tags/VER-2-14-3.tar.gz
    URL_HASH SHA256=dc49de6b01a266eef4876a4dd34d9842c475d3e28ff2eff63bd2fb760ab56261
    EXCLUDE_FROM_ALL
    SYSTEM
)

# mimalloc, the allocator that core's mimalloc backend is built on.
pr_dependency(mimalloc
    URL https://github.com/microsoft/mimalloc/archive/refs/tags/v3.5.3.tar.gz
    URL_HASH SHA256=3b4a15153a59905995f7070296ed604bb5ccc00cabb8b93446931aff77224d47
    EXCLUDE_FROM_ALL
    SYSTEM
)

# Slang's compiler, which compiles the shaders at build time: the release's binaries for the host. PR_SLANGC names a
# slangc to use instead.
set(PR_SLANGC "" CACHE FILEPATH "The Slang compiler that builds the shaders, which the build fetches if this is empty")
if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(PR_SLANG_ASSET macos-aarch64.zip)
    set(PR_SLANG_HASH ede1865bf086442c8786c5b7c0470ce5f4fd4f032eefe881356726ff9c445f40)
elseif (CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "ARM64|arm64")
    set(PR_SLANG_ASSET windows-aarch64.zip)
    set(PR_SLANG_HASH 4f18a7e76a59969bc9bf6ea596bd9e496a3c689af173990ae028b18f03e1ac1a)
elseif (CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(PR_SLANG_ASSET windows-x86_64.zip)
    set(PR_SLANG_HASH e6dd32bbddf1720ebb4e679d55e9635ce792865a5bd84f3d2cd9af28647c0174)
else()
    set(PR_SLANG_ASSET linux-x86_64.tar.gz)
    set(PR_SLANG_HASH e863e568c204810d6e8bc31852f056aef6763c55ea9925db2db7affa060347c9)
endif()
pr_dependency(slang
    URL https://github.com/shader-slang/slang/releases/download/v2026.20/slang-2026.20-${PR_SLANG_ASSET}
    URL_HASH SHA256=${PR_SLANG_HASH}
    SOURCE_SUBDIR none
)

# The development build's dependencies: the test framework, and the implementations that the engine is compared with.

pr_dependency(doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.12.tar.gz
    URL_HASH SHA256=73381c7aa4dee704bd935609668cf41880ea7f19fa0504a200e13b74999c2d70
    EXCLUDE_FROM_ALL
    SYSTEM
)

# Dear ImGui, which lib/draw/comparisons/imgui records into, with the changes that patches/imgui makes. Its commit is
# on features/drawlist_193, where ImDrawList gains what it is growing next.
pr_patch_arguments(PR_IMGUI_PATCH imgui ${PROJECT_SOURCE_DIR}/patches/imgui)
pr_dependency(imgui
    URL https://github.com/ocornut/imgui/archive/241bf3bb05ebb918063c148da2c44062b5508ed4.tar.gz
    URL_HASH SHA256=3fe2e934b341a124fb72b21b20e8465dc07c6392ac15683662664eb7b5ab857b
    ${PR_IMGUI_PATCH}
    SOURCE_SUBDIR none
)

# Blend2D, which lib/draw/comparisons/blend2d renders with.
pr_dependency(blend2d
    URL https://github.com/blend2d/blend2d/archive/58ca9460b4138af6e793184682e9324e0a8053b2.tar.gz
    URL_HASH SHA256=b7887f581dc1340ce4c7198a4ccc96c79fd832fe099367db065c3b9ff5dfffd8
    EXCLUDE_FROM_ALL
    SYSTEM
)

# rlottie, another Lottie player, which the viewer compares lib/lottie with, at the commit of the rlottie suite's corpus.
pr_dependency(rlottie
    URL https://github.com/Samsung/rlottie/archive/683bbaa39dd0d366cf6b4bc300b4dfbee677ea6b.tar.gz
    URL_HASH SHA256=1a1919ad7dd09c59f133469b35020efbda1d408a6ea1e3869f00bec553f92555
    EXCLUDE_FROM_ALL
    SYSTEM
)

# cmark-gfm 0.29.0.gfm.13, which GitHub renders Markdown with, and which tools/suite/markdown/markdown_compare.py
# compares lib/markdown with.
pr_dependency(cmark_gfm
    URL https://github.com/github/cmark-gfm/archive/587a12bb54d95ac37241377e6ddc93ea0e45439b.tar.gz
    URL_HASH SHA256=9214b42f13a471fc2984e9806c37bf0f42a13027854ab19103bd6d8085eb3416
    SOURCE_SUBDIR none
)

# Yoga v3.2.1 and Taffy v0.14.0, the flex layouts that the flex benchmark measures the engine against.
pr_dependency(yoga
    URL https://github.com/facebook/yoga/archive/042f5013152eb81c1552dec945b88f7b95ca350f.tar.gz
    URL_HASH SHA256=4742f41722a16f181e3da37abf943390db1e928f00f26402cb154662ae7f110f
    SOURCE_SUBDIR none
)
pr_dependency(taffy
    URL https://github.com/DioxusLabs/taffy/archive/77f385683c1d698c91a23a259f87fdddf26925fb.tar.gz
    URL_HASH SHA256=0e61a69c6b4f1722df706f9985bc5ec6c0813ba9f43630451cb5cffaef5382e0
    SOURCE_SUBDIR none
)
