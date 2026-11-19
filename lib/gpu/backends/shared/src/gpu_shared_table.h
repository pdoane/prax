// The tables in which every GPU backend keeps its objects by id: its buffers and textures, its samplers, and its
// pipelines.

#pragma once

#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/base.h"
#include "pr/memory.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The records of a backend's objects of one kind by id, and the references to them. A device allocates each table whole
// when it is created, for the number of objects that its options allow (PrGpuDeviceOptions), so a record never moves.
// The ids run from `firstId`, which is never zero, so that zero stays no object.
//
// The references are a structure of arrays beside the records. A count is the number of references past the one that
// creation gives, so a newly taken id's count is zero and no creation sets one.
//
// Threads: taking and freeing ids are thread-safe, under the table's mutex, which nothing else takes. Retaining and
// releasing are thread-safe, take no lock and touch no record. A record belongs to the code that took its id until the
// id is freed, and the backend orders the reads of a record after its writes, as it publishes the object's id.
template <typename T>
struct GpuSharedTable {
    T* PR_NULLABLE records;                     // by id less `firstId`
    Atomic<uint32_t>* PR_NULLABLE references;   // by id less `firstId`
    PrMutex mutex;                              // guards `freeIds` and `nextId`
    RawArray<uint32_t> freeIds;
    uint32_t firstId;
    uint32_t count;             // the number of ids
    uint32_t nextId;            // the lowest id that was never taken

    // Allocates the records and counts of `idCount` ids from `first`.
    void init(uint32_t first, uint32_t idCount) {
        PR_ASSERT(first != 0);
        records = static_cast<T*>(prMemoryAllocZeroed(size_t(idCount) * sizeof(T)));
        references = static_cast<Atomic<uint32_t>*>(prMemoryAllocZeroed(size_t(idCount) * sizeof(Atomic<uint32_t>)));
        prMutexInit(&mutex);
        firstId = first;
        count = idCount;
        nextId = first;
    }

    // Frees the table, or does nothing to one that was never initialized.
    void reset() {
        if (records != nullptr)
            prMutexReset(&mutex);
        prMemoryFree(records);
        prMemoryFree(references);
        freeIds.reset();
        *this = GpuSharedTable{};
    }

    // Returns true if `id` is one of the table's ids.
    bool contains(uint32_t id) const {
        return id >= firstId && id - firstId < count;
    }

    // Takes a free id, whose record is zeroed, or returns zero if every id is taken.
    uint32_t take() {
        uint32_t id = 0;
        prMutexLock(&mutex);
        if (freeIds.count != 0) {
            id = freeIds.last();
            freeIds.pop();
        } else if (nextId - firstId < count) {
            id = nextId++;
        }
        prMutexUnlock(&mutex);
        if (id != 0)
            records[id - firstId] = T{};
        return id;
    }

    // Frees `id`, whose object the backend destroyed, for a later take.
    void free(uint32_t id) {
        PR_ASSERT(contains(id));
        prMutexLock(&mutex);
        PR_ASSERT(id < nextId);
        freeIds.push(id);
        prMutexUnlock(&mutex);
    }

    T& operator[](uint32_t id) {
        PR_ASSERT(contains(id));
        return records[id - firstId];
    }

    const T& operator[](uint32_t id) const {
        PR_ASSERT(contains(id));
        return records[id - firstId];
    }

    void retain(uint32_t id) {
        PR_ASSERT(contains(id));
        references[id - firstId].fetchAdd(1);
    }

    // Drops a reference to `id`. Returns true if it was the last, which the backend then retires.
    bool release(uint32_t id) {
        PR_ASSERT(contains(id));
        Atomic<uint32_t>& held = references[id - firstId];
        uint32_t extra = held.load();
        while (extra != 0) {
            if (held.cas(extra, extra - 1))
                return false;
        }
        return true;
    }
};

} // namespace pr_impl

PR_ASSUME_NONNULL_END
