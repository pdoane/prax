// LZFSE decoding, a piece of input at a time.
//
// lzfse - https://github.com/lzfse/lzfse, Apple's reference implementation, which is the format's specification

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// What a read needs next, or how the stream ended.
enum {
    LzfseResult_Input,          // more input
    LzfseResult_Output,         // more room for output
    LzfseResult_Done,           // the stream ended
    LzfseResult_Truncated,      // the final input ended inside the stream
    LzfseResult_Invalid,        // the stream is corrupt
};
typedef uint8_t LzfseResult;

// A decoder. It holds the decoding state, the output of earlier reads that back-references read, and a compressed block
// that spans reads, whose entropy-coded streams read from its end. A read decodes into its output directly. Once the
// stream ends, every read returns the same result until the decoder is reset.
struct Lzfse;

// Creates a decoder at the start of a stream.
Lzfse* lzfseCreate(void);

// Starts a new stream, keeping the storage.
void lzfseReset(Lzfse* decoder);

// Frees the decoder.
void lzfseDestroy(Lzfse* PR_NULLABLE decoder);

// Decodes from `bytes` into up to `capacity` bytes at `output`. Writes the number of input bytes read to `*consumed`
// and the number of output bytes written to `*written`. `final` means no input follows `bytes`.
//
// Bytes after the stream's end are left unread. Output may have been written before an Invalid or Truncated result.
LzfseResult lzfseRead(Lzfse* decoder, PrByteView bytes, bool final, size_t* consumed,
    uint8_t* PR_NULLABLE output, size_t capacity, size_t* written);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
