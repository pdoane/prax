// Blobs.

#include "pr/blob.h"

#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/file.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"
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

// The progress of a range of a source whose bytes are arriving: the part of the range that has arrived.
PrBlobProgress rangeProgress(void* PR_NULLABLE context) {
    const RangeSource* range = static_cast<const RangeSource*>(context);
    PrBlobProgress inner = prBlobSourceProgress(range->source);
    uint64_t past = inner.available > range->range.offset ? inner.available - range->range.offset : 0;
    inner.available = past < range->range.size ? past : range->range.size;
    if (inner.available == range->range.size)
        inner.complete = true;
    return inner;
}

// The storage of a feed: the bytes appended so far, at the start of `capacity` bytes. A blob of a read holds a
// reference to the storage that it views. A feed that outgrows its storage copies its bytes to larger storage and
// releases the old, which lives while the blobs of earlier reads hold it.
struct FeedStorage {
    Atomic<uint32_t> references;
    uint64_t capacity;
};

uint8_t* storageBytes(FeedStorage* storage) {
    return reinterpret_cast<uint8_t*>(storage + 1);
}

void storageRelease(FeedStorage* PR_NULLABLE storage) {
    if (storage != nullptr && storage->references.fetchSub(1) == 1)
        prMemoryFree(storage);
}

// What a feed and its source share. `waiting` holds the reads whose ranges have not arrived.
struct FeedState {
    Atomic<uint32_t> references;
    PrMutex lock;
    FeedStorage* PR_NULLABLE storage;
    uint64_t length;
    bool ended;
    PrStatus status;
    RawArray<PrBlobRead*> waiting;
    PrNotifier* notifier;
    Atomic<bool> writing;               // a thread is appending, ending or failing the feed
};

// Marks the start of a write to the feed: one thread at a time appends, ends or fails a feed, which this asserts.
void beginWrite(FeedState& state) {
    const bool overlapped = state.writing.exchange(true);
    PR_ASSERT(!overlapped);
    (void)overlapped;
}

void endWrite(FeedState& state) {
    state.writing.store(false);
}

void feedRelease(FeedState* state) {
    if (state->references.fetchSub(1) != 1)
        return;
    PR_ASSERT(state->waiting.count == 0);
    storageRelease(state->storage);
    state->waiting.reset();
    prNotifierRelease(state->notifier);
    prMutexReset(&state->lock);
    prMemoryFree(state);
}

// Returns true if the read's range has arrived.
bool feedHolds(const FeedState& state, const PrBlobRead* read) {
    PrBlobRange range = prBlobReadRange(read);
    return range.size != kPrBlobSizeUnknown && range.offset + range.size <= state.length;
}

// Returns true if `read` can complete now: its range has arrived, or the feed failed or ended.
bool feedSatisfies(const FeedState& state, const PrBlobRead* read) {
    return state.status != PrStatus_Ok || state.ended || feedHolds(state, read);
}

// Returns a blob of the feed's bytes, which holds a reference to their storage. Called with the lock held.
PrBlob* feedBlob(FeedState& state) {
    if (state.storage == nullptr)
        return prBlobCopy(PrByteView{});
    state.storage->references.fetchAdd(1);
    return prBlobCreate(PrByteView{storageBytes(state.storage), size_t(state.length)},
        [](void* PR_NULLABLE context, PrByteView) { storageRelease(static_cast<FeedStorage*>(context)); },
        state.storage);
}

// Completes `reads`, which the feed satisfies, with the bytes in `blob`, or with `status` for a read whose range had
// not arrived when the feed failed. Called without the lock.
void feedComplete(Span<PrBlobRead*> reads, PrStatus status, PrBlob* blob) {
    for (PrBlobRead* read : reads) {
        PrBlobRange range = prBlobReadRange(read);
        bool held = range.size != kPrBlobSizeUnknown && range.offset + range.size <= prBlobBytes(blob).size;
        if (status != PrStatus_Ok && !held) {
            prBlobReadComplete(read, status, nullptr, 0);
            continue;
        }
        prBlobRetain(blob);
        completeWith(read, blob);
    }
}

// Takes the waiting reads that the feed now satisfies, in order. Called with the lock held.
void takeSatisfied(FeedState& state, RawArray<PrBlobRead*>* reads) {
    uint32_t kept = 0;
    for (PrBlobRead* read : state.waiting.span()) {
        if (feedSatisfies(state, read))
            reads->push(read);
        else
            state.waiting.data[kept++] = read;
    }
    state.waiting.count = kept;
}

// Completes the reads that the feed satisfies and notifies the feed's notifier. Called with the lock held, which it
// releases.
void feedSettle(FeedState& state) {
    RawArray<PrBlobRead*> reads;
    takeSatisfied(state, &reads);
    PrStatus status = state.status;
    PrBlob* blob = reads.count != 0 ? feedBlob(state) : nullptr;
    prMutexUnlock(&state.lock);
    if (blob != nullptr)
        feedComplete(reads.span(), status, blob);
    prBlobRelease(blob);
    reads.reset();
    prNotifierNotify(state.notifier);
}

void startFeed(void* PR_NULLABLE context, PrBlobRead* read) {
    FeedState& state = *static_cast<FeedState*>(context);
    prMutexLock(&state.lock);
    if (!feedSatisfies(state, read)) {
        state.waiting.push(read);
        prMutexUnlock(&state.lock);
        return;
    }
    PrStatus status = state.status;
    PrBlob* blob = feedBlob(state);
    prMutexUnlock(&state.lock);
    PrBlobRead* reads[] = {read};
    feedComplete(Span(reads, 1), status, blob);
    prBlobRelease(blob);
}

