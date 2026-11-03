// Base64: encoding and decoding of bytes.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Returns the size of `size` bytes encoded as base64, padding included.
size_t prBase64EncodedSize(size_t size);

// Encodes `input` as base64 with padding, writes up to `capacity` bytes of it to `out`, and returns the full size,
// which is prBase64EncodedSize(input.size). `out` must not overlap `input`.
size_t prBase64Encode(PrStringView input, char* PR_NULLABLE out, size_t capacity);

// Decodes the base64 `input`, writes up to `capacity` bytes of it to `out`, and writes the full size to `*size`. `out`
// may be `input.data`. ASCII whitespace is ignored. Padding is optional: one or two '=' may end a text whose length is
// a multiple of four. Returns false if any other character is outside the alphabet, or if the number of alphabet
// characters is one more than a multiple of four.
bool prBase64Decode(PrStringView input, char* PR_NULLABLE out, size_t capacity, size_t* size);

// Returns a capacity that holds the decoding of any base64 text of `size` characters.
PR_CONSTEXPR size_t prBase64DecodedCapacity(size_t size) {
    return size / 4 * 3 + size % 4;
}

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
