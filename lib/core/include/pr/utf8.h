// UTF-8 decoding and encoding.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Code points and sizes of UTF-8 decoding and encoding.
enum {
    kPrUtf8Replacement = 0xfffd,     // U+FFFD REPLACEMENT CHARACTER
    kPrUtf8MaxBytes = 4,             // the most bytes one code point takes
    kPrUtf8MaxCodePoint = 0x10ffff,  // the highest code point Unicode defines
};

// Decodes the code point at `offset` in `text`, writes it to `*outCodePoint`, and returns its length in bytes.
// Ill-formed or truncated input yields kPrUtf8Replacement and consumes its maximal subpart: the longest run of bytes
// that begins a well-formed sequence, or one byte if none does. A surrogate decodes as a code point, and U+0000 as
// itself. At or past the end of `text`, returns zero and writes nothing.
uint32_t prUtf8Decode(PrStringView text, size_t offset, uint32_t* outCodePoint);

// Decodes the code point at `offset` in `text`, and yields only Unicode scalar values other than U+0000. The three
// bytes that encode a surrogate are ill-formed, so each yields kPrUtf8Replacement. U+0000 also yields
// kPrUtf8Replacement. In every other respect it behaves like prUtf8Decode.
uint32_t prUtf8DecodeScalar(PrStringView text, size_t offset, uint32_t* outCodePoint);

// Returns the length of the longest prefix of `text` that has no ill-formed sequence. The result is `text.size` if all
// of `text` is well-formed. A surrogate counts as well-formed, because prUtf8Decode decodes it as a code point.
size_t prUtf8Validate(PrStringView text);

// Encodes `codePoint` into `out` and returns its length in bytes. A surrogate, or a value above kPrUtf8MaxCodePoint, is
// encoded as kPrUtf8Replacement.
uint32_t prUtf8Encode(uint32_t codePoint, char out[PR_NONNULL kPrUtf8MaxBytes]);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
