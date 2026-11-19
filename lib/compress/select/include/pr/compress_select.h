// The selector for lib/compress's codecs: the choice of the backend of each bare format, and a set of every format for
// a program that wants them all. Programs and composition libraries link it. A program that wants only some formats
// builds its set from their codecs' own constructors instead, and carries only those.

#pragma once

#include "pr/backend.h"
#include "pr/base.h"
#include "pr/compress.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The backends of a bare format.
enum {
    PrCompressCodecBackend_Default,
    PrCompressCodecBackend_Builtin, // Prax's own (pr/compress_deflate.h, pr/compress_brotli.h, pr/compress_lzfse.h)
    PrCompressCodecBackend_Apple,   // Apple's Compression library (pr/compress_apple.h)
    PrCompressCodecBackend_Count,
};
typedef uint8_t PrCompressCodecBackend;

// Returns the descriptor of the codecs of the bare `format`: Deflate, Brotli, LZFSE, LZ4 or Zstandard. The environment
// chooses one with PR_BACKEND_COMPRESS_<FORMAT>, such as PR_BACKEND_COMPRESS_DEFLATE.
const PrBackendInterface* prCompressCodecSelectInterface(PrCompressFormat format);

// Creates a codec of the bare `format` with `backend`. Default is the backend that the format's variable chooses, or
// else Prax's own, so that a program codes each format the same way on every platform. Returns null if the build does
// not include the backend, or the backend does not code the format, as Apple's codes no LZ4 or Zstandard. For Default,
// it also returns null if the variable is not valid (prBackendFromEnvironment).
PrCompressCodec* PR_NULLABLE prCompressCodecCreate(PrCompressCodecBackend backend, PrCompressFormat format);

// Creates a set of a codec of each bare format, with its Default backend, leaving out a format that no backend of the
// build codes. Returns null if a variable is not valid.
PrCompressCodecSet* PR_NULLABLE prCompressCodecSetCreateDefault(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
