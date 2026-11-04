// Base definitions: the annotations and macros that every header uses.

#pragma once

// The standard headers that the C API uses. The rest of the standard library is reached through core's own headers.
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Nullability annotations for declarations. Between PR_ASSUME_NONNULL_BEGIN and PR_ASSUME_NONNULL_END, every pointer is
// non-null unless it carries PR_NULLABLE.
#if defined(__clang__)
#define PR_NULLABLE _Nullable
#define PR_NONNULL _Nonnull
#define PR_ASSUME_NONNULL_BEGIN _Pragma("clang assume_nonnull begin")
#define PR_ASSUME_NONNULL_END _Pragma("clang assume_nonnull end")
#else
#define PR_NULLABLE
#define PR_NONNULL
#define PR_ASSUME_NONNULL_BEGIN
#define PR_ASSUME_NONNULL_END
#endif

// Keeps a function out of line, as for a cold path.
#if defined(_MSC_VER)
#define PR_NOINLINE __declspec(noinline)
#else
#define PR_NOINLINE __attribute__((noinline))
#endif

// Marks a function as rarely called, such as one that reports a failure, so that the compiler keeps it and the paths
// that reach it out of the way.
#if defined(_MSC_VER)
#define PR_COLD
#else
#define PR_COLD __attribute__((cold))
#endif

// Marks a C++ member function's result as referring into its object, written after its qualifiers, so that the compiler
// warns where the result outlives a temporary object, as a view of a temporary string does.
#if defined(__cplusplus) && defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::lifetimebound)
#define PR_LIFETIMEBOUND [[clang::lifetimebound]]
#elif __has_cpp_attribute(msvc::lifetimebound)
#define PR_LIFETIMEBOUND [[msvc::lifetimebound]]
#endif
#endif
#if !defined(PR_LIFETIMEBOUND)
#define PR_LIFETIMEBOUND
#endif

// Checks the arguments of a function that formats as printf does, written after its declaration: `string` is the
// position of its format string and `first` of the first argument that the string formats, counting from 1.
#if defined(_MSC_VER)
#define PR_PRINTF_FORMAT(string, first)
#else
#define PR_PRINTF_FORMAT(string, first) __attribute__((format(printf, string, first)))
#endif

// Inlines a function, even in an unoptimized build.
#if defined(_MSC_VER)
#define PR_FORCE_INLINE __forceinline
#else
#define PR_FORCE_INLINE inline __attribute__((always_inline))
#endif

// Inlines a lambda into each call of it, written after its parameter list: `[&](int x) PR_FORCE_INLINE_LAMBDA { ... }`.
// A loop that calls one lambda from several places then costs what a loop with the body written out does.
#if defined(_MSC_VER)
#define PR_FORCE_INLINE_LAMBDA [[msvc::forceinline]]
#else
#define PR_FORCE_INLINE_LAMBDA __attribute__((always_inline))
#endif

// Declares a function of the C runtime that MSVC's runtime exports from its DLL, in a build that links the runtime
// dynamically, as the runtime's own headers declare it.
#if defined(_MSC_VER) && defined(_DLL)
#define PR_CRT_IMPORT __declspec(dllimport)
#else
#define PR_CRT_IMPORT
#endif

// Defined as 1 if the build instruments memory accesses with AddressSanitizer, and 0 otherwise.
#if defined(__SANITIZE_ADDRESS__)
#define PR_ADDRESS_SANITIZER 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define PR_ADDRESS_SANITIZER 1
#endif
#endif
#if !defined(PR_ADDRESS_SANITIZER)
#define PR_ADDRESS_SANITIZER 0
#endif

// Implicit padding is an error for records between PR_NO_PADDING_BEGIN and PR_NO_PADDING_END. Every byte of such a
// record is a named field, so hashing or comparing its bytes is meaningful. A nested record needs its own pair.
#if defined(_MSC_VER)
#define PR_NO_PADDING_BEGIN __pragma(warning(push)) __pragma(warning(error : 4820))
#define PR_NO_PADDING_END __pragma(warning(pop))
#elif defined(__clang__) || defined(__GNUC__)
#define PR_NO_PADDING_BEGIN                        \
    _Pragma("GCC diagnostic push")                 \
    _Pragma("GCC diagnostic error \"-Wpadded\"")
#define PR_NO_PADDING_END _Pragma("GCC diagnostic pop")
#else
#define PR_NO_PADDING_BEGIN
#define PR_NO_PADDING_END
#endif

PR_ASSUME_NONNULL_BEGIN

// Fails compilation if `condition` is false, in C and C++.
#if defined(__cplusplus) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L)
#define PR_STATIC_ASSERT(condition) static_assert(condition, #condition)
#else
#define PR_STATIC_ASSERT(condition) _Static_assert(condition, #condition)
#endif

// Declares a function that never returns, in C and C++: `PR_NORETURN void f(void);`.
#if defined(__cplusplus) || (defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L)
#define PR_NORETURN [[noreturn]]
#else
#define PR_NORETURN _Noreturn
#endif

// Spells a braced value of `type` in a header that C and C++ share: `PR_LITERAL(PrStringView){data, size}`.
#if defined(__cplusplus)
#define PR_LITERAL(type) type
#else
#define PR_LITERAL(type) (type)
#endif

// Casts the pointer `value` to `type`, in a header that C and C++ share.
#if defined(__cplusplus)
#define PR_POINTER_CAST(type, value) reinterpret_cast<type>(value)
#else
#define PR_POINTER_CAST(type, value) ((type)(value))
#endif

// Converts the arithmetic `value` to `type`, in a header that C and C++ share.
#if defined(__cplusplus)
#define PR_VALUE_CAST(type, value) static_cast<type>(value)
#else
#define PR_VALUE_CAST(type, value) ((type)(value))
#endif

// Declares a function defined in a header that C and C++ share. The function is inline, and in C++ it is usable in
// constant expressions. C compilers fold calls to such a function on constant arguments.
#if defined(__cplusplus)
#define PR_CONSTEXPR constexpr
#else
#define PR_CONSTEXPR static inline
#endif

// Declares a constant of a header that C and C++ share that an enum cannot hold, such as a float. In C++ it is usable
// in constant expressions.
#if defined(__cplusplus)
#define PR_CONSTANT constexpr
#else
#define PR_CONSTANT static const
#endif

// Defines the upcasts of a handle to an interface that it inherits from: pr<Derived>As<Name>, and
// pr<Derived>AsConst<Name> for a const handle. `Derived` and `Base` are type names without `Pr`, and `Name` is the web
// interface name of the base. PR_HANDLE_UPCAST(DomText, DomNode, Node) defines prDomTextAsNode. An upcast returns the
// same pointer, and null stays null.
#define PR_HANDLE_UPCAST(Derived, Base, Name)                                               \
    static inline Pr##Base* PR_NULLABLE pr##Derived##As##Name(Pr##Derived* PR_NULLABLE derived) { \
        return PR_POINTER_CAST(Pr##Base*, derived);                                         \
    }                                                                                       \
    static inline const Pr##Base* PR_NULLABLE pr##Derived##AsConst##Name(                   \
        const Pr##Derived* PR_NULLABLE derived) {                                           \
        return PR_POINTER_CAST(const Pr##Base*, derived);                                   \
    }

PR_ASSUME_NONNULL_END

// The C++ API is in namespace pr. The implementation, in pr_impl, names the C++ API without qualifying it, and code
// that takes pr_impl with a using-directive takes pr with it.
#if defined(__cplusplus)
namespace pr {}
namespace pr_impl {
using namespace pr;
}
#endif
