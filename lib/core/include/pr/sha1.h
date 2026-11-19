// SHA-1 digests of bytes.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The number of bytes of a SHA-1 digest.
enum { kPrSha1Bytes = 20 };

// Writes the SHA-1 digest of `bytes` (rfc 3174) to `out`. SHA-1 does not resist collisions, so it serves only the
// protocols that require it, such as the WebSocket handshake.
void prSha1Digest(PrByteView bytes, uint8_t out[PR_NONNULL kPrSha1Bytes]);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
