// Blobs: immutable bytes shared by reference, which whoever made them frees.
//
// A blob holds bytes and what frees them. Whatever makes bytes hands them out as a blob: a file that the platform maps
// (prFileOpen), a resource of the program (prResourceOpen), or a copy. Whatever keeps bytes retains the blob, rather
// than copying them or holding a mapping of its own, and releases it when it is done. The last release frees the bytes
// as their maker asked, so the holder never knows how they were made.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Bytes and what frees them, shared by reference.
typedef struct PrBlob PrBlob;

// Called with `context` and the bytes of a blob once its last reference is released, on the thread that releases it.
typedef void (*PrBlobReleaseFn)(void* PR_NULLABLE context, PrByteView bytes);

// Returns a blob of `bytes`, which it takes: they stay valid and unchanged until `release` is called with `context` and
// the bytes, once the blob's last reference is released. The caller holds the blob's one reference.
PrBlob* prBlobCreate(PrByteView bytes, PrBlobReleaseFn release, void* PR_NULLABLE context);

// Returns a blob of a copy of `bytes`. The caller holds the blob's one reference.
PrBlob* prBlobCopy(PrByteView bytes);

// Returns the blob's bytes, which stay valid while a reference to the blob is held.
PrByteView prBlobBytes(const PrBlob* blob);

// Takes a reference to the blob. May be called from any thread.
void prBlobRetain(PrBlob* blob);

// Releases a reference to the blob. The last one frees the blob and releases its bytes. May be called from any thread.
void prBlobRelease(PrBlob* PR_NULLABLE blob);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
