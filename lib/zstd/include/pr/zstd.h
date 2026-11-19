// Zstandard: decoding, a piece of input at a time, and the encoding of a whole input as one frame.
//
// rfc 8878 - Zstandard Compression and the 'application/zstd' Media Type

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// What a read needs next, or that the stream ended.
enum {
    PrZstdProgress_Input,       // more input
    PrZstdProgress_Output,      // more room for output
    PrZstdProgress_Done,        // the stream ended
};
typedef uint8_t PrZstdProgress;

// The result of a read.
typedef struct PrZstdRead {
    size_t consumed;            // number of input bytes read
    size_t written;             // number of output bytes written
    PrZstdProgress progress;
} PrZstdRead;

// The largest window that a decoder accepts unless it is told otherwise, as the reference decoder's default.
enum { kPrZstdDefaultWindowLimit = 1 << 27 };

// A decoder of a stream: its frames, one after another, whose contents follow one another, and skippable frames, which
// decode to nothing. It holds the decoding state, a header or a compressed block that spans reads, and the history: the
// window of each frame, which matches read back over, and the output that a read had no room for. A first read that has
// the whole stream decodes into its output directly while the output has room. One thread uses a decoder at a time.
typedef struct PrZstdDecoder PrZstdDecoder;

// Creates a decoder at the start of a stream, which accepts frames whose window is at most `windowLimit` bytes.
PrZstdDecoder* prZstdDecoderCreate(size_t windowLimit);

// Frees the decoder.
void prZstdDecoderDestroy(PrZstdDecoder* PR_NULLABLE decoder);

// Accepts frames whose window is at most `windowLimit` bytes, from the next frame on.
void prZstdDecoderSetWindowLimit(PrZstdDecoder* decoder, size_t windowLimit);

// Starts a new stream, keeping the storage and the window limit.
void prZstdDecoderReset(PrZstdDecoder* decoder);

// Decodes from `input` into up to `capacity` bytes at `output`, and writes the result to `*out`. `final` means no input
// follows `input`: the stream is the frames to the end of the final input. The frames' checksums are checked. Returns
// Truncated if the final input ends inside the stream, NotSupported for a frame that needs a dictionary, and
// InvalidData if the stream is corrupt or a frame's checksum does not match its content. A read that fails may have
// written output first, and once a stream ends, every read reports how it ended until the decoder is reset.
PrStatus prZstdDecoderRead(PrZstdDecoder* decoder, PrByteView input, bool final, uint8_t* PR_NULLABLE output,
    size_t capacity, PrZstdRead* out);

// Returns the content size that the first frame of `stream` states, or -1 if it states none.
int64_t prZstdContentSize(PrByteView stream);

// How hard the encoder searches for matches: as the reference encoder's levels 1, 3 and 9 search.
enum {
    PrZstdLevel_Fast,
    PrZstdLevel_Default,
    PrZstdLevel_Small,
};
typedef uint8_t PrZstdLevel;

// Returns the largest number of bytes that prZstdEncode writes for `size` bytes of input.
size_t prZstdEncodeBound(size_t size);

// Encodes `input` as one frame at `level`, with its content size and a checksum, into `output`, which has room for the
// bytes that prZstdEncodeBound returns, and returns the number of bytes written.
size_t prZstdEncode(PrZstdLevel level, PrByteView input, uint8_t* output);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
