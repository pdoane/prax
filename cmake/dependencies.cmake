# What the build fetches: the libraries that Prax builds on, the programs that its build runs, and what the development
# build compares the engine with. Each is an archive at a pinned release or commit, checked against its hash. A build
# that has its own copy of one points FETCHCONTENT_SOURCE_DIR_<NAME> at it, as FetchContent provides.
#
# A dependency is declared here and made available where a target needs it, so a build fetches only what its options
# build. Moving a pin means changing its URL and its hash together.

include(${CMAKE_CURRENT_LIST_DIR}/pr_dependency.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/dependencies_tree_sitter.cmake)

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
    URL https://github.com/quickjs-ng/quickjs/archive/refs/tags/v0.18.0.tar.gz
    URL_HASH SHA256=28bcd89aa3683fd2a946537e5ce3f11cc46b5cbf3383c7561aaef3df5ff9f1dd
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

# mimalloc, the allocator that core's mimalloc backend is built on, with the changes in patches/mimalloc. On macOS, the
# patch tags its memory with the first application-specific tag. vmmap and footprint then report it as the
# application's heap, not as GPU memory (VM_MEMORY_IOACCELERATOR, which mimalloc uses by default).
pr_patch_arguments(PR_MIMALLOC_PATCH mimalloc ${PROJECT_SOURCE_DIR}/patches/mimalloc)
pr_dependency(mimalloc
    URL https://github.com/microsoft/mimalloc/archive/refs/tags/v3.5.3.tar.gz
    URL_HASH SHA256=3b4a15153a59905995f7070296ed604bb5ccc00cabb8b93446931aff77224d47
    ${PR_MIMALLOC_PATCH}
    EXCLUDE_FROM_ALL
    SYSTEM
)

include(${CMAKE_CURRENT_LIST_DIR}/slang.cmake)

# The development build's dependencies: the test framework, and the implementations that the engine is compared with.

# doctest, with the changes that patches/doctest makes: its header declares the streams it names through <iosfwd> with
# libc++, rather than including <ostream> and <istream>, which are most of the standard library and so most of a test's
# compile.
pr_patch_arguments(PR_DOCTEST_PATCH doctest ${PROJECT_SOURCE_DIR}/patches/doctest)
pr_dependency(doctest
    URL https://github.com/doctest/doctest/archive/refs/tags/v2.4.12.tar.gz
    URL_HASH SHA256=73381c7aa4dee704bd935609668cf41880ea7f19fa0504a200e13b74999c2d70
    ${PR_DOCTEST_PATCH}
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

# libvterm 0.3.3, the terminal emulator that the vterm benchmark measures lib/vterm against. The release archive
# includes the tables that its Makefile generates, so the sources compile without running the Makefile.
pr_dependency(libvterm
    URL https://www.leonerd.org.uk/code/libvterm/libvterm-0.3.3.tar.gz
    URL_HASH SHA256=09156f43dd2128bd347cbeebe50d9a571d32c64e0cf18d211197946aff7226e0
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

# Zstandard v1.5.7, the reference implementation, whose encoder writes the frames that tools/suite/zstd decodes with
# lib/zstd and whose decoder it compares that with.
pr_dependency(zstd
    URL https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
    URL_HASH SHA256=eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
    SOURCE_SUBDIR none
)

# zlib 1.3.1 and Brotli 1.2.0, the reference implementations of Deflate and Brotli, which the compression benchmark
# (tools/bench) compares lib/compress's encoders and decoders with.
pr_dependency(zlib
    URL https://github.com/madler/zlib/releases/download/v1.3.1/zlib-1.3.1.tar.gz
    URL_HASH SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
    SOURCE_SUBDIR none
)
pr_dependency(brotli
    URL https://github.com/google/brotli/archive/refs/tags/v1.2.0.tar.gz
    URL_HASH SHA256=816c96e8e8f193b40151dad7e8ff37b1221d019dbcb9c35cd3fadbfe6477dfec
    DOWNLOAD_NAME brotli-1.2.0.tar.gz
    SOURCE_SUBDIR none
)

# Basis Universal 1.50.0.2, the reference transcoder and encoder for the ETC1S and UASTC textures of KTX 2.0 files.
# tools/suite/basis compares the decoders of lib/basis with it.
pr_dependency(basis_universal
    URL https://github.com/BinomialLLC/basis_universal/archive/refs/tags/v1_50_0_2.tar.gz
    URL_HASH SHA256=0ef344cc7e3373ca9c15de2bd80512ea4ea17e09ed895febdf9e70f6c789bc27
    SOURCE_SUBDIR none
)

# libwebp v1.6.0, the reference WebP codec: its decoders, which tools/suite/webp compares lib/webp's decoder with,
# and its encoders, which write the images of each feature that the comparison decodes.
pr_dependency(libwebp
    URL https://github.com/webmproject/libwebp/archive/refs/tags/v1.6.0.tar.gz
    URL_HASH SHA256=93a852c2b3efafee3723efd4636de855b46f9fe1efddd607e1f42f60fc8f2136
    SOURCE_SUBDIR none
)

# OpenEXR v3.3.15, the reference implementation of OpenEXR: its core library's decoders, which tools/suite/exr compares
# lib/exr's decoder with, and its encoders, which write the files of each compression that the comparison
# decodes. libdeflate v1.18 is the Deflate library that OpenEXR's build compiles into the core library at that version.
# The change in patches/openexr makes DWA decoding use the scalar inverse DCT on every processor. OpenEXR otherwise
# chooses an SSE2 or AVX version on x86, whose results differ from the scalar version's in the last bits.
pr_patch_arguments(PR_OPENEXR_PATCH openexr ${PROJECT_SOURCE_DIR}/patches/openexr)
pr_dependency(openexr
    URL https://github.com/AcademySoftwareFoundation/openexr/archive/refs/tags/v3.3.15.tar.gz
    URL_HASH SHA256=21bd9e029185f4da7f6a662be18a13ccaf698d5c9bd8964c40e0a4227cbc7e1b
    DOWNLOAD_NAME openexr-3.3.15.tar.gz
    ${PR_OPENEXR_PATCH}
    SOURCE_SUBDIR none
)
pr_dependency(libdeflate
    URL https://github.com/ebiggers/libdeflate/archive/refs/tags/v1.18.tar.gz
    URL_HASH SHA256=225d982bcaf553221c76726358d2ea139bb34913180b20823c782cede060affd
    DOWNLOAD_NAME libdeflate-1.18.tar.gz
    SOURCE_SUBDIR none
)
