// A reference to a blob (pr/blob.h), which it releases at the end of its life.

#pragma once

#include "pr/blob.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Holds one reference to a blob, or none. Copying takes another reference, and moving passes the reference on. It
// converts to the PrBlob* that the C API takes, which retains what it keeps, so a font made from a BlobRef is the only
// holder of the bytes once the BlobRef goes.
struct BlobRef {
    BlobRef() = default;
    BlobRef(const BlobRef& other) : blob(other.blob) {
        if (blob != nullptr)
            prBlobRetain(blob);
    }
    BlobRef(BlobRef&& other) : blob(other.blob) { other.blob = nullptr; }

    BlobRef& operator=(BlobRef other) {
        PrBlob* PR_NULLABLE held = blob;
        blob = other.blob;
        other.blob = held;
        return *this;
    }

    ~BlobRef() { prBlobRelease(blob); }

    // Returns a reference that takes `owned`, the caller's reference, which may be null.
    static BlobRef adopt(PrBlob* PR_NULLABLE owned) {
        BlobRef out;
        out.blob = owned;
        return out;
    }

    // The blob, or null if there is none.
    PrBlob* PR_NULLABLE get() const { return blob; }
    operator PrBlob* PR_NULLABLE() const { return blob; }
    explicit operator bool() const { return blob != nullptr; }

    // The blob's bytes, or the empty view if there is no blob.
    PrByteView bytes() const { return blob != nullptr ? prBlobBytes(blob) : PrByteView{}; }

private:
    PrBlob* PR_NULLABLE blob = nullptr;
};

} // namespace pr

PR_ASSUME_NONNULL_END
