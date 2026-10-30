// Deflate decoding, bare or framed as zlib or gzip, a piece of input at a time.
//
// rfc1950 - https://www.rfc-editor.org/rfc/rfc1950
// rfc1951 - https://www.rfc-editor.org/rfc/rfc1951
// rfc1952 - https://www.rfc-editor.org/rfc/rfc1952

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The framing around the Deflate data. A gzip stream may hold several members.
enum {
    InflateFormat_Raw,      // bare Deflate
    InflateFormat_Zlib,
    InflateFormat_Gzip,
};
typedef uint8_t InflateFormat;

// What a read needs next, or how the stream ended.
enum {
    InflateResult_Input,        // more input
    InflateResult_Output,       // more room for output
    InflateResult_Dictionary,   // a zlib preset dictionary, given by inflateSetDictionary
    InflateResult_Done,         // the stream ended
    InflateResult_Truncated,    // the final input ended inside the stream
    InflateResult_Invalid,      // the stream is corrupt
};
typedef uint8_t InflateResult;

// A decoder. It holds the decoding state and the history that back-references read. It keeps no reference to the spans
// that a read is given. Once the stream ends, every read returns the same result until the decoder is reset.
struct Inflate;

// Creates a decoder at the start of a stream in `format`.
Inflate* inflateCreate(InflateFormat format);

// Starts a new stream in `format`, keeping the storage.
void inflateReset(Inflate* parser, InflateFormat format);

// Frees the decoder.
void inflateDestroy(Inflate* PR_NULLABLE parser);

// Returns the Adler-32 checksum of the preset dictionary requested by a read that returned Dictionary.
uint32_t inflateDictionaryChecksum(const Inflate* parser);

// Supplies the preset dictionary requested by a read that returned Dictionary. Returns false if the checksum of
// `dictionary` is not the one the stream names. The decoder then keeps waiting for the right one.
bool inflateSetDictionary(Inflate* parser, PrByteView dictionary);

// Decodes from `bytes` into up to `capacity` bytes at `output`. Writes the number of input bytes read to `*consumed`
// and the number of output bytes written to `*written`. `final` means no input follows `bytes`.
//
// Raw and zlib streams leave any bytes after their end unread. Gzip reads members to the end of the final input. The
// checksums of framed streams are checked. Output may have been written before an Invalid or Truncated result.
InflateResult inflateRead(Inflate* parser, PrByteView bytes, bool final, size_t* consumed,
    uint8_t* PR_NULLABLE output, size_t capacity, size_t* written);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
