// Brotli decoding, a piece of input at a time.
//
// rfc7932 - https://www.rfc-editor.org/rfc/rfc7932

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// What a read needs next, or how the stream ended.
enum {
    BrotliResult_Input,         // more input
    BrotliResult_Output,        // more room for output
    BrotliResult_Done,          // the stream ended
    BrotliResult_Truncated,     // the final input ended inside the stream
    BrotliResult_Invalid,       // the stream is corrupt
};
typedef uint8_t BrotliResult;

// A decoder. It holds the decoding state, the window of output that back-references read, and the input of an
// incomplete step, which is at most a few bytes. Once the stream ends, every read returns the same result until the
// decoder is reset.
struct Brotli;

// Creates a decoder at the start of a stream.
Brotli* brotliCreate(void);

// Starts a new stream, keeping the storage.
void brotliReset(Brotli* decoder);

// Frees the decoder.
void brotliDestroy(Brotli* PR_NULLABLE decoder);

// Decodes from `bytes` into up to `capacity` bytes at `output`. Writes the number of input bytes read to `*consumed`
// and the number of output bytes written to `*written`. `final` means no input follows `bytes`.
//
// Bytes after the stream's end are left unread. Output may have been written before an Invalid or Truncated result.
BrotliResult brotliRead(Brotli* decoder, PrByteView bytes, bool final, size_t* consumed,
                        uint8_t* PR_NULLABLE output, size_t capacity, size_t* written);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
