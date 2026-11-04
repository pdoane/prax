// Key hashes: the 32-bit hash that a hash index files a key by, for the kinds of key that tables are keyed by.
//
// A type of key has an overload of hashKey beside it, as String does (pr/string.h). Keys that compare equal hash the
// same, and so do a key and its lookup (HashLookup), as a String and a view of the same text do.

#pragma once

#include "pr/base.h"
#include "pr/hash.h"
#include "pr/string_view.h"
#include "pr/type_traits.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Returns the hash of the integer or enumeration `key`.
template <typename T>
constexpr uint32_t hashKey(T key) requires(isIntegral<T> || isEnum<T>) {
    uint64_t bits = uint64_t(key);
    return uint32_t(((bits ^ (bits >> 29)) * 0x9e3779b97f4a7c15ull) >> 32);
}

// Returns the hash of the pointer `key`, which hashes its address. A pointer to char is text, which is not a key.
template <typename T>
uint32_t hashKey(T* PR_NULLABLE key) requires(!isSame<RemoveConst<T>, char>) {
    return prHashPointer(key, 0);
}

// Returns the hash of the bytes of `key`.
inline uint32_t hashKey(PrStringView key) {
    uint64_t hash = prHashBytes(key.data, key.size, 0);
    return uint32_t(hash ^ (hash >> 32));
}

// A NUL-terminated string is not a key: its text and its address are both plausible meanings, so a table neither hashes
// nor compares it, whether its chars are const or not. A key of text is a String or a PrStringView. A lookup of a
// String or PrStringView key may still be a NUL-terminated string, which converts to the key's HashLookup before it is
// hashed.
uint32_t hashKey(const char* PR_NULLABLE key) = delete;

// The lookup of a table keyed by `Key`: `Type` is what the table takes a lookup as, which it hashes and compares with
// the stored keys. A key whose type owns its data looks up by a view of it, as a String does by PrStringView
// (pr/string.h), so that a lookup copies nothing. A key's type names its lookup with a member `Lookup`. A type that
// cannot have the member, such as one of another library, specializes HashLookup instead.
template <typename Key>
struct HashLookup {
    using Type = Key;
};
template <typename Key> requires requires { typename Key::Lookup; }
struct HashLookup<Key> {
    using Type = typename Key::Lookup;
};

} // namespace pr

PR_ASSUME_NONNULL_END
