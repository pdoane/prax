// Charsets: the encodings of the Encoding Standard, decoded to UTF-8 and encoded from it.
//
// encoding - https://encoding.spec.whatwg.org/

#pragma once

#include "pr/base.h"
#include "pr/charset_names.h"
#include "pr/string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Charsets

// Returns the charset that `label` names, or PrCharset_None if it names none (encoding/#concept-encoding-get). Leading
// and trailing ASCII whitespace is ignored, and ASCII letters match in either case.
PrCharset prCharsetFromLabel(PrStringView label);

// Returns the charset that the charset parameter of the MIME type `type` names (fetch/#legacy-extract-an-encoding), or
// PrCharset_None if the type does not parse, has no charset parameter, or names no charset.
PrCharset prCharsetFromMimeType(PrStringView type);

// Returns the canonical name of `charset` (encoding/#name), or the null view for PrCharset_None.
PrStringView prCharsetName(PrCharset charset);

// Returns the charset that text is encoded in when the document's charset is `charset`
// (encoding/#get-an-output-encoding). UTF-16BE, UTF-16LE and replacement have no encoder, so text is encoded as UTF-8
// instead.
PrCharset prCharsetOutput(PrCharset charset);

// Returns the charset whose byte order mark starts `bytes`, or PrCharset_None if they start with none
// (encoding/#bom-sniff). The charset is UTF-8, UTF-16BE or UTF-16LE, and its mark is 3, 2 and 2 bytes long.
PrCharset prCharsetSniffBom(PrStringView bytes);

// Returns true if `bytes`, the start of a stream, decide whether the stream starts with a byte order mark. They decide
// it once they are as long as any mark that they could start.
bool prCharsetIsBomDecided(PrStringView bytes);

// ---------------------------------------------------------------------------------------------------------------------
// Decoding

// How a decoder treats an error and a byte order mark.
enum {
    // An error stops the decode, which then fails (encoding/#error-mode). Otherwise each error decodes as U+FFFD.
    PrCharsetDecoderFlag_Fatal = 1u << 0,
    // A byte order mark of UTF-8, UTF-16BE or UTF-16LE at the start of the input decodes as U+FEFF. Otherwise the
    // decoder of that charset removes it.
    PrCharsetDecoderFlag_KeepBom = 1u << 1,
};
typedef uint8_t PrCharsetDecoderFlags;

// Whether the bytes passed to a decode end the input.
enum {
    PrCharsetInput_Partial,  // more bytes follow, and a sequence that the bytes leave incomplete waits for them
    PrCharsetInput_Final,    // the bytes end the input, and an incomplete sequence at their end is an error
};
typedef uint8_t PrCharsetInput;

// The decoding of an input in one charset, which may arrive in parts. A sequence of bytes that spans two parts decodes
// as if the parts had been one.
typedef struct PrCharsetDecoder {
    PrCharset charset;
    PrCharsetDecoderFlags flags;

    // The state of a decode in progress, which only the decoder reads and writes. The meaning of each member depends on
    // the charset.
    bool bomSeen;
    bool outputFlag;
    uint8_t state;
    uint8_t outputState;
    uint8_t needed;
    uint8_t seen;
    uint8_t lower;
    uint8_t upper;
    uint8_t lead[3];
    uint16_t surrogate;
    uint32_t codePoint;
} PrCharsetDecoder;

// Returns a decoder of `charset`, which is not PrCharset_None, at the start of its input.
PrCharsetDecoder prCharsetDecoderMake(PrCharset charset, PrCharsetDecoderFlags flags);

// Returns the most bytes that decoding the next `size` bytes of input can write.
size_t prCharsetDecodeMaxSize(const PrCharsetDecoder* decoder, size_t size);

// Decodes `bytes`, the next part of the input, writes the text to `out` as UTF-8, and sets `*outSize` to the number of
// bytes written. `out` holds at least prCharsetDecodeMaxSize bytes. A fatal decoder returns false at the first error,
// having written what came before it, and the bytes after the error are not read. The decoder keeps the state that the
// error left, as the spec's decoders do (encoding/#concept-encoding-process). After a final part, the decoder is at the
// start of a new input.
bool prCharsetDecode(PrCharsetDecoder* decoder, PrStringView bytes, PrCharsetInput input, char* out, size_t* outSize);

// Decodes `bytes`, the next part of the input, and returns its text as UTF-8. If the text is the bytes themselves, or
// the bytes without the byte order mark that the decoder removes, the result is a view of `bytes` and nothing is
// copied. Otherwise the text is appended to `out`, and the result is a view of what was appended. A fatal decoder
// returns the null view at the first error, and appends nothing. The decoder's state then and after a final part is
// that of prCharsetDecode.
PrStringView prCharsetDecodeView(PrCharsetDecoder* decoder, PrStringView bytes, PrCharsetInput input, PrString* out);

// ---------------------------------------------------------------------------------------------------------------------
// Encoding

// The encoding of text into one charset (encoding/#encoder), which may arrive in parts. ISO-2022-JP's encoder keeps the
// state that its escapes select from one part to the next.
typedef struct PrCharsetEncoder {
    PrCharset charset;
    uint8_t state;      // the state of an encode in progress, which only the encoder reads and writes
} PrCharsetEncoder;

// The code point that stops no encode: every code point of the text had bytes in the charset.
enum { kPrCharsetUnencodedNone = 0x110000 };

// Returns an encoder of `charset`, which has one: it is its own prCharsetOutput.
PrCharsetEncoder prCharsetEncoderMake(PrCharset charset);

// Returns the most bytes that encoding the next `size` bytes of text can write.
size_t prCharsetEncodeMaxSize(const PrCharsetEncoder* encoder, size_t size);

// Encodes `text`, the next part of the input, which is UTF-8 that holds scalar values, as far as the first code point
// that the charset has no bytes for (encoding/#encode-or-fail). Writes the bytes to `out`, which holds at least
// prCharsetEncodeMaxSize bytes, and sets `*outSize` to their number. Returns the number of bytes of `text` read, which
// include the code point that stopped the encode, and sets `*outUnencoded` to that code point, or to
// kPrCharsetUnencodedNone. A caller that writes the code point in some other form, as URL percent-encoding does, then
// encodes the rest of the text with the same encoder. Once a final part has encoded whole, the encoder is in its first
// state again, with the bytes that this takes written.
size_t prCharsetEncode(PrCharsetEncoder* encoder, PrStringView text, PrCharsetInput input, char* out, size_t* outSize,
    uint32_t* outUnencoded);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
