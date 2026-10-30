// Memory: operations on raw bytes.

#pragma once

#include "pr/base.h"

// MSVC has none of the builtins below except __builtin_memcmp, and inlines the C library's functions as intrinsics.
#if defined(_MSC_VER) && !defined(__clang__)
extern "C" void* __cdecl memcpy(void* destination, const void* source, size_t size);
extern "C" void* __cdecl memmove(void* destination, const void* source, size_t size);
extern "C" void* __cdecl memset(void* destination, int value, size_t size);
extern "C" const void* __cdecl memchr(const void* bytes, int value, size_t size);
#define PR_MEM_BUILTIN(name) ::name
#else
#define PR_MEM_BUILTIN(name) __builtin_##name
#endif

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Copies `size` bytes from `source` to `destination`. The ranges must not overlap.
inline void memCopy(void* destination, const void* source, size_t size) {
    PR_MEM_BUILTIN(memcpy)(destination, source, size);
}

// Copies `size` bytes from `source` to `destination`. The ranges may overlap.
inline void memMove(void* destination, const void* source, size_t size) {
    PR_MEM_BUILTIN(memmove)(destination, source, size);
}

// Zeroes the `size` bytes at `destination`.
inline void memZero(void* destination, size_t size) {
    PR_MEM_BUILTIN(memset)(destination, 0, size);
}

// Sets the `size` bytes at `destination` to `value`.
inline void memFill(void* destination, uint8_t value, size_t size) {
    PR_MEM_BUILTIN(memset)(destination, value, size);
}

// Returns the first of the `size` bytes at `bytes` that equals `value`, or null if none does.
inline const char* PR_NULLABLE memFind(const char* bytes, char value, size_t size) {
    return static_cast<const char*>(PR_MEM_BUILTIN(memchr)(bytes, uint8_t(value), size));
}

// Compares the `size` bytes at `left` and `right`. Returns a negative value, zero or a positive value if `left` sorts
// before, with or after `right`.
inline int memCompare(const void* left, const void* right, size_t size) {
    return __builtin_memcmp(left, right, size);
}

// Returns true if the `size` bytes at `left` and `right` are equal.
inline bool memEquals(const void* left, const void* right, size_t size) {
    return __builtin_memcmp(left, right, size) == 0;
}

// Reads or writes an unsigned integer at `bytes` in big-endian (Be) or little-endian (Le) byte order, regardless of the
// byte order of the machine.
inline uint16_t readBeU16(const uint8_t* bytes) {
    return uint16_t((bytes[0] << 8) | bytes[1]);
}
inline uint32_t readBeU32(const uint8_t* bytes) {
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) | bytes[3];
}
inline uint16_t readLeU16(const uint8_t* bytes) {
    return uint16_t(bytes[0] | (bytes[1] << 8));
}
inline uint32_t readLeU32(const uint8_t* bytes) {
    return bytes[0] | (uint32_t(bytes[1]) << 8) | (uint32_t(bytes[2]) << 16) | (uint32_t(bytes[3]) << 24);
}
inline uint64_t readLeU64(const uint8_t* bytes) {
    return uint64_t(readLeU32(bytes)) | (uint64_t(readLeU32(bytes + 4)) << 32);
}
inline void writeBeU32(uint8_t* bytes, uint32_t value) {
    bytes[0] = uint8_t(value >> 24);
    bytes[1] = uint8_t(value >> 16);
    bytes[2] = uint8_t(value >> 8);
    bytes[3] = uint8_t(value);
}

} // namespace pr

PR_ASSUME_NONNULL_END
