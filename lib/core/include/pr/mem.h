// Memory: operations on raw bytes, and reads and writes of integers in them.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The C library's functions, through Clang's and GCC's builtins, or under MSVC, which has none of them, through the
// library's own functions, which it inlines as intrinsics.
#if defined(_MSC_VER) && !defined(__clang__)
void* __cdecl memcpy(void* destination, const void* source, size_t size);
void* __cdecl memmove(void* destination, const void* source, size_t size);
void* __cdecl memset(void* destination, int value, size_t size);
int __cdecl memcmp(const void* left, const void* right, size_t size);
#if defined(__cplusplus)
const void* __cdecl memchr(const void* bytes, int value, size_t size);
#else
void* __cdecl memchr(const void* bytes, int value, size_t size);
#endif
#define PR_MEM_BUILTIN(name) name
#else
#define PR_MEM_BUILTIN(name) __builtin_##name
#endif

// Copies `size` bytes from `source` to `destination`. The ranges must not overlap.
static inline void prMemCopy(void* destination, const void* source, size_t size) {
    PR_MEM_BUILTIN(memcpy)(destination, source, size);
}

// Copies `size` bytes from `source` to `destination`. The ranges may overlap.
static inline void prMemMove(void* destination, const void* source, size_t size) {
    PR_MEM_BUILTIN(memmove)(destination, source, size);
}

// Zeroes the `size` bytes at `destination`.
static inline void prMemZero(void* destination, size_t size) {
    PR_MEM_BUILTIN(memset)(destination, 0, size);
}

// Sets the `size` bytes at `destination` to `value`.
static inline void prMemFill(void* destination, uint8_t value, size_t size) {
    PR_MEM_BUILTIN(memset)(destination, value, size);
}

// Returns the first of the `size` bytes at `bytes` that equals `value`, or null if none does.
static inline const char* PR_NULLABLE prMemFind(const char* bytes, char value, size_t size) {
    return PR_VALUE_CAST(const char*, PR_MEM_BUILTIN(memchr)(bytes, PR_VALUE_CAST(uint8_t, value), size));
}

// Compares the `size` bytes at `left` and `right`. Returns a negative value, zero or a positive value if `left` sorts
// before, with or after `right`.
static inline int prMemCompare(const void* left, const void* right, size_t size) {
    return PR_MEM_BUILTIN(memcmp)(left, right, size);
}

// Returns true if the `size` bytes at `left` and `right` are equal.
static inline bool prMemEquals(const void* left, const void* right, size_t size) {
    return PR_MEM_BUILTIN(memcmp)(left, right, size) == 0;
}

// Reads an unsigned integer at `bytes` in the byte order of the machine. `bytes` need not be aligned.
static inline uint32_t prMemReadU32(const void* bytes) {
    uint32_t value;
    prMemCopy(&value, bytes, sizeof(value));
    return value;
}
static inline uint64_t prMemReadU64(const void* bytes) {
    uint64_t value;
    prMemCopy(&value, bytes, sizeof(value));
    return value;
}

// Reads or writes an unsigned integer at `bytes` in big-endian (Be) or little-endian (Le) byte order, regardless of the
// byte order of the machine.
PR_CONSTEXPR uint16_t prMemReadBeU16(const uint8_t* bytes) {
    return PR_VALUE_CAST(uint16_t, (bytes[0] << 8) | bytes[1]);
}
PR_CONSTEXPR uint32_t prMemReadBeU32(const uint8_t* bytes) {
    return (PR_VALUE_CAST(uint32_t, bytes[0]) << 24) | (PR_VALUE_CAST(uint32_t, bytes[1]) << 16) |
        (PR_VALUE_CAST(uint32_t, bytes[2]) << 8) | bytes[3];
}
PR_CONSTEXPR uint16_t prMemReadLeU16(const uint8_t* bytes) {
    return PR_VALUE_CAST(uint16_t, bytes[0] | (bytes[1] << 8));
}
PR_CONSTEXPR uint32_t prMemReadLeU32(const uint8_t* bytes) {
    return bytes[0] | (PR_VALUE_CAST(uint32_t, bytes[1]) << 8) | (PR_VALUE_CAST(uint32_t, bytes[2]) << 16) |
        (PR_VALUE_CAST(uint32_t, bytes[3]) << 24);
}
PR_CONSTEXPR uint64_t prMemReadLeU64(const uint8_t* bytes) {
    return PR_VALUE_CAST(uint64_t, prMemReadLeU32(bytes)) | (PR_VALUE_CAST(uint64_t, prMemReadLeU32(bytes + 4)) << 32);
}
PR_CONSTEXPR void prMemWriteBeU32(uint8_t* bytes, uint32_t value) {
    bytes[0] = PR_VALUE_CAST(uint8_t, value >> 24);
    bytes[1] = PR_VALUE_CAST(uint8_t, value >> 16);
    bytes[2] = PR_VALUE_CAST(uint8_t, value >> 8);
    bytes[3] = PR_VALUE_CAST(uint8_t, value);
}

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
