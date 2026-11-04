// Formatting: text made from a format string and arguments, written to a Writer (pr/writer.h).
//
// A format string holds text and replacement fields. `{}` is the next argument, `{N}` is argument N, and either may
// carry a spec after a colon: `{:>8.2f}`. `{{` and `}}` are literal braces. A spec is
//
//   [[fill]align][sign][#][0][width][.precision][type]
//
//   fill       any character but a brace, before an align
//   align      `<` left, `>` right or `^` centered, within `width` bytes. Numbers default to right, and the rest to
//              left
//   sign       `+` for a sign on every number, ` ` for a space before a positive one, or `-`, the default, for
//              negatives only
//   #          the alternate form: a 0x, 0X, 0b, 0B or 0 prefix for an integer, and a point that a float always
//              keeps
//   0          zeros between a number's sign or prefix and its digits, to `width`, unless an align is given
//   width      the fewest bytes that the field takes, or `{}` or `{N}` for an integer argument
//   precision  digits after the point for f and e, significant digits for g, or the most bytes of text; or `{}`
//              or `{N}` for an integer argument
//   type       d, x, X, o, b or B for an integer's base, c for an integer as a character; f, F, e, E, g or G for a
//              float; s for text and p for a pointer
//
// A float with no type and no precision is the shortest decimal that reads back as the value, at the float's own width,
// so that 0.1f is "0.1". A precision with no type is as with g. The digits are correctly rounded, with ties to even. A
// format string that does not parse, or names an argument that is not there, fails an assertion; a release build writes
// what it can and skips the rest.
//
// The arguments are integers, enumerations (as their integers), bool, char, floats, text (PrStringView, and a
// NUL-terminated string as its text), and pointers, as their addresses. Another type joins them with an overload of
// formatValue beside it, which ADL finds.

#pragma once

#include "pr/base.h"
#include "pr/span.h"
#include "pr/string_view.h"
#include "pr/type_traits.h"
#include "pr/writer.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// A replacement field's spec, as this header describes it.
struct FormatSpec {
    uint32_t width = 0;
    int32_t precision = -1;         // -1 for none
    char fill = ' ';
    char align = '\0';              // '<', '>' or '^', or '\0' for the type's own
    char sign = '-';
    char type = '\0';               // '\0' for the type's own
    bool alternate = false;
};

// Write one value under `spec`.
void formatInteger(Writer& writer, bool negative, uint64_t magnitude, const FormatSpec& spec);
void formatFloat(Writer& writer, double value, const FormatSpec& spec);
void formatFloat(Writer& writer, float value, const FormatSpec& spec);
void formatText(Writer& writer, PrStringView text, const FormatSpec& spec);
void formatChar(Writer& writer, char value, const FormatSpec& spec);
void formatBool(Writer& writer, bool value, const FormatSpec& spec);
void formatPointer(Writer& writer, const void* PR_NULLABLE value, const FormatSpec& spec);

// The formatting of each type of argument.
template <typename T>
void formatValue(Writer& writer, T value, const FormatSpec& spec) requires(isIntegral<T> || isEnum<T>) {
    if constexpr (isEnum<T>) {
        formatValue(writer, UnderlyingType<T>(value), spec);
    } else if constexpr (isSame<T, bool>) {
        formatBool(writer, value, spec);
    } else if constexpr (isSame<T, char>) {
        formatChar(writer, value, spec);
    } else if constexpr (T(-1) < T(0)) {
        int64_t wide = int64_t(value);
        formatInteger(writer, wide < 0, wide < 0 ? 0 - uint64_t(wide) : uint64_t(wide), spec);
    } else {
        formatInteger(writer, false, uint64_t(value), spec);
    }
}
inline void formatValue(Writer& writer, double value, const FormatSpec& spec) {
    formatFloat(writer, value, spec);
}
inline void formatValue(Writer& writer, float value, const FormatSpec& spec) {
    formatFloat(writer, value, spec);
}
inline void formatValue(Writer& writer, PrStringView text, const FormatSpec& spec) {
    formatText(writer, text, spec);
}
inline void formatValue(Writer& writer, const char* PR_NULLABLE text, const FormatSpec& spec) {
    formatText(writer, PrStringView(text), spec);
}
inline void formatValue(Writer& writer, decltype(nullptr), const FormatSpec& spec) {
    formatPointer(writer, nullptr, spec);
}
template <typename T>
void formatValue(Writer& writer, T* PR_NULLABLE pointer, const FormatSpec& spec) requires(!isSame<RemoveConst<T>, char>) {
    formatPointer(writer, pointer, spec);
}

// An argument as formatting reads it: its value, the formatting of its type, and its integer for an argument that gives
// a width or a precision.
struct FormatArgument {
    const void* value;
    void (*format)(Writer& writer, const void* value, const FormatSpec& spec);
    bool (*integer)(const void* value, int64_t* out);
};

// Returns the argument for `value`, which must outlive it.
template <typename T>
FormatArgument formatArgument(const T& value) {
    FormatArgument argument{};
    argument.value = &value;
    argument.format = [](Writer& writer, const void* at, const FormatSpec& spec) {
        formatValue(writer, *static_cast<const T*>(at), spec);
    };
    if constexpr (isIntegral<T> && !isSame<T, bool>) {
        argument.integer = [](const void* at, int64_t* out) {
            *out = int64_t(*static_cast<const T*>(at));
            return true;
        };
    } else {
        argument.integer = [](const void*, int64_t*) { return false; };
    }
    return argument;
}

// Writes `format` with its fields replaced by `arguments`.
void formatArguments(Writer& writer, PrStringView format, Span<const FormatArgument> arguments);

// Writes `format` with its fields replaced by `arguments`, as in `formatTo(writer, "{} of {}", done, total)`.
template <typename... Arguments>
void formatTo(Writer& writer, PrStringView format, const Arguments&... arguments) {
    if constexpr (sizeof...(Arguments) == 0) {
        formatArguments(writer, format, Span<const FormatArgument>());
    } else {
        const FormatArgument converted[] = {formatArgument(arguments)...};
        formatArguments(writer, format, Span(converted));
    }
}

// Writes `format` with its fields replaced by `arguments` into `buffer`, and returns the text that fit.
template <typename... Arguments>
PrStringView formatToBuffer(Span<char> buffer, PrStringView format, const Arguments&... arguments) {
    Writer writer = bufferWriter(buffer);
    formatTo(writer, format, arguments...);
    return writer.view();
}

} // namespace pr

PR_ASSUME_NONNULL_END
