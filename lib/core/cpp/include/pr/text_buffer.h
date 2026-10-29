// Text buffers: text under construction.

#pragma once

#include "pr/array.h"
#include "pr/ascii.h"
#include "pr/integer.h"
#include "pr/string.h"
#include "pr/string_view.h"
#include "pr/utf8.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Text that is built by appending to it.
struct TextBuffer {
    Array<char> text;
};

// Appends the `length` bytes at `data`, or the bytes of the view `data`.
inline void textAppend(TextBuffer* out, const char* data, uint32_t length) {
    if (length != 0)
        out->text.append(data, length);
}
inline void textAppend(TextBuffer* out, PrStringView data) {
    if (data.size != 0)
        out->text.append(data.data, uint32_t(data.size));
}

// Appends the byte `c`.
inline void textAppendChar(TextBuffer* out, char c) {
    out->text.push(c);
}

// Appends `codePoint` encoded as UTF-8 by prUtf8Encode.
inline void textAppendCodePoint(TextBuffer* out, uint32_t codePoint) {
    char encoded[kPrUtf8MaxBytes];
    uint32_t size = prUtf8Encode(codePoint, encoded);
    textAppend(out, encoded, size);
}

// Appends the digits of `value` in `radix`, written by formatU64.
inline void textAppendU64(TextBuffer* out, uint64_t value, uint32_t radix = 10) {
    char buffer[kU64TextMaxBytes];
    size_t size = formatU64(value, buffer, radix);
    textAppend(out, buffer, uint32_t(size));
}

// Appends `text`, and replaces each maximal subpart of an ill-formed sequence with kPrUtf8Replacement. The maximal
// subparts are those that prUtf8Decode consumes.
void textAppendUtf8(TextBuffer* out, PrStringView text);

// Returns a view of the text, which is valid until the buffer changes. A buffer that has never been written to gives
// the empty string, not the null view.
inline PrStringView textView(const TextBuffer* out) {
    if (out->text.data != nullptr)
        return {out->text.data, out->text.count};
    return {"", 0};
}

// Reserves room for `size` more bytes, so that appending them does not grow the buffer.
inline void textReserve(TextBuffer* out, uint32_t size) {
    out->text.reserve(out->text.count + size);
}

// Folds every ASCII uppercase byte of the text to lowercase.
inline void textAsciiLower(TextBuffer* out) {
    if (out->text.count != 0)
        asciiLower(out->text.data, out->text.count);
}

// Folds every ASCII lowercase byte of the text to uppercase.
inline void textAsciiUpper(TextBuffer* out) {
    if (out->text.count != 0)
        asciiUpper(out->text.data, out->text.count);
}

// Empties the text and keeps its storage.
inline void textClear(TextBuffer* out) {
    out->text.count = 0;
}

// Frees the storage and leaves the text empty.
inline void textReset(TextBuffer* out) {
    out->text.reset();
}

// Returns a copy of the text, as a string that the caller owns. The buffer is unchanged.
inline PrString textCopy(const TextBuffer* out) {
    return prStringCopy(textView(out));
}

// Returns the text as a string that the caller owns. The buffer is left empty, with no storage. If the text does not
// fit inline, the string takes the storage of the buffer.
PrString textTake(TextBuffer* out);

} // namespace pr

PR_ASSUME_NONNULL_END
