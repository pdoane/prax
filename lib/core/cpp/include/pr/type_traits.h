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

// True if destroying a `T` runs no code.
template <typename T>
inline constexpr bool isTriviallyDestructible = __is_trivially_destructible(T);

// True if a `T` can be made from `Arguments`.
template <typename T, typename... Arguments>
inline constexpr bool isConstructible = __is_constructible(T, Arguments...);

// True if a `From` converts implicitly to a `To`. Everything converts to void, as a discarded result does.
template <typename From, typename To>
inline constexpr bool isConvertible = requires(void (*accept)(To), From (*make)()) { accept(make()); };
template <typename From>
inline constexpr bool isConvertible<From, void> = true;

// True if `T` is a class with no non-static data members, so that its size carries nothing.
template <typename T>
inline constexpr bool isEmpty = __is_empty(T);

// True if `T` is an enumeration.
template <typename T>
inline constexpr bool isEnum = __is_enum(T);

// The integer type that the enumeration `T` is stored as.
template <typename T>
using UnderlyingType = __underlying_type(T);

// True if `A` and `B` are the same type.
template <typename A, typename B>
inline constexpr bool isSame = false;
template <typename T>
inline constexpr bool isSame<T, T> = true;

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

// `T` without a reference.
template <typename T>
struct RemoveReferenceOf {
    using Type = T;
};
template <typename T>
struct RemoveReferenceOf<T&> {
    using Type = T;
};
template <typename T>
struct RemoveReferenceOf<T&&> {
    using Type = T;
};
template <typename T>
using RemoveReference = typename RemoveReferenceOf<T>::Type;

// `T` without const.
template <typename T>
struct RemoveConstOf {
    using Type = T;
};
template <typename T>
struct RemoveConstOf<const T> {
    using Type = T;
};
template <typename T>
using RemoveConst = typename RemoveConstOf<T>::Type;

// `T` as a value of it is stored: without a reference or const, and an array or a function as a pointer to it.
template <typename T>
struct DecayOf {
    using Type = RemoveConst<T>;
};
template <typename T, size_t N>
struct DecayOf<T[N]> {
    using Type = T*;
};
template <typename T>
struct DecayOf<T[]> {
    using Type = T*;
};
template <typename Result, typename... Arguments>
struct DecayOf<Result(Arguments...)> {
    using Type = Result (*)(Arguments...);
};
template <typename Result, typename... Arguments>
struct DecayOf<Result(Arguments...) noexcept> {
    using Type = Result (*)(Arguments...) noexcept;
};
template <typename T>
using Decay = typename DecayOf<RemoveReference<T>>::Type;

// Casts `value` to an rvalue, so that it is moved from.
template <typename T>
constexpr RemoveReference<T>&& move(T&& value) {
    return static_cast<RemoveReference<T>&&>(value);
}

// Passes `value` on as the kind of reference that it was passed as.
template <typename T>
constexpr T&& forward(RemoveReference<T>& value) {
    return static_cast<T&&>(value);
}
template <typename T>
constexpr T&& forward(RemoveReference<T>&& value) {
    return static_cast<T&&>(value);
}

} // namespace pr

PR_ASSUME_NONNULL_END
