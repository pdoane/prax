// LZ4 blocks: the block format without a frame. A block ends where its input ends, and has no checksum.
//
// LZ4 Block Format Description - https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// What a read needs next, or that the block ended.
enum {
    PrLz4Progress_Input,        // more input
    PrLz4Progress_Output,       // more room for output
    PrLz4Progress_Done,         // the block ended
};
typedef uint8_t PrLz4Progress;

// The result of a read.
typedef struct PrLz4Read {
    size_t consumed;            // number of input bytes read
    size_t written;             // number of output bytes written
    PrLz4Progress progress;
} PrLz4Read;

// Decodes the whole block in `block` into up to `capacity` bytes at `output`, and writes the result to `*out`. A block
// does not state its own size, so it ends where `block` ends. The progress is Output if the decoded block does not fit.
// Returns Truncated if the block ends inside a sequence, and InvalidData if it is corrupt. Output may have been written
// before a failure.
PrStatus prLz4DecodeBlock(PrByteView block, uint8_t* PR_NULLABLE output, size_t capacity, PrLz4Read* out);

// A decoder of one block that arrives in pieces, or whose output is taken in pieces. A block cannot be decoded before
// its end is known, so the decoder holds the input until the final piece, and then holds whatever output a read has no
// room for. One thread uses a decoder at a time.
typedef struct PrLz4Decoder PrLz4Decoder;

// Creates a decoder at the start of a block.
PrLz4Decoder* prLz4DecoderCreate(void);

// Frees the decoder.
void prLz4DecoderDestroy(PrLz4Decoder* PR_NULLABLE decoder);

// Starts a new block, keeping the storage.
void prLz4DecoderReset(PrLz4Decoder* decoder);

// Decodes from `input` into up to `capacity` bytes at `output`, and writes the result to `*out`. `final` means no input
// follows `input`, which is where the block ends. Returns Truncated if the block ends inside a sequence, and
// InvalidData if it is corrupt. Once the block ends, every read reports how it ended until the decoder is reset.
PrStatus prLz4DecoderRead(PrLz4Decoder* decoder, PrByteView input, bool final, uint8_t* PR_NULLABLE output,
    size_t capacity, PrLz4Read* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
