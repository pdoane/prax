// Hash indexes: open-addressed indexes over positions in their client's storage.

#pragma once

#include "pr/assert.h"
#include "pr/memory.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// The position that a lookup returns when nothing matches, and the position of an empty slot.
constexpr uint32_t kHashMissing = 0xffffffffu;

// A slot of a hash index.
struct HashSlot {
    uint32_t hash;
    uint32_t index;                 // a position in the client's storage, or kHashMissing if the slot is empty
};

// An open-addressed hash index over positions in a client's storage. Keys and values live in that storage, and a lookup
// resolves collisions by comparing against it. The storage may grow without a rehash, as long as positions stay valid.
struct HashIndex {
    HashSlot* PR_NULLABLE slots = nullptr;
    uint32_t mask = 0;
    uint32_t count = 0;

    // Returns the position with hash `hash` for which `equals` returns true, or kHashMissing.
    template <typename Equals>
    uint32_t find(uint32_t hash, Equals equals) const {
        if (slots == nullptr)
            return kHashMissing;
        for (uint32_t slot = hash & mask;; slot = (slot + 1) & mask) {
            const HashSlot& at = slots[slot];
            if (at.index == kHashMissing)
                return kHashMissing;
            if (at.hash == hash && equals(at.index))
                return at.index;
        }
    }

    // Adds the position `index` with hash `hash`. The position must not already be in the index.
    void insert(uint32_t hash, uint32_t index);

    // Makes room for `count` positions in total, so that the slots do not grow again until the index holds more than
    // that many.
    void reserve(uint32_t count);

    // Removes the position `index` with hash `hash`, if the index holds it.
    void erase(uint32_t hash, uint32_t index);

    // Changes the position `from` with hash `hash` to `to`, for a client that moves the entry at `from` to `to`. The
    // index must hold `from` and must not hold `to`.
    void relocate(uint32_t hash, uint32_t from, uint32_t to);

    // Empties the index and keeps its slots.
    void clear();

    // Frees the slots and leaves the index empty.
    void reset();

    // Makes the index a copy of `other`, for a client whose storage was copied with its positions. The slots are copied
    // as they are, with nothing rehashed, and the index's own slots are reused if they are the same size.
    void assign(const HashIndex& other);
};

// A hash index over the entries of a client by a 64-bit key, such as the id of a resource. Removing an entry moves the
// entries after it, so the client rebuilds the index after a removal.
struct KeyIndex {
    HashIndex index;

    // The hash of `key`.
    static uint32_t hashOf(uint64_t key) { return uint32_t(((key ^ (key >> 29)) * 0x9e3779b97f4a7c15ull) >> 32); }

    // Returns the position of the entry whose key, as read by `keyAt`, is `key`, or kHashMissing.
    template <typename KeyAt>
    uint32_t find(uint64_t key, KeyAt keyAt) const {
        return index.find(hashOf(key), [&](uint32_t at) { return keyAt(at) == key; });
    }

    // Adds the entry at `at`, whose key is `key`.
    void insert(uint64_t key, uint32_t at) { index.insert(hashOf(key), at); }

    // Clears the index and indexes the first `count` entries of the client again.
    template <typename KeyAt>
    void rebuild(uint32_t count, KeyAt keyAt) {
        index.clear();
        for (uint32_t at = 0; at < count; ++at)
            insert(keyAt(at), at);
    }

    // Empties the index and keeps its slots.
    void clear() { index.clear(); }

    // Frees the slots and leaves the index empty.
    void reset() { index.reset(); }
};

} // namespace pr

PR_ASSUME_NONNULL_END
