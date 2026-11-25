// UTF-8 and WTF-8: text as bytes.
//
// UTF-8 encodes Unicode scalar values, which are the code points other than the surrogates. WTF-8 also encodes a lone
// surrogate, as the three bytes that its value would take, so that it holds any sequence of UTF-16 code units, such as
// a JavaScript string or a DOMString. A surrogate pair is never two such sequences in WTF-8: it is the four bytes of
// the code point that the pair encodes. So every UTF-8 string is a WTF-8 string, and a WTF-8 string is UTF-8 if it
// holds no lone surrogate.
//
// A sequence is ill formed if it is truncated, overlong, encodes a value above U+10FFFF, or, in UTF-8, encodes a
// surrogate. Its maximal subpart is the longest run of bytes that begins a well-formed sequence, or one byte if none
// does. Decoding replaces each maximal subpart with one U+FFFD, as the web's UTF-8 decoder does. U+0000 is a code point
// like any other: a format that replaces or forbids it does so itself.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Code points and sizes of UTF-8 and WTF-8.
enum {
    kPrUtf8Replacement = 0xfffd,     // U+FFFD REPLACEMENT CHARACTER
    kPrUtf8MaxBytes = 4,             // the most bytes one code point takes
    kPrUtf8MaxCodePoint = 0x10ffff,  // the highest code point Unicode defines
};

// Decodes the code point at `offset` in `text`, writes it to `*outCodePoint`, and returns its length in bytes. An
// ill-formed sequence yields kPrUtf8Replacement and consumes its maximal subpart. At or past the end of `text`, returns
// zero and writes nothing.
uint32_t prUtf8Decode(PrStringView text, size_t offset, uint32_t* outCodePoint);

// Decodes the code point at `offset` in `text`, which is WTF-8, as prUtf8Decode does, except that a lone surrogate
// decodes as itself. A surrogate pair written as two three-byte surrogates, which WTF-8 does not allow, decodes as the
// code point of the pair, which is what those UTF-16 code units hold.
uint32_t prWtf8Decode(PrStringView text, size_t offset, uint32_t* outCodePoint);

// Decodes the code point at `offset` in `text`, which is WTF-8, as prWtf8Decode does, except that a lone surrogate
// decodes as kPrUtf8Replacement. Every code point that it yields is a Unicode scalar value, as when a DOMString
// converts to a USVString.
uint32_t prWtf8DecodeScalar(PrStringView text, size_t offset, uint32_t* outCodePoint);

// Returns the length of the longest prefix of `text` that is well formed: in UTF-8 for prUtf8Validate, and in WTF-8 for
// prWtf8Validate. The result is `text.size` if all of `text` is well formed.
size_t prUtf8Validate(PrStringView text);
size_t prWtf8Validate(PrStringView text);

// Encodes `codePoint` into `out` as UTF-8, and returns its length in bytes. A surrogate, or a value above
// kPrUtf8MaxCodePoint, is encoded as kPrUtf8Replacement.
uint32_t prUtf8Encode(uint32_t codePoint, char out[PR_NONNULL kPrUtf8MaxBytes]);

// Encodes `codePoint` into `out` as WTF-8, and returns its length in bytes: a surrogate as its three bytes, and a value
// above kPrUtf8MaxCodePoint as kPrUtf8Replacement. A high surrogate encoded and then a low one are not WTF-8, which
// writes a pair as the four bytes of its code point; prTextBufferAppendWtf8CodePoint joins them.
uint32_t prWtf8Encode(uint32_t codePoint, char out[PR_NONNULL kPrUtf8MaxBytes]);

// What prWtf8ToUtf16 returns for text that does not convert, and prWtf8FromUtf16 for units whose conversion is too long
// to count.
enum { kPrWtf8Invalid = 0xffffffff };

// Converts `count` UTF-16 code units to WTF-8: a surrogate pair as the code point that it encodes, and a lone surrogate
// as itself. Any sequence of units converts, and prWtf8ToUtf16 restores it. Returns the number of bytes, and writes
// them to `out` if `capacity` holds them all, so that a call with no capacity measures and a second call fills.
uint32_t prWtf8FromUtf16(const uint16_t* PR_NULLABLE units, uint32_t count, char* PR_NULLABLE out, uint32_t capacity);

// Converts `text`, WTF-8, to UTF-16 code units: a lone surrogate as itself. Returns the number of units, and writes
// them to `out` if `capacity` holds them all, or kPrWtf8Invalid if `text` is not well-formed WTF-8.
uint32_t prWtf8ToUtf16(PrStringView text, uint16_t* PR_NULLABLE out, uint32_t capacity);

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
// decoder reads UTF-8, and yields what prUtf8Decode does. Each maximal subpart of an ill-formed sequence decodes as one
// replacement. If the stream ends while the decoder is in a sequence (prUtf8DecoderIsInSequence), the stream ends with
// an ill-formed sequence, which also decodes as one replacement.
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
