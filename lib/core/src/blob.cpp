// Blobs.

#include "pr/blob.h"

#include "pr/atomic.h"
#include "pr/mem.h"
#include "pr/memory.h"

using namespace pr;

struct PrBlob {
    Atomic<uint32_t> references;
    PrByteView bytes;
    PrBlobReleaseFn PR_NULLABLE release;   // null for a copy, which follows the blob in its allocation
    void* PR_NULLABLE context;
};

PrBlob* prBlobCreate(PrByteView bytes, PrBlobReleaseFn release, void* PR_NULLABLE context) {
    PrBlob* blob = static_cast<PrBlob*>(prMemoryAllocZeroed(sizeof(PrBlob)));
    blob->references.storeRelaxed(1);
    blob->bytes = bytes;
    blob->release = release;
    blob->context = context;
    return blob;
}

PrBlob* prBlobCopy(PrByteView bytes) {
    // The copy follows the blob in its allocation, so that freeing the blob frees it.
    PrBlob* blob = static_cast<PrBlob*>(prMemoryAllocZeroed(sizeof(PrBlob) + bytes.size));
    blob->references.storeRelaxed(1);
    uint8_t* copy = reinterpret_cast<uint8_t*>(blob + 1);
    if (bytes.size != 0)
        prMemCopy(copy, bytes.data, bytes.size);
    blob->bytes = PrByteView{copy, bytes.size};
    return blob;
}

PrByteView prBlobBytes(const PrBlob* blob) {
    return blob->bytes;
}

void prBlobRetain(PrBlob* blob) {
    blob->references.fetchAdd(1);
}

void prBlobRelease(PrBlob* PR_NULLABLE blob) {
    if (blob == nullptr || blob->references.fetchSub(1) != 1)
        return;
    if (blob->release != nullptr)
        blob->release(blob->context, blob->bytes);
    prMemoryFree(blob);
}
