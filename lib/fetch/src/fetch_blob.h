// Blobs and the stores of their URLs.

#pragma once

#include "pr/array.h"
#include "pr/atomic.h"
#include "pr/fetch_blob.h"
#include "pr/mutex.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Immutable bytes, and a file's name and time.
struct FetchBlob {
    mutable Atomic<uint32_t> references;
    Array<uint8_t> bytes;
    PrString type;
    PrString name;
    double lastModified;
    bool isFile;
};

// An entry of a store.
struct FetchBlobUrl {
    PrString url;
    PrString origin;
    const PrFetchBlob* blob;
};

// A store of blob URLs. `lock` guards `entries`.
struct FetchBlobStore {
    PrMutex lock;
    Array<FetchBlobUrl> entries;
};

} // namespace pr_impl

// C opaque types
struct PrFetchBlob : pr_impl::FetchBlob {};
struct PrFetchBlobStore : pr_impl::FetchBlobStore {};

PR_ASSUME_NONNULL_END
