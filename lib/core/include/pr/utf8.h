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

// A decoder of UTF-8 that receives one byte at a time, as from a stream whose pieces may end in the middle of a
// sequence. Between bytes, it holds the bits of the sequence in progress, the number of continuation bytes still
// needed, and the range that the next byte must fall in. A zeroed decoder is between sequences.
typedef struct PrUtf8Decoder {
    uint32_t codePoint;
    uint8_t needed;
    uint8_t lower;
    uint8_t upper;
} PrUtf8Decoder;

// What a byte does to a decoder.
enum {
    PrUtf8Step_Continue,        // the byte begins or continues a sequence that needs more bytes
    PrUtf8Step_CodePoint,       // the byte completes a code point
    PrUtf8Step_Invalid,         // the byte begins no sequence, and decodes as kPrUtf8Replacement

    // The byte cannot continue the sequence in progress, and that sequence decodes as kPrUtf8Replacement. The byte is
    // not consumed, so the caller pushes it again.
    PrUtf8Step_Interrupted,
};
typedef uint8_t PrUtf8Step;

// Pushes `byte` after the bytes that the decoder holds. For CodePoint, writes the code point to `*outCodePoint`. The
// decoder yields Unicode scalar values as prUtf8DecodeScalar does, except that U+0000 decodes as itself. Each maximal
// subpart of an ill-formed sequence decodes as one replacement. If the stream ends while the decoder is in a sequence
// (prUtf8DecoderIsInSequence), the stream ends with an ill-formed sequence, which also decodes as one replacement.
static inline PrUtf8Step prUtf8DecoderPush(PrUtf8Decoder* decoder, uint8_t byte, uint32_t* outCodePoint) {
    if (decoder->needed == 0) {
        if (byte < 0x80) {
            *outCodePoint = byte;
            return PrUtf8Step_CodePoint;
        }
        // The range of the second byte excludes the overlong forms, the surrogates and the values past U+10FFFF.
        decoder->lower = 0x80;
        decoder->upper = 0xbf;
        if (byte >= 0xc2 && byte <= 0xdf) {
            decoder->needed = 1;
            decoder->codePoint = byte & 0x1fu;
        } else if (byte >= 0xe0 && byte <= 0xef) {
            decoder->needed = 2;
            decoder->codePoint = byte & 0x0fu;
            if (byte == 0xe0)
                decoder->lower = 0xa0;
            else if (byte == 0xed)
                decoder->upper = 0x9f;
        } else if (byte >= 0xf0 && byte <= 0xf4) {
            decoder->needed = 3;
            decoder->codePoint = byte & 0x07u;
            if (byte == 0xf0)
                decoder->lower = 0x90;
            else if (byte == 0xf4)
                decoder->upper = 0x8f;
        } else {
            return PrUtf8Step_Invalid;
        }
        return PrUtf8Step_Continue;
    }
    if (byte < decoder->lower || byte > decoder->upper) {
        decoder->needed = 0;
        return PrUtf8Step_Interrupted;
    }
    decoder->codePoint = decoder->codePoint << 6 | (byte & 0x3fu);
    decoder->lower = 0x80;
    decoder->upper = 0xbf;
    if (--decoder->needed != 0)
        return PrUtf8Step_Continue;
    *outCodePoint = decoder->codePoint;
    return PrUtf8Step_CodePoint;
}

// Returns true if the decoder holds part of a sequence.
static inline bool prUtf8DecoderIsInSequence(const PrUtf8Decoder* decoder) {
    return decoder->needed != 0;
}

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
