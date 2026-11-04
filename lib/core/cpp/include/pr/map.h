// Maps: hash tables from keys to values, which own both.

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/hash_index.h"
#include "pr/hash_key.h"
#include "pr/span.h"
#include "pr/type_traits.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// An entry of a map, as iteration gives it: `for (auto [key, value] : map.entries())`. The key is read-only, since
// changing it would leave the entry filed under the wrong hash.
template <typename Key, typename Value>
struct MapEntry {
    const Key& key;
    Value& value;
};

// The entries of a map in order, for a range-based for loop. Valid until the map adds or removes an entry.
template <typename Key, typename Value>
struct MapEntries {
    struct Iterator {
        const Key* PR_NULLABLE key;
        Value* PR_NULLABLE value;

        MapEntry<Key, Value> operator*() const { return MapEntry<Key, Value>{*key, *value}; }
        Iterator& operator++() {
            ++key;
            ++value;
            return *this;
        }
        bool operator==(const Iterator& other) const { return key == other.key; }
    };

    const Key* PR_NULLABLE keys;
    Value* PR_NULLABLE values;
    uint32_t count;

    Iterator begin() const { return Iterator{keys, values}; }
    Iterator end() const { return Iterator{keys + count, values + count}; }
};

// A hash table from keys to values. The entries are kept in two arrays, of keys and of values, in the order that they
// were added, apart from removal, which moves the last entry into the removed one's place. A key is hashed by hashKey
// (pr/hash_key.h), and a lookup takes the key's HashLookup, such as a view for a String, so that it copies nothing. A
// key whose lookup is another type converts to it.
//
// Adding an entry may move every key and value, and removing one moves the last. A pointer or reference into the map,
// such as get() or operator[] returns, and a span of its keys or values, are valid until the map adds or removes an
// entry.
template <typename Key, typename Value>
struct Map {
    using Lookup = typename HashLookup<Key>::Type;
    static_assert(isConvertible<const Key&, Lookup>, "a key converts to its lookup (HashLookup)");

    Map() = default;
    Map(const Map& other) : storedKeys(other.storedKeys), storedValues(other.storedValues) { slots.assign(other.slots); }
    Map(Map&& other) : storedKeys(move(other.storedKeys)), storedValues(move(other.storedValues)), slots(other.slots) {
        other.slots = HashIndex{};
    }
    Map& operator=(const Map& other) {
        if (this != &other) {
            storedKeys = other.storedKeys;
            storedValues = other.storedValues;
            slots.assign(other.slots);
        }
        return *this;
    }
    Map& operator=(Map&& other) {
        if (this != &other) {
            slots.reset();
            storedKeys = move(other.storedKeys);
            storedValues = move(other.storedValues);
            slots = other.slots;
            other.slots = HashIndex{};
        }
        return *this;
    }
    ~Map() { slots.reset(); }

    // The keys and the values by position. The values may be changed in place, and the keys are changed only through
    // the functions below.
    Span<const Key> keys() const PR_LIFETIMEBOUND { return storedKeys.span(); }
    Span<Value> values() PR_LIFETIMEBOUND { return storedValues.span(); }
    Span<const Value> values() const PR_LIFETIMEBOUND { return storedValues.span(); }
    uint32_t count() const { return storedKeys.count(); }
    bool isEmpty() const { return storedKeys.count() == 0; }

    // The entries in order, for a range-based for loop.
    MapEntries<Key, Value> entries() PR_LIFETIMEBOUND {
        return MapEntries<Key, Value>{storedKeys.data(), storedValues.data(), count()};
    }
    MapEntries<Key, const Value> entries() const PR_LIFETIMEBOUND {
        return MapEntries<Key, const Value>{storedKeys.data(), storedValues.data(), count()};
    }

    // Returns the position of the entry of `key`, or kHashMissing.
    uint32_t find(const Lookup& key) const {
        return slots.find(hashKey(key), [&](uint32_t at) { return storedKeys[at] == key; });
    }

    // Returns the value of `key`, or null if the map has none.
    Value* PR_NULLABLE get(const Lookup& key) PR_LIFETIMEBOUND {
        uint32_t at = find(key);
        return at == kHashMissing ? nullptr : &storedValues[at];
    }
    const Value* PR_NULLABLE get(const Lookup& key) const PR_LIFETIMEBOUND {
        uint32_t at = find(key);
        return at == kHashMissing ? nullptr : &storedValues[at];
    }

    bool contains(const Lookup& key) const { return find(key) != kHashMissing; }

    // Sets the value of `key`, adding the entry if the map has none, and returns the value. A new entry's key is made
    // from `key`, or is `key` itself, moved.
    Value& set(const Lookup& key, Value value) {
        uint32_t at = find(key);
        if (at != kHashMissing) {
            storedValues[at] = move(value);
            return storedValues[at];
        }
        add(key);
        return storedValues.emplace(move(value));
    }
    template <typename Moved>
    Value& set(Moved&& key, Value value) requires(isSame<Moved, Key> && !isSame<Key, Lookup>) {
        uint32_t at = find(key);
        if (at != kHashMissing) {
            storedValues[at] = move(value);
            return storedValues[at];
        }
        add(move(key));
        return storedValues.emplace(move(value));
    }

    // Returns the value of `key`, adding the entry with a value made with no arguments if the map has none. A new
    // entry's key is made from `key`, or is `key` itself, moved.
    Value& operator[](const Lookup& key) {
        uint32_t at = find(key);
        if (at != kHashMissing)
            return storedValues[at];
        add(key);
        return storedValues.emplace();
    }
    template <typename Moved>
    Value& operator[](Moved&& key) requires(isSame<Moved, Key> && !isSame<Key, Lookup>) {
        uint32_t at = find(key);
        if (at != kHashMissing)
            return storedValues[at];
        add(move(key));
        return storedValues.emplace();
    }

    // Removes the entry of `key`, and moves the last entry into its place. Returns false if the map has none.
    bool erase(const Lookup& key) {
        uint32_t at = find(key);
        if (at == kHashMissing)
            return false;
        slots.erase(hashKey(key), at);
        uint32_t last = storedKeys.count() - 1;
        if (at != last) {
            slots.relocate(hashKey(storedKeys[last]), last, at);
            storedKeys[at] = move(storedKeys[last]);
            storedValues[at] = move(storedValues[last]);
        }
        storedKeys.pop();
        storedValues.pop();
        return true;
    }

    // Removes every entry and keeps the storage.
    void clear() {
        storedKeys.clear();
        storedValues.clear();
        slots.clear();
    }

private:
    Array<Key> storedKeys;
    Array<Value> storedValues;
    HashIndex slots;

    // File a new key, made from `key` or moved from it, at the end of the keys.
    void add(const Lookup& key) {
        slots.insert(hashKey(key), storedKeys.count());
        storedKeys.emplace(key);
    }
    void add(Key&& key) requires(!isSame<Key, Lookup>) {
        slots.insert(hashKey(key), storedKeys.count());
        storedKeys.emplace(move(key));
    }
};

} // namespace pr

PR_ASSUME_NONNULL_END
