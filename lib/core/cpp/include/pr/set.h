// Sets: hash sets, which own their items.

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/hash_index.h"
#include "pr/hash_key.h"
#include "pr/span.h"
#include "pr/type_traits.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// A hash set. The items are kept in an array, in the order that they were added, apart from removal, which moves the
// last item into the removed one's place. An item is hashed by hashKey (pr/hash_key.h), and a lookup takes the item's
// HashLookup, such as a view for a String, so that it copies nothing. An item whose lookup is another type converts to
// it.
//
// Adding an item may move every item, and removing one moves the last. A pointer or reference to an item, and a span of
// the items, are valid until the set adds or removes an item.
template <typename Item>
struct Set {
    using Lookup = typename HashLookup<Item>::Type;
    static_assert(isConvertible<const Item&, Lookup>, "an item converts to its lookup (HashLookup)");

    Set() = default;
    Set(const Set& other) : storedItems(other.storedItems) { slots.assign(other.slots); }
    Set(Set&& other) : storedItems(move(other.storedItems)), slots(other.slots) { other.slots = HashIndex{}; }
    Set& operator=(const Set& other) {
        if (this != &other) {
            storedItems = other.storedItems;
            slots.assign(other.slots);
        }
        return *this;
    }
    Set& operator=(Set&& other) {
        if (this != &other) {
            slots.reset();
            storedItems = move(other.storedItems);
            slots = other.slots;
            other.slots = HashIndex{};
        }
        return *this;
    }
    ~Set() { slots.reset(); }

    // The items by position, which are changed only through the functions below.
    Span<const Item> items() const PR_LIFETIMEBOUND { return storedItems.span(); }
    uint32_t count() const { return storedItems.count(); }
    bool isEmpty() const { return storedItems.count() == 0; }

    const Item* PR_NULLABLE begin() const { return storedItems.begin(); }
    const Item* PR_NULLABLE end() const { return storedItems.end(); }

    // Returns the position of `item`, or kHashMissing.
    uint32_t find(const Lookup& item) const {
        return slots.find(hashKey(item), [&](uint32_t at) { return storedItems[at] == item; });
    }

    bool contains(const Lookup& item) const { return find(item) != kHashMissing; }

    // Adds an item made from `item`, or `item` itself, moved. Returns false if the set already holds it.
    bool add(const Lookup& item) {
        if (find(item) != kHashMissing)
            return false;
        slots.insert(hashKey(item), storedItems.count());
        storedItems.emplace(item);
        return true;
    }
    template <typename Moved>
    bool add(Moved&& item) requires(isSame<Moved, Item> && !isSame<Item, Lookup>) {
        if (find(item) != kHashMissing)
            return false;
        slots.insert(hashKey(item), storedItems.count());
        storedItems.emplace(move(item));
        return true;
    }

    // Removes `item`, and moves the last item into its place. Returns false if the set does not hold it.
    bool erase(const Lookup& item) {
        uint32_t at = find(item);
        if (at == kHashMissing)
            return false;
        slots.erase(hashKey(item), at);
        uint32_t last = storedItems.count() - 1;
        if (at != last) {
            slots.relocate(hashKey(storedItems[last]), last, at);
            storedItems[at] = move(storedItems[last]);
        }
        storedItems.pop();
        return true;
    }

    // Removes every item and keeps the storage.
    void clear() {
        storedItems.clear();
        slots.clear();
    }

private:
    Array<Item> storedItems;
    HashIndex slots;
};

} // namespace pr

PR_ASSUME_NONNULL_END
