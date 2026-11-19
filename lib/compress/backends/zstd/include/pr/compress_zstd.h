// The Zstandard codec: lib/zstd (pr/zstd.h) as a member of a codec set.

#pragma once

#include "pr/base.h"
#include "pr/compress.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes and encodes Zstandard. Its decoders accept the reference decoder's default window unless
// they are given a limit (prCompressDecoderSetWindowLimit). The codec has no state of its own, so one codec serves
// every thread.
PrCompressCodec* prCompressZstdCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
