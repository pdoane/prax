// Text buffers: text under construction.

#pragma once

#include "pr/ascii.h"
#include "pr/base.h"
#include "pr/float.h"
#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"
#include "pr/utf8.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Text that is built by appending to it. Zeroed storage is an empty buffer. Growth invalidates `data`, so text that is
// appended must not point into the buffer.
typedef struct PrTextBuffer {
    char* PR_NULLABLE data;
    uint32_t size;
    uint32_t capacity;   // allocated bytes, from prMemoryAlloc
} PrTextBuffer;

// Reserves room for `size` more bytes, so that appending them does not grow the buffer. Growth at least doubles the
// capacity, so building text one append at a time copies each byte once, amortized.
void prTextBufferReserve(PrTextBuffer* buffer, uint32_t size);

// Appends `text`, bytes read as UTF-8, and replaces each maximal subpart of an ill-formed sequence, as prUtf8Decode
// consumes it, with kPrUtf8Replacement, so that what it appends is UTF-8. A surrogate's three bytes are ill formed
// here.
void prTextBufferAppendUtf8(PrTextBuffer* buffer, PrStringView text);

// Returns `text` as UTF-8: `text` itself if it holds no lone surrogate or ill-formed sequence, and otherwise its code
// points as prWtf8DecodeScalar reads them, written to `scratch`, which this clears. A lone surrogate becomes
// kPrUtf8Replacement, as converting a DOMString to a USVString does.
PrStringView prTextBufferUtf8FromWtf8(PrTextBuffer* scratch, PrStringView text);

// Appends `text` to text that is WTF-8, and replaces each maximal subpart of an ill-formed sequence with
// kPrUtf8Replacement, so that the buffer stays WTF-8. A lone surrogate stays itself, and a surrogate pair written as
// its two surrogates, here or across the buffer's end and `text`'s start, becomes the four bytes of its pair.
void prTextBufferAppendWtf8(PrTextBuffer* buffer, PrStringView text);

// Appends `codePoint` to text that is WTF-8, encoded by prWtf8Encode. A low surrogate after a high surrogate joins it:
// the two become the four bytes of their pair, as WTF-8 requires.
void prTextBufferAppendWtf8CodePoint(PrTextBuffer* buffer, uint32_t codePoint);

// Frees the storage and leaves the buffer empty.
void prTextBufferReset(PrTextBuffer* buffer);

// Returns the text as a string that the caller owns. The buffer is left empty, with no storage. If the text does not
// fit inline, the string takes the storage of the buffer.
PrRawString prTextBufferTake(PrTextBuffer* buffer);

// Appends the bytes of `text`.
static inline void prTextBufferAppend(PrTextBuffer* buffer, PrStringView text) {
    if (text.size == 0)
        return;
    if (text.size > buffer->capacity - buffer->size)
        prTextBufferReserve(buffer, PR_VALUE_CAST(uint32_t, text.size));
    prMemCopy(buffer->data + buffer->size, text.data, text.size);
    buffer->size += PR_VALUE_CAST(uint32_t, text.size);
}

// Inserts the bytes of `text` at the offset `at`, and moves the bytes from `at` onward up.
static inline void prTextBufferInsert(PrTextBuffer* buffer, uint32_t at, PrStringView text) {
    if (text.size == 0)
        return;
    if (text.size > buffer->capacity - buffer->size)
        prTextBufferReserve(buffer, PR_VALUE_CAST(uint32_t, text.size));
    prMemMove(buffer->data + at + text.size, buffer->data + at, buffer->size - at);
    prMemCopy(buffer->data + at, text.data, text.size);
    buffer->size += PR_VALUE_CAST(uint32_t, text.size);
}

// Appends the byte `c`.
static inline void prTextBufferAppendChar(PrTextBuffer* buffer, char c) {
    if (buffer->size == buffer->capacity)
        prTextBufferReserve(buffer, 1);
    buffer->data[buffer->size++] = c;
}

// Appends `codePoint` encoded as UTF-8 by prUtf8Encode.
static inline void prTextBufferAppendCodePoint(PrTextBuffer* buffer, uint32_t codePoint) {
    char encoded[kPrUtf8MaxBytes];
    uint32_t size = prUtf8Encode(codePoint, encoded);
    prTextBufferAppend(buffer, PR_LITERAL(PrStringView){encoded, size});
}

// Appends the digits of `value` in `radix`, written by prU64Format.
static inline void prTextBufferAppendU64(PrTextBuffer* buffer, uint64_t value, uint32_t radix) {
    char digits[kPrU64TextMaxBytes];
    size_t size = prU64Format(value, digits, radix);
    prTextBufferAppend(buffer, PR_LITERAL(PrStringView){digits, size});
}

// Appends `value` rounded to `decimals` places, written by prF64FormatFixed.
static inline void prTextBufferAppendF64Fixed(PrTextBuffer* buffer, double value, uint32_t decimals) {
    char digits[kPrF64FixedMaxBytes];
    size_t size = prF64FormatFixed(value, decimals, digits);
    prTextBufferAppend(buffer, PR_LITERAL(PrStringView){digits, size});
}

// Returns a view of the text, which is valid until the buffer changes. A buffer that has never been written to gives
// the empty string, not the null view.
static inline PrStringView prTextBufferView(const PrTextBuffer* buffer) {
    if (buffer->data)
        return PR_LITERAL(PrStringView){buffer->data, buffer->size};
    return PR_LITERAL(PrStringView){"", 0};
}

// Folds every ASCII uppercase byte of the text to lowercase.
static inline void prTextBufferAsciiLower(PrTextBuffer* buffer) {
    if (buffer->size != 0)
        prAsciiLowerText(buffer->data, buffer->size);
}

// Folds every ASCII lowercase byte of the text to uppercase.
static inline void prTextBufferAsciiUpper(PrTextBuffer* buffer) {
    if (buffer->size != 0)
        prAsciiUpperText(buffer->data, buffer->size);
}

// Sets the size of the text to `size`, growing the storage if it is larger than the capacity, so that a writer can fill
// the bytes in place. Bytes past the old size are uninitialized.
static inline void prTextBufferResize(PrTextBuffer* buffer, uint32_t size) {
    if (size > buffer->capacity)
        prTextBufferReserve(buffer, size - buffer->size);
    buffer->size = size;
}

// Empties the text and keeps its storage.
static inline void prTextBufferClear(PrTextBuffer* buffer) {
    buffer->size = 0;
}

// Returns a copy of the text, as a string that the caller owns. The buffer is unchanged.
static inline PrRawString prTextBufferCopy(const PrTextBuffer* buffer) {
    return prRawStringCopy(prTextBufferView(buffer));
}

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