PrBlobProgress feedProgress(void* PR_NULLABLE context) {
    FeedState& state = *static_cast<FeedState*>(context);
    prMutexLock(&state.lock);
    PrBlobProgress progress{
        .available = state.length,
        .status = state.status,
        .complete = state.ended || state.status != PrStatus_Ok,
    };
    prMutexUnlock(&state.lock);
    return progress;
}

// Appends `bytes` to the feed, completes the reads that they satisfy and notifies. Bytes after the end or a failure are
// ignored.
void appendBytes(FeedState& state, PrByteView bytes) {
    prMutexLock(&state.lock);
    if (state.ended || state.status != PrStatus_Ok || bytes.size == 0) {
        prMutexUnlock(&state.lock);
        return;
    }
    uint64_t needed = state.length + bytes.size;
    if (state.storage == nullptr || state.storage->capacity < needed) {
        // The storage at least doubles, so the copies of a resource cost a few times its size in all.
        uint64_t capacity = state.storage != nullptr ? state.storage->capacity * 2 : 4096;
        while (capacity < needed)
            capacity *= 2;
        FeedStorage* grown = static_cast<FeedStorage*>(prMemoryAlloc(sizeof(FeedStorage) + size_t(capacity)));
        grown->references.storeRelaxed(1);
        grown->capacity = capacity;
        if (state.length != 0)
            prMemCopy(storageBytes(grown), storageBytes(state.storage), size_t(state.length));
        storageRelease(state.storage);
        state.storage = grown;
    }
    // The new bytes go past the length that every earlier blob views, so those blobs never change.
    prMemCopy(storageBytes(state.storage) + state.length, bytes.data, bytes.size);
    state.length = needed;
    feedSettle(state);
}

// Ends the feed's bytes with `status`: Ok for the end, or a failure.
void endBytes(FeedState& state, PrStatus status) {
    prMutexLock(&state.lock);
    if (state.ended || state.status != PrStatus_Ok) {
        prMutexUnlock(&state.lock);
        return;
    }
    if (status == PrStatus_Ok)
        state.ended = true;
    else
        state.status = status;
    feedSettle(state);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

struct PrBlobSource {
    Atomic<uint32_t> references;
    PrBlobSourceDesc desc;
};

// A feed: its state, which its source shares, and the source.
struct PrBlobFeed {
    FeedState* state;
    PrBlobSource* source;
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
    if (desc->notifier != nullptr)
        prNotifierRetain(desc->notifier);
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
    // A range of arriving bytes arrives as they do.
    PrBlobSourceDesc desc{
        .start = startRange,
        .release = releaseRange,
        .context = held,
        .size = range.size,
        .progress = source->desc.progress != nullptr ? rangeProgress : nullptr,
        .notifier = source->desc.notifier,
    };
    return prBlobSourceCreate(&desc);
}

void prBlobSourceRetain(const PrBlobSource* source) {
    const_cast<PrBlobSource*>(source)->references.fetchAdd(1);
}

void prBlobSourceRelease(const PrBlobSource* PR_NULLABLE source) {
    PrBlobSource* held = const_cast<PrBlobSource*>(source);
    if (held == nullptr || held->references.fetchSub(1) != 1)
        return;
    PrNotifier* notifier = held->desc.notifier;
    if (held->desc.release != nullptr)
        held->desc.release(held->desc.context);
    prNotifierRelease(notifier);
    prMemoryFree(held);
}

uint64_t prBlobSourceSize(const PrBlobSource* source) {
    return source->desc.size;
}

PrBlobProgress prBlobSourceProgress(const PrBlobSource* source) {
    if (source->desc.progress != nullptr)
        return source->desc.progress(source->desc.context);
    return PrBlobProgress{.available = source->desc.size, .complete = true};
}

PrNotifier* PR_NULLABLE prBlobSourceNotifier(const PrBlobSource* source) {
    return source->desc.notifier;
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

PrBlobFeed* prBlobFeedCreate(uint64_t size) {
    FeedState* state = static_cast<FeedState*>(prMemoryAllocZeroed(sizeof(FeedState)));
    // The feed and its source each hold a reference.
    state->references.storeRelaxed(2);
    prMutexInit(&state->lock);
    state->notifier = prNotifierCreate();
    PrBlobSourceDesc desc{
        .start = startFeed,
        .release = [](void* PR_NULLABLE context) { feedRelease(static_cast<FeedState*>(context)); },
        .context = state,
        .size = size,
        .progress = feedProgress,
        .notifier = state->notifier,
    };
    PrBlobFeed* feed = static_cast<PrBlobFeed*>(prMemoryAlloc(sizeof(PrBlobFeed)));
    *feed = PrBlobFeed{state, prBlobSourceCreate(&desc)};
    return feed;
}

void prBlobFeedDestroy(PrBlobFeed* PR_NULLABLE feed) {
    if (feed == nullptr)
        return;
    prBlobFeedFail(feed, PrStatus_Lost);
    feedRelease(feed->state);
    prBlobSourceRelease(feed->source);
    prMemoryFree(feed);
}

const PrBlobSource* prBlobFeedSource(const PrBlobFeed* feed) {
    return feed->source;
}

void prBlobFeedAppend(PrBlobFeed* feed, PrByteView bytes) {
    beginWrite(*feed->state);
    appendBytes(*feed->state, bytes);
    endWrite(*feed->state);
}

void prBlobFeedEnd(PrBlobFeed* feed) {
    beginWrite(*feed->state);
    endBytes(*feed->state, PrStatus_Ok);
    endWrite(*feed->state);
}

void prBlobFeedFail(PrBlobFeed* feed, PrStatus status) {
    PR_ASSERT(status != PrStatus_Ok);
    beginWrite(*feed->state);
    endBytes(*feed->state, status);
    endWrite(*feed->state);
}
