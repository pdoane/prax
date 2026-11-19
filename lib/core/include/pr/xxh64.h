// XXH64, xxHash's 64-bit hash of bytes, whole or as they arrive in pieces.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The state of an XXH64 hash (xxHash's 64-bit hash, whose low 32 bits are a Zstandard frame's checksum) of bytes that
// arrive in pieces: the four lanes over each stripe of 32 bytes, the count of bytes added, and those of a stripe that
// has arrived in part.
typedef struct PrXxh64 {
    uint64_t lanes[4];
    uint64_t total;
    uint32_t held;
    uint8_t stripe[32];
} PrXxh64;

// Returns the state of a hash of no bytes with `seed`.
PrXxh64 prXxh64Start(uint64_t seed);

// Adds `bytes` to the hash.
void prXxh64Update(PrXxh64* hash, PrByteView bytes);

// Returns the hash of the bytes added.
uint64_t prXxh64Digest(const PrXxh64* hash);

// Returns the XXH64 of `bytes` with `seed`.
uint64_t prXxh64(PrByteView bytes, uint64_t seed);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
