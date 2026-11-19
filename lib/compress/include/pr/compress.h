// Compression codecs, which decode compressed byte streams and may encode them, and the sets that a program builds of
// them.
//
// A codec codes the bare formats that its backend codes: Deflate, Brotli and LZFSE. Each backend has its own
// constructor, such as pr/compress_deflate.h's, and one bare format may have several backends: Prax's own, and the
// platform's (pr/compress_apple.h). A program puts the codecs that it wants in a set, and a library that decodes or
// encodes takes the set from its caller. The set frames zlib and gzip itself, around the Deflate of its codec.
//
// A codec and a set may be used from any number of threads at once. A decoder is used by one thread at a time.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A source of decoders and encodings, for the formats that its backend reads.
typedef struct PrCompressCodec PrCompressCodec;

// Codecs in priority order (prCompressCodecSetCreate).
typedef struct PrCompressCodecSet PrCompressCodecSet;

// One compressed stream being decoded.
typedef struct PrCompressDecoder PrCompressDecoder;

// The encodings a decoder reads. A decoder checks the checksums of the framed formats.
enum {
    PrCompressFormat_Deflate,   // bare Deflate
    PrCompressFormat_Zlib,      // Deflate framed as zlib
    PrCompressFormat_Gzip,      // Deflate framed as gzip, whose members decode as one stream
    PrCompressFormat_Brotli,    // Brotli (rfc 7932)
    PrCompressFormat_Lzfse,     // LZFSE, Apple's LZ77 with finite state entropy coding, and its LZVN blocks
    PrCompressFormat_Lz4,       // one LZ4 block, without a frame: it ends where its input ends, and has no checksum
    PrCompressFormat_Zstd,      // Zstandard (rfc 8878): frames, whose contents decode as one stream
    PrCompressFormat_Count,
};
typedef uint8_t PrCompressFormat;

// How hard an encoder works to make its output small, against how long it takes.
enum {
    PrCompressLevel_Fast,
    PrCompressLevel_Default,
    PrCompressLevel_Small,
    PrCompressLevel_Count,
};
typedef uint8_t PrCompressLevel;

// The progress of a read, which says what it needs next.
enum {
    PrCompressProgress_Input,    // more input
    PrCompressProgress_Output,   // more room for output
    PrCompressProgress_Done,     // nothing, because the stream ended
};
typedef uint8_t PrCompressProgress;

// The result of a read.
typedef struct PrCompressRead {
    size_t consumed;                // number of input bytes read
    size_t written;                 // number of output bytes written
    PrCompressProgress progress;
} PrCompressRead;

// Frees a codec that no set owns. Its decoders must be destroyed first.
void prCompressCodecDestroy(PrCompressCodec* PR_NULLABLE codec);

// ---------------------------------------------------------------------------------------------------------------------
// Sets

// Creates a set of `count` codecs, in priority order, and takes ownership of them. The set does not change once it is
// created, so any thread may use it. A stream is decoded by the first codec that decodes its bare format, and encoded
// by the first codec that encodes it. A codec whose Deflate decoder may read past the end of its stream decodes zlib
// and gzip only where the frame ends with the input: a zlib stream with bytes after it, or a gzip stream of several
// members, is then NotSupported. Only the codecs that a program creates are linked into it, so a program that names no
// Brotli codec carries no Brotli decoder.
PrCompressCodecSet* prCompressCodecSetCreate(PrCompressCodec* PR_NONNULL const* PR_NULLABLE codecs, uint32_t count);

// Frees the set and its codecs. Its decoders must be destroyed first.
void prCompressCodecSetDestroy(PrCompressCodecSet* PR_NULLABLE set);

// Returns true if a codec of the set decodes `format`. If one does, prCompressDecoderCreate does not return
// NotSupported for it.
bool prCompressCodecSetDecodes(const PrCompressCodecSet* set, PrCompressFormat format);

// Returns true if a codec of the set encodes `format`.
bool prCompressCodecSetEncodes(const PrCompressCodecSet* set, PrCompressFormat format);

// Creates a decoder at the start of a stream in `format`, with the first codec of the set that decodes it. Returns
// NotSupported if no codec of the set decodes `format`. On failure, `*out` is null.
PrStatus prCompressDecoderCreate(const PrCompressCodecSet* set, PrCompressFormat format,
    PrCompressDecoder* PR_NULLABLE* PR_NONNULL out);

// Frees the decoder.
void prCompressDecoderDestroy(PrCompressDecoder* PR_NULLABLE decoder);

// Starts a new stream in the decoder's format, keeping its storage and its window limit, so that one decoder decodes
// many streams without making its state again.
void prCompressDecoderReset(PrCompressDecoder* decoder);

// Holds the stream's window, the output that its matches may reach back over and that the decoder keeps, to `limit`
// bytes: a stream that names a larger window is InvalidData, so that a stream cannot make the decoder take more memory
// than its caller allows. It is set before the first read, and stays across resets. Zstandard's window is the stream's
// to choose, up to 128M by default, as the reference decoder's is. Every other format bounds its own: Deflate's at 32K,
// Brotli's at 16M, and LZFSE's and LZ4's at their block, and the limit does not change them.
void prCompressDecoderSetWindowLimit(PrCompressDecoder* decoder, size_t limit);

// Decodes as much of `input` as it can into up to `capacity` bytes at `output`, and writes the result to `*out`.
// `final` means no input follows `input`.
//
// Returns InvalidData if the stream is corrupt, Truncated if the final input ends inside the stream, and NotSupported
// if the stream needs a preset dictionary. After a failure, the output already written stays, and the decoder reads no
// further.
PrStatus prCompressDecoderRead(PrCompressDecoder* decoder, PrByteView input, bool final,
    uint8_t* PR_NULLABLE output, size_t capacity, PrCompressRead* out);

// Decodes the whole stream in `input` into up to `capacity` bytes at `output`. Returns the same failures as
// prCompressDecoderCreate and prCompressDecoderRead. The progress is Output if the decoded stream does not fit.
PrStatus prCompressCodecSetDecode(const PrCompressCodecSet* set, PrCompressFormat format, PrByteView input,
    uint8_t* PR_NULLABLE output, size_t capacity, PrCompressRead* out);

// Returns the largest number of bytes that prCompressCodecSetEncode writes for `size` bytes of input in `format`.
// Returns zero if no codec of the set encodes `format`.
size_t prCompressCodecSetEncodeBound(const PrCompressCodecSet* set, PrCompressFormat format, size_t size);

// Encodes the whole of `input` as one stream in `format` at `level`, and sets `*written` to the size of the stream.
// `output` must have room for the bound that prCompressCodecSetEncodeBound returns. Returns NotSupported if no codec of
// the set encodes `format`, and then writes nothing.
PrStatus prCompressCodecSetEncode(const PrCompressCodecSet* set, PrCompressFormat format, PrCompressLevel level,
    PrByteView input, uint8_t* output, size_t* written);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
