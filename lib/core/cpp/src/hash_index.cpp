#include "pr/hash_index.h"

#include "pr/memory.h"

namespace pr {
namespace {

// Grows the slots to `size`, a power of two, and reinserts every entry.
void resize(HashIndex* index, uint32_t size) {
    HashSlot* old = index->slots;
    uint32_t oldSize = old == nullptr ? 0 : index->mask + 1;
    index->slots = static_cast<HashSlot*>(prMemoryAlloc(size_t(size) * sizeof(HashSlot)));
    index->mask = size - 1;
    index->clear();
    for (uint32_t i = 0; i < oldSize; ++i) {
        if (old[i].index != kHashMissing)
            index->insert(old[i].hash, old[i].index);
    }
    prMemoryFree(old);
}

// Doubles the slots.
void grow(HashIndex* index) {
    uint32_t oldSize = index->slots == nullptr ? 0 : index->mask + 1;
    PR_ASSERT(oldSize <= 0x40000000u);
    resize(index, oldSize == 0 ? 16 : oldSize * 2);
}

} // namespace

void HashIndex::insert(uint32_t hash, uint32_t index) {
    PR_ASSERT(index != kHashMissing);
    if (slots == nullptr || uint64_t(count + 1) * 10 > uint64_t(mask + 1) * 7)
        grow(this);
    uint32_t slot = hash & mask;
    while (slots[slot].index != kHashMissing)
        slot = (slot + 1) & mask;
    slots[slot] = {hash, index};
    ++count;
}

void HashIndex::reserve(uint32_t total) {
    uint32_t size = slots == nullptr ? 16 : mask + 1;
    while (uint64_t(total) * 10 > uint64_t(size) * 7) {
        PR_ASSERT(size <= 0x40000000u);
        size *= 2;
    }
    if (slots == nullptr || size != mask + 1)
        resize(this, size);
}

void HashIndex::erase(uint32_t hash, uint32_t index) {
    if (slots == nullptr)
        return;
    uint32_t hole = hash & mask;
    while (slots[hole].index != kHashMissing
           && (slots[hole].hash != hash || slots[hole].index != index))
        hole = (hole + 1) & mask;
    if (slots[hole].index == kHashMissing)
        return;
    // Move entries whose probe sequence crossed the hole back into it, so that no tombstones accumulate.
    for (uint32_t at = (hole + 1) & mask; slots[at].index != kHashMissing; at = (at + 1) & mask) {
        uint32_t home = slots[at].hash & mask;
        if (((hole - home) & mask) < ((at - home) & mask)) {
            slots[hole] = slots[at];
            hole = at;
        }
    }
    slots[hole] = {0, kHashMissing};
    --count;
}

void HashIndex::relocate(uint32_t hash, uint32_t from, uint32_t to) {
    PR_ASSERT(slots != nullptr && to != kHashMissing);
    uint32_t slot = hash & mask;
    while (slots[slot].hash != hash || slots[slot].index != from) {
        PR_ASSERT(slots[slot].index != kHashMissing);
        slot = (slot + 1) & mask;
    }
    slots[slot].index = to;
}

void HashIndex::clear() {
    if (slots != nullptr) {
        for (uint32_t i = 0; i <= mask; ++i)
            slots[i] = {0, kHashMissing};
    }
    count = 0;
}

void HashIndex::reset() {
    prMemoryFree(slots);
    slots = nullptr;
    mask = 0;
    count = 0;
}

} // namespace pr
