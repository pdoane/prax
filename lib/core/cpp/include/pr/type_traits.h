// Type traits: what the compiler knows about a type, for templates that choose an implementation by it.
//
// The only header that names the compiler's type intrinsics (lint.py, `builtin`), and the only one that templates need
// in place of the standard library's <type_traits>.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// True if a `T` can be copied as its bytes.
template <typename T>
inline constexpr bool isTriviallyCopyable = __is_trivially_copyable(T);

// True if `T` is a class with no non-static data members, so that its size carries nothing.
template <typename T>
inline constexpr bool isEmpty = __is_empty(T);

// True for the integral types, bool and the character types included. MSVC has no __is_integral.
template <typename T>
inline constexpr bool isIntegral = false;
template <>
inline constexpr bool isIntegral<bool> = true;
template <>
inline constexpr bool isIntegral<char> = true;
template <>
inline constexpr bool isIntegral<signed char> = true;
template <>
inline constexpr bool isIntegral<unsigned char> = true;
template <>
inline constexpr bool isIntegral<char8_t> = true;
template <>
inline constexpr bool isIntegral<char16_t> = true;
template <>
inline constexpr bool isIntegral<char32_t> = true;
template <>
inline constexpr bool isIntegral<wchar_t> = true;
template <>
inline constexpr bool isIntegral<short> = true;
template <>
inline constexpr bool isIntegral<unsigned short> = true;
template <>
inline constexpr bool isIntegral<int> = true;
template <>
inline constexpr bool isIntegral<unsigned int> = true;
template <>
inline constexpr bool isIntegral<long> = true;
template <>
inline constexpr bool isIntegral<unsigned long> = true;
template <>
inline constexpr bool isIntegral<long long> = true;
template <>
inline constexpr bool isIntegral<unsigned long long> = true;

} // namespace pr

PR_ASSUME_NONNULL_END
