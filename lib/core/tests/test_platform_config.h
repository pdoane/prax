// A build configuration that binds memory and failure at compile time in place of their defaults, for the tests of a
// configured build. It binds neither zeroed allocation nor usable sizes, which the library then does without.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The configured facilities, which test_platform_config.cpp defines.
void* PR_NULLABLE configAllocBytes(size_t size);
void* PR_NULLABLE configReallocBytes(void* memory, size_t size);
void configFreeBytes(void* memory);
PR_NORETURN void configFailed(PrStringView message, PrStringView file, uint32_t line);

} // namespace pr_impl

PR_ASSUME_NONNULL_END

// The bindings, each to the function above of the same name.
#define PR_PLATFORM_ALLOC_BYTES(size) ::pr_impl::configAllocBytes(size)
#define PR_PLATFORM_REALLOC_BYTES(memory, size) ::pr_impl::configReallocBytes(memory, size)
#define PR_PLATFORM_FREE_BYTES(memory) ::pr_impl::configFreeBytes(memory)
#define PR_PLATFORM_FAIL(message, file, line) ::pr_impl::configFailed(message, file, line)
#define PR_PLATFORM_CRASH_HANDLER() ((void)0)
