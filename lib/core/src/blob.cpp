// Blobs.

#include "pr/blob.h"

#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/file.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/thread.h"

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

namespace pr_impl {

namespace {

void releaseSlice(void* PR_NULLABLE context, PrByteView) {
    prBlobRelease(static_cast<PrBlob*>(context));
}

// Returns the part of `bytes` that `range` names, clamped to `bytes`.
PrByteView clampRange(PrByteView bytes, PrBlobRange range) {
    uint64_t offset = range.offset < bytes.size ? range.offset : bytes.size;
    uint64_t rest = bytes.size - offset;
    uint64_t size = range.size < rest ? range.size : rest;
    return PrByteView{bytes.data + offset, size_t(size)};
}

// The source of a file. The path of the file follows the source in the same allocation.
struct FileSource {
    size_t pathSize;
};

// Completes a read with the part of `blob` that the read asks for. Takes over the caller's reference to `blob`.
void completeWith(PrBlobRead* read, PrBlob* blob) {
    PrBlobRange range = prBlobReadRange(read);
    if (range.offset == 0 && range.size >= prBlobBytes(blob).size) {
        prBlobReadComplete(read, PrStatus_Ok, blob, 0);
        return;
    }
    PrBlob* slice = prBlobSlice(blob, range.offset, range.size);
    prBlobRelease(blob);
    prBlobReadComplete(read, PrStatus_Ok, slice, 0);
}

void startBlob(void* PR_NULLABLE context, PrBlobRead* read) {
    PrBlob* blob = static_cast<PrBlob*>(context);
    prBlobRetain(blob);
    completeWith(read, blob);
}

void releaseBlob(void* PR_NULLABLE context) {
    prBlobRelease(static_cast<PrBlob*>(context));
}

void startFile(void* PR_NULLABLE context, PrBlobRead* read) {
    const FileSource* file = static_cast<const FileSource*>(context);
    PrBlob* blob = prFileOpen(PrStringView{reinterpret_cast<const char*>(file + 1), file->pathSize});
    if (blob == nullptr)
        prBlobReadComplete(read, PrStatus_Io, nullptr, 0);
    else
        completeWith(read, blob);
}

void releaseFile(void* PR_NULLABLE context) {
    prMemoryFree(context);
}

// A source of a range of another source.
struct RangeSource {
    const PrBlobSource* source;
    PrBlobRange range;
};

// The completion callback of the read that a range source makes of its underlying source. Completes the outer read.
void rangeDone(void* PR_NULLABLE context, PrBlobRead* inner) {
    PrBlobRead* outer = static_cast<PrBlobRead*>(context);
    PrStatus status = prBlobReadStatus(inner);
    PrBlob* blob = prBlobReadBlob(inner);
    uint64_t count = prBlobReadByteCount(inner);
    if (blob != nullptr)
        prBlobRetain(blob);
    prBlobReadDestroy(inner);
    prBlobReadComplete(outer, status, blob, count);
}

void startRange(void* PR_NULLABLE context, PrBlobRead* read) {
    const RangeSource* range = static_cast<const RangeSource*>(context);
    PrBlobRange asked = prBlobReadRange(read);
    uint64_t offset = asked.offset < range->range.size ? asked.offset : range->range.size;
    uint64_t rest = range->range.size - offset;
    PrBlobReadDesc desc{
        .range = {range->range.offset + offset, asked.size < rest ? asked.size : rest},
        .storage = prBlobReadStorage(read),
        .done = rangeDone,
        .context = read,
    };
    prBlobReadCreate(range->source, &desc);
}

void releaseRange(void* PR_NULLABLE context) {
    RangeSource* range = static_cast<RangeSource*>(context);
    prBlobSourceRelease(range->source);
    prMemoryFree(range);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

struct PrBlobSource {
    Atomic<uint32_t> references;
    PrBlobSourceDesc desc;
};

// A read: what it asks for, and once `done` is set, how it went. The lock and the condition serve its waits.
struct PrBlobRead {
    const PrBlobSource* source;
    PrBlobReadDesc desc;
    PrStatus status;
    PrBlob* PR_NULLABLE blob;
    uint64_t byteCount;
    Atomic<bool> done;
    PrMutex lock;
    PrCondition completed;
};

PrBlob* prBlobSlice(PrBlob* blob, uint64_t offset, uint64_t size) {
    prBlobRetain(blob);
    return prBlobCreate(clampRange(prBlobBytes(blob), PrBlobRange{offset, size}), releaseSlice, blob);
}

PrBlobRange prBlobRangeWhole(void) {
    return PrBlobRange{0, kPrBlobSizeUnknown};
}

PrBlobSource* prBlobSourceCreate(const PrBlobSourceDesc* desc) {
    PrBlobSource* source = static_cast<PrBlobSource*>(prMemoryAllocZeroed(sizeof(PrBlobSource)));
    source->references.storeRelaxed(1);
    source->desc = *desc;
    return source;
}

PrBlobSource* prBlobSourceCreateBlob(PrBlob* blob) {
    prBlobRetain(blob);
    PrBlobSourceDesc desc{.start = startBlob, .release = releaseBlob, .context = blob, .size = prBlobBytes(blob).size};
    return prBlobSourceCreate(&desc);
}

PrBlobSource* prBlobSourceCreateFile(PrStringView path) {
    FileSource* file = static_cast<FileSource*>(prMemoryAlloc(sizeof(FileSource) + path.size));
    file->pathSize = path.size;
    if (path.size != 0)
        prMemCopy(file + 1, path.data, path.size);
    PrBlobSourceDesc desc{.start = startFile, .release = releaseFile, .context = file, .size = kPrBlobSizeUnknown};
    return prBlobSourceCreate(&desc);
}

PrBlobSource* prBlobSourceCreateRange(const PrBlobSource* source, PrBlobRange range) {
    uint64_t size = source->desc.size;
    if (size != kPrBlobSizeUnknown) {
        uint64_t offset = range.offset < size ? range.offset : size;
        range = PrBlobRange{offset, range.size < size - offset ? range.size : size - offset};
    }
    RangeSource* held = static_cast<RangeSource*>(prMemoryAlloc(sizeof(RangeSource)));
    prBlobSourceRetain(source);
    *held = RangeSource{source, range};
    PrBlobSourceDesc desc{.start = startRange, .release = releaseRange, .context = held, .size = range.size};
    return prBlobSourceCreate(&desc);
}

void prBlobSourceRetain(const PrBlobSource* source) {
    const_cast<PrBlobSource*>(source)->references.fetchAdd(1);
}

void prBlobSourceRelease(const PrBlobSource* PR_NULLABLE source) {
    PrBlobSource* held = const_cast<PrBlobSource*>(source);
    if (held == nullptr || held->references.fetchSub(1) != 1)
        return;
    if (held->desc.release != nullptr)
        held->desc.release(held->desc.context);
    prMemoryFree(held);
}

uint64_t prBlobSourceSize(const PrBlobSource* source) {
    return source->desc.size;
}

PrBlobReadDesc prBlobReadDescDefault(void) {
    return PrBlobReadDesc{.range = prBlobRangeWhole()};
}

PrBlobRead* prBlobReadCreate(const PrBlobSource* source, const PrBlobReadDesc* desc) {
    PR_ASSERT(desc->storage == nullptr || desc->range.size != kPrBlobSizeUnknown);
    PrBlobRead* read = static_cast<PrBlobRead*>(prMemoryAllocZeroed(sizeof(PrBlobRead)));
    prBlobSourceRetain(source);
    read->source = source;
    read->desc = *desc;
    read->done.store(false);
    prMutexInit(&read->lock);
    prConditionInit(&read->completed);
    source->desc.start(source->desc.context, read);
    return read;
}

bool prBlobReadIsDone(const PrBlobRead* read) {
    return read->done.load();
}

void prBlobReadWait(PrBlobRead* read) {
    if (read->done.load())
        return;
    prMutexLock(&read->lock);
    while (!read->done.load())
        prConditionWait(&read->completed, &read->lock);
    prMutexUnlock(&read->lock);
}

PrStatus prBlobReadStatus(const PrBlobRead* read) {
    PR_ASSERT(read->done.load());
    return read->status;
}

PrBlob* PR_NULLABLE prBlobReadBlob(const PrBlobRead* read) {
    PR_ASSERT(read->done.load());
    return read->blob;
}

uint64_t prBlobReadByteCount(const PrBlobRead* read) {
    PR_ASSERT(read->done.load());
    return read->byteCount;
}

void prBlobReadDestroy(PrBlobRead* PR_NULLABLE read) {
    if (read == nullptr)
        return;
    // A provider may still hold the lock while it wakes the waiters.
    prMutexLock(&read->lock);
    PR_ASSERT(read->done.load());
    prMutexUnlock(&read->lock);
    prBlobRelease(read->blob);
    prBlobSourceRelease(read->source);
    prMutexReset(&read->lock);
    prConditionReset(&read->completed);
    prMemoryFree(read);
}

PrStatus prBlobSourceRead(const PrBlobSource* source, PrBlobRange range, PrBlob* PR_NULLABLE* PR_NONNULL out) {
    PrBlobReadDesc desc = prBlobReadDescDefault();
    desc.range = range;
    PrBlobRead* read = prBlobReadCreate(source, &desc);
    prBlobReadWait(read);
    PrStatus status = prBlobReadStatus(read);
    *out = status == PrStatus_Ok ? read->blob : nullptr;
    if (*out != nullptr)
        prBlobRetain(*out);
    prBlobReadDestroy(read);
    return status;
}

PrBlobRange prBlobReadRange(const PrBlobRead* read) {
    return read->desc.range;
}

uint8_t* PR_NULLABLE prBlobReadStorage(const PrBlobRead* read) {
    return read->desc.storage;
}

void prBlobReadComplete(PrBlobRead* read, PrStatus status, PrBlob* PR_NULLABLE bytes, uint64_t byteCount) {
    PR_ASSERT(!read->done.load());
    if (status != PrStatus_Ok) {
        prBlobRelease(bytes);
        bytes = nullptr;
        byteCount = 0;
    } else if (bytes != nullptr) {
        PrByteView view = clampRange(prBlobBytes(bytes), PrBlobRange{0, read->desc.range.size});
        byteCount = view.size;
        if (read->desc.storage != nullptr) {
            if (view.size != 0)
                prMemCopy(read->desc.storage, view.data, view.size);
            prBlobRelease(bytes);
            bytes = nullptr;
        }
    }
    read->status = status;
    read->blob = bytes;
    read->byteCount = byteCount;
    // The callback may destroy the read, so the waiters are woken first. They are woken under the lock, which
    // destroying the read takes.
    PrBlobReadDoneFn done = read->desc.done;
    void* context = read->desc.context;
    prMutexLock(&read->lock);
    read->done.store(true);
    prConditionBroadcast(&read->completed);
    prMutexUnlock(&read->lock);
    if (done != nullptr)
        done(context, read);
}
