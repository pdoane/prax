// The tests' doctest: doctest itself, and how its failures print core's text and bytes. Every test includes this header
// rather than doctest's own (lint.py, `test-include`), so that a failed check shows the values it compared.

#pragma once

#include "pr/byte_view.h"
#include "pr/raw_string.h"
#include "pr/string.h"
#include "pr/string_view.h"

#include "doctest/doctest.h"

PR_ASSUME_NONNULL_BEGIN

namespace doctest {

// Text prints in quotes, as doctest prints a C string, and the null view prints as doctest prints a null C string.
template <>
struct StringMaker<PrStringView> {
    static String convert(const PrStringView& text) {
        if (text.isNull())
            return "{null string}";
        return String("\"") + String(text.data, String::size_type(text.size)) + "\"";
    }
};
template <>
struct StringMaker<PrRawString> {
    static String convert(const PrRawString& text) { return StringMaker<PrStringView>::convert(prRawStringView(&text)); }
};
template <>
struct StringMaker<pr::String> {
    static String convert(const pr::String& text) { return StringMaker<PrStringView>::convert(text.view()); }
};

// Bytes print in hexadecimal, as many as fit a line, and then how many there are.
template <>
struct StringMaker<PrByteView> {
    static String convert(const PrByteView& bytes) {
        constexpr size_t kShown = 32;
        constexpr char kDigits[] = "0123456789abcdef";
        if (bytes.isNull())
            return "{null bytes}";
        char text[kShown * 3 + 4];
        size_t used = 0;
        for (size_t i = 0; i < bytes.size && i < kShown; ++i) {
            if (i != 0)
                text[used++] = ' ';
            text[used++] = kDigits[bytes.data[i] >> 4];
            text[used++] = kDigits[bytes.data[i] & 15];
        }
        if (bytes.size > kShown) {
            text[used++] = ' ';
            text[used++] = '.';
            text[used++] = '.';
            text[used++] = '.';
        }
        String shown = String("{") + String(text, String::size_type(used)) + "}";
        return shown + " (" + toString(uint64_t(bytes.size)) + " bytes)";
    }
};

} // namespace doctest

PR_ASSUME_NONNULL_END
