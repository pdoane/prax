// Blobs and the stores of their URLs.

#pragma once

#include "pr/atomic.h"
#include "pr/fetch_blob.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Immutable bytes, and a file's name and time.
struct FetchBlob {
    mutable Atomic<uint32_t> references;
    RawArray<uint8_t> bytes;
    PrRawString type;
    PrRawString name;
    double lastModified;
    bool isFile;
};

// An entry of a store.
struct FetchBlobUrl {
    PrRawString url;
    PrRawString origin;
    const PrFetchBlob* blob;
};

// A store of blob URLs. `lock` guards `entries`.
struct FetchBlobStore {
    PrMutex lock;
    RawArray<FetchBlobUrl> entries;
};

} // namespace pr_impl

// C opaque types
struct PrFetchBlob : pr_impl::FetchBlob {};
struct PrFetchBlobStore : pr_impl::FetchBlobStore {};

PR_ASSUME_NONNULL_END
