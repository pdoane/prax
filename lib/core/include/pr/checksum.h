// Checksums of bytes, computed incrementally.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Returns `checksum` extended by `bytes`, computed as a CRC-32 with the IEEE polynomial in reflected bit order. A
// checksum starts at zero, and each result is the input to the next call.
uint32_t prCrc32Update(uint32_t checksum, PrByteView bytes);

// Returns `checksum` extended by `bytes`, computed as an Adler-32. A checksum starts at one, and each result is the
// input to the next call.
uint32_t prAdler32Update(uint32_t checksum, PrByteView bytes);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
