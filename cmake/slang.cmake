# Slang's compiler, which compiles the shaders at build time: the release's binaries for the host. The build includes
# this file, and so does the installed package, whose consumers compile shader sets of their own
# (render_shaders.cmake). PR_SLANGC names a slangc to use instead of the release's.

set(PR_SLANGC "" CACHE FILEPATH "The Slang compiler that builds the shaders, which the build fetches if this is empty")
if (CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
    set(_pr_slang_asset macos-aarch64.zip)
    set(_pr_slang_hash f6d6fe9cace17ee74836805786163d63ddf29ff1ff6c516b7eae59855af61cc3)
elseif (CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "ARM64|arm64")
    set(_pr_slang_asset windows-aarch64.zip)
    set(_pr_slang_hash 3b74b69e1924d359d3626949b3cde0f6e65068a7b5eb2457e83a351697fe4e71)
elseif (CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
    set(_pr_slang_asset windows-x86_64.zip)
    set(_pr_slang_hash dd0c4a86bd44cdb7e9cf712af0c8ebf2fff1bba19fa5e3c128ff87384e8ff17b)
else()
    set(_pr_slang_asset linux-x86_64.tar.gz)
    set(_pr_slang_hash a877b75c6ec515f5d8bf66ea4bae4f0a5ba85050b9540c2046670285808347e6)
endif()
pr_dependency(slang
    URL https://github.com/shader-slang/slang/releases/download/v2026.22/slang-2026.22-${_pr_slang_asset}
    URL_HASH SHA256=${_pr_slang_hash}
    SOURCE_SUBDIR none
)

# Sets `out` to the path of slangc: PR_SLANGC, or the release's, which the first call fetches.
function(pr_slangc out)
    if (PR_SLANGC)
        set(${out} ${PR_SLANGC} PARENT_SCOPE)
        return()
    endif()
    pr_dependency_available(slang)
    set(slangc ${slang_SOURCE_DIR}/bin/slangc)
    if (CMAKE_HOST_WIN32)
        string(APPEND slangc .exe)
    endif()
    set(${out} ${slangc} PARENT_SCOPE)
endfunction()
