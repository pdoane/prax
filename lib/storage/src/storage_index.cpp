// The index of a store's records, by key and by their last use, which the backends share.

#include "storage/src/storage_index.h"

#include "pr/hash.h"
#include "pr/mem.h"
#include "pr/memory.h"

namespace pr_impl {

namespace {

uint32_t hashOf(PrStringView key) {
    return uint32_t(prHashBytes(key.data, key.size, 0));
}

} // namespace

StorageBuffer* storageBufferCreate(PrByteView bytes) {
    StorageBuffer* buffer = static_cast<StorageBuffer*>(prMemoryAlloc(sizeof(StorageBuffer) + bytes.size));
    buffer->references.storeRelaxed(1);
    buffer->size = bytes.size;
    if (bytes.size != 0)
        prMemCopy(buffer->bytes(), bytes.data, bytes.size);
    return buffer;
}

void storageBufferRetain(StorageBuffer* buffer) {
    buffer->references.fetchAddRelaxed(1);
}

void storageBufferRelease(StorageBuffer* PR_NULLABLE buffer) {
    if (buffer != nullptr && buffer->references.fetchSub(1) == 1)
        prMemoryFree(buffer);
}

uint32_t storageIndexFind(const StorageIndex* index, PrStringView key) {
    return index->hashes.find(hashOf(key), [&](uint32_t position) {
        return prRawStringView(&index->entries.data[position].key) == key;
    });
}

uint32_t storageIndexInsert(StorageIndex* index, PrStringView key, uint64_t size, uint64_t used) {
    uint32_t position = index->entries.count;
    index->entries.push(StorageEntry{prRawStringCopy(key), size, used, 0, nullptr});
    index->hashes.insert(hashOf(key), position);
    index->total += key.size + size;
    return position;
}

void storageIndexResize(StorageIndex* index, uint32_t position, uint64_t size) {
    StorageEntry& entry = index->entries.data[position];
    index->total = index->total - entry.size + size;
    entry.size = size;
}

void storageIndexRemove(StorageIndex* index, uint32_t position) {
    StorageEntry& entry = index->entries.data[position];
    index->total -= prRawStringView(&entry.key).size + entry.size;
    index->hashes.erase(hashOf(prRawStringView(&entry.key)), position);
    prRawStringReset(&entry.key);
    uint32_t last = index->entries.count - 1;
    if (position != last) {
        StorageEntry& moved = index->entries.data[last];
        index->hashes.relocate(hashOf(prRawStringView(&moved.key)), last, position);
        index->entries.data[position] = moved;
    }
    index->entries.pop();
}

uint32_t storageIndexOldest(const StorageIndex* index) {
    uint32_t oldest = kHashMissing;
    for (uint32_t i = 0; i < index->entries.count; ++i) {
        if (oldest == kHashMissing || index->entries.data[i].used < index->entries.data[oldest].used)
            oldest = i;
    }
    return oldest;
}

void storageIndexReset(StorageIndex* index) {
    for (uint32_t i = 0; i < index->entries.count; ++i)
        prRawStringReset(&index->entries.data[i].key);
    index->entries.reset();
    index->hashes.reset();
    index->total = 0;
}

} // namespace pr_impl
