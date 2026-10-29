// Compression codecs, which decode compressed byte streams and may encode them.
//
// A codec may be used from any number of threads at once. A decoder is used by one thread at a time.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A source of decoders. Each backend provides its own constructor.
typedef struct PrCompressCodec PrCompressCodec;

// One compressed stream being decoded. Only the codec that made it reads it.
typedef struct PrCompressDecoder PrCompressDecoder;

// The encodings a decoder reads. A decoder checks the checksums of the framed formats.
enum {
    PrCompressFormat_Deflate,   // bare Deflate
    PrCompressFormat_Zlib,      // Deflate framed as zlib
    PrCompressFormat_Gzip,      // Deflate framed as gzip, whose members decode as one stream
    PrCompressFormat_Brotli,    // Brotli (rfc 7932)
};
typedef uint8_t PrCompressFormat;

// How hard an encoder works to make its output small, against how long it takes.
enum {
    PrCompressLevel_Fast,
    PrCompressLevel_Default,
    PrCompressLevel_Small,
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

// The start of every decoder's state. `codec` is the codec that made the decoder.
struct PrCompressDecoder {
    PrCompressCodec* codec;
};

// The function table of a codec, placed at the start of the backend's state. Each member implements the public function
// of the same name. A null `destroy` means there is nothing to free. Null `encodeBound` and `encode` mean the codec
// encodes nothing. Callers use the functions below, never these members.
struct PrCompressCodec {
    void (*PR_NULLABLE destroy)(PrCompressCodec* codec);
    PrStatus (*decoderCreate)(PrCompressCodec* codec, PrCompressFormat format,
                              PrCompressDecoder* PR_NULLABLE* PR_NONNULL out);
    void (*decoderDestroy)(PrCompressCodec* codec, PrCompressDecoder* decoder);
    PrStatus (*decoderRead)(PrCompressCodec* codec, PrCompressDecoder* decoder, PrByteView input,
                            bool final, uint8_t* PR_NULLABLE output, size_t capacity,
                            PrCompressRead* out);
    size_t (*PR_NULLABLE encodeBound)(PrCompressCodec* codec, PrCompressFormat format, size_t size);
    PrStatus (*PR_NULLABLE encode)(PrCompressCodec* codec, PrCompressFormat format, PrCompressLevel level,
                                   PrByteView input, uint8_t* output, size_t* written);
};

// The library's backends of codecs, which the build turns on (README.md), and Default, the first that the build has.
enum {
    PrCompressCodecBackend_Default,
    PrCompressCodecBackend_Builtin,
    PrCompressCodecBackend_Count,
};
typedef uint8_t PrCompressCodecBackend;

// Returns the backend that `name` names, as prCompressCodecBackendName spells it, ignoring case, or Count if it names
// none.
PrCompressCodecBackend prCompressCodecBackendFromName(PrStringView name);

// Returns the name of a backend in lowercase, as a command line spells it.
PrStringView prCompressCodecBackendName(PrCompressCodecBackend backend);

// Returns true if the build has the backend. The build has Default if it has any backend.
bool prCompressCodecBackendIsBuilt(PrCompressCodecBackend backend);

// Creates a codec by the backend. A backend's own header documents what it decodes and encodes. Returns null if the
// build does not have the backend.
PrCompressCodec* PR_NULLABLE prCompressCodecCreate(PrCompressCodecBackend backend);

// Frees the codec. Its decoders must be destroyed first.
void prCompressCodecDestroy(PrCompressCodec* PR_NULLABLE codec);

// Creates a decoder at the start of a stream in `format`. Returns NotSupported for a format the codec does not read. On
// failure, `*out` is null.
PrStatus prCompressDecoderCreate(PrCompressCodec* codec, PrCompressFormat format,
                                 PrCompressDecoder* PR_NULLABLE* PR_NONNULL out);

// Frees the decoder.
void prCompressDecoderDestroy(PrCompressDecoder* PR_NULLABLE decoder);

// Decodes as much of `input` as it can into up to `capacity` bytes at `output`, and writes the result to `*out`.
// `final` means no input follows `input`.
//
// Returns InvalidData if the stream is corrupt, Truncated if the final input ends inside the stream, and NotSupported
// if the stream needs a preset dictionary. After a failure, the output already written stays, and the decoder reads no
// further.
PrStatus prCompressDecoderRead(PrCompressDecoder* decoder, PrByteView input, bool final,
                               uint8_t* PR_NULLABLE output, size_t capacity, PrCompressRead* out);

// Decodes the whole stream in `input` into up to `capacity` bytes at `output`. Returns the same failures as
// prCompressDecoderRead. The progress is Output if the decoded stream does not fit.
PrStatus prCompressCodecDecode(PrCompressCodec* codec, PrCompressFormat format, PrByteView input,
                               uint8_t* PR_NULLABLE output, size_t capacity, PrCompressRead* out);

// Returns the largest number of bytes that prCompressCodecEncode writes for `size` bytes of input in `format`. Returns
// zero if the codec does not encode `format`.
size_t prCompressCodecEncodeBound(PrCompressCodec* codec, PrCompressFormat format, size_t size);

// Encodes the whole of `input` as one stream in `format` at `level`, and sets `*written` to the size of the stream.
// `output` must have room for the bound that prCompressCodecEncodeBound returns. Returns NotSupported if the codec does
// not encode `format`, and then writes nothing.
PrStatus prCompressCodecEncode(PrCompressCodec* codec, PrCompressFormat format, PrCompressLevel level,
                               PrByteView input, uint8_t* output, size_t* written);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
