// The interface that storage backends implement. Callers use the functions of pr/storage.h, which call these. Only
// backends include this header.

#pragma once

#include "pr/base.h"
#include "pr/storage.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The functions of a store's backend, at the start of its state. Each implements the function of pr/storage.h that it
// is named for.
struct PrStorage {
    void (*destroyFn)(PrStorage* storage);
    bool (*readFn)(PrStorage* storage, PrStringView key, PrStorageRecord* out);
    void (*releaseFn)(PrStorage* storage, PrStorageRecord* record);
    bool (*writeFn)(PrStorage* storage, PrStringView key, PrByteView bytes);
    void (*removeFn)(PrStorage* storage, PrStringView key);
    void (*clearFn)(PrStorage* storage);
    uint64_t (*sizeFn)(const PrStorage* storage);
    uint32_t (*countFn)(const PrStorage* storage);
    PrRawString (*keyFn)(const PrStorage* storage, uint32_t position);
};

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
