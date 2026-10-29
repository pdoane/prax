// The built-in compression codec.

#pragma once

#include "pr/base.h"
#include "pr/compress.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes every PrCompressFormat and encodes the Deflate formats. The codec has no state of its
// own, so one codec serves every thread.
PrCompressCodec* prCompressBuiltinCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
