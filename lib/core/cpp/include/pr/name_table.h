// Name tables: lookups over a fixed set of names.
//
// tools/gen/name_table.py generates each table: a perfect hash over the set, and the names in one blob. A lookup hashes
// the text once and reads the one entry that the hash selects. Two texts can share a hash, so the lookup then compares
// the name of that entry against the text.

#pragma once

#include "pr/ascii.h"
#include "pr/base.h"
#include "pr/bit_cast.h"
#include "pr/mem.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr {

// Returns the text at `index` of `blob`, whose texts start at the offsets in `at`. `at` has one more entry than the
// blob has texts.
template <typename Offset>
inline PrStringView nameAt(const char* blob, const Offset* PR_NONNULL at, uint32_t index) {
    return PrStringView{blob + at[index], size_t(at[index + 1] - at[index])};
}

// Returns the `size` bytes at `text` in the low bytes of the result, with zeros above them. `size` must be below eight.
// The reads overlap and stay inside the text, and a byte that two reads cover has the same value in each.
constexpr uint64_t nameWordPart(const char* text, size_t size) {
    if (size >= 4)
        return uint64_t(loadUnaligned<uint32_t>(text)) |
            (uint64_t(loadUnaligned<uint32_t>(text + size - 4)) << (8 * (size - 4)));
    if (size == 0)
        return 0;
    return uint64_t(uint8_t(text[0])) | (uint64_t(uint8_t(text[size >> 1])) << (8 * (size >> 1))) |
        (uint64_t(uint8_t(text[size - 1])) << (8 * (size - 1)));
}

// Returns a mask with the low `size` bytes set and zeros above them. `size` must be eight or fewer.
constexpr uint64_t nameWordMask(size_t size) {
    return size == 8 ? ~uint64_t(0) : (uint64_t(1) << (8 * size)) - 1;
}

// Returns `word` with every ASCII uppercase byte folded to lowercase. A byte is uppercase if adding 0x3f carries into
// the high bit and adding 0x25 does not, which is 0x41 through 0x5A. Only the low seven bits go into either sum, so the
// carry of each byte stays inside that byte.
constexpr uint64_t nameFold(uint64_t word) {
    uint64_t low = word & 0x7f7f7f7f7f7f7f7full;
    uint64_t upper = (low + 0x3f3f3f3f3f3f3f3full) & ~(low + 0x2525252525252525ull) & ~word & 0x8080808080808080ull;
    return word | (upper >> 2);
}

// Returns `hash` with its bits mixed by the 64-bit finalizer of MurmurHash3.
constexpr uint64_t nameMix(uint64_t hash) {
    hash ^= hash >> 33;
    hash *= 0xff51afd7ed558ccdull;
    hash ^= hash >> 29;
    hash *= 0xc4ceb9fe1a85ec53ull;
    hash ^= hash >> 32;
    return hash;
}

// Returns the hash of the `size` bytes at `text` with `seed`, and writes the first eight bytes, or all of a shorter
// text, to `*word`. The low half of the hash selects the slot, and the high half selects the displacement.
constexpr uint64_t nameHash(const char* text, size_t size, uint64_t seed, uint64_t* word) {
    uint64_t hash = seed ^ size;
    if (size < 8) {
        *word = nameWordPart(text, size);
        return nameMix(hash ^ *word);
    }
    *word = loadUnaligned<uint64_t>(text);
    const char* last = text + size - 8;
    for (; text < last; text += 8)
        hash = nameMix(hash ^ loadUnaligned<uint64_t>(text));
    return nameMix(hash ^ loadUnaligned<uint64_t>(last));
}

// Computes nameHash over the text with ASCII case folded. The word that it writes is folded too.
constexpr uint64_t nameHashLower(const char* text, size_t size, uint64_t seed, uint64_t* word) {
    uint64_t hash = seed ^ size;
    if (size < 8) {
        *word = nameFold(nameWordPart(text, size));
        return nameMix(hash ^ *word);
    }
    *word = nameFold(loadUnaligned<uint64_t>(text));
    const char* last = text + size - 8;
    for (; text < last; text += 8)
        hash = nameMix(hash ^ nameFold(loadUnaligned<uint64_t>(text)));
    return nameMix(hash ^ nameFold(loadUnaligned<uint64_t>(last)));
}

// The seed for hashing a name outside the generated tables, such as an author-defined name that is looked up by its
// hash.
constexpr uint64_t kNameSeed = 0x9e3779b97f4a7c15ull;

// Overloads of nameHash and nameHashLower for callers that need only the hash.
constexpr uint64_t nameHash(const char* text, size_t size, uint64_t seed) {
    uint64_t word;
    return nameHash(text, size, seed, &word);
}
constexpr uint64_t nameHashLower(const char* text, size_t size, uint64_t seed) {
    uint64_t word;
    return nameHashLower(text, size, seed, &word);
}

// Returns true if `name` equals `text`, whose first eight bytes are in `word`. `name` is an entry of a generated blob.
// Eight bytes of padding follow the last name of a blob, so a full word can be read from any name.
inline bool nameEquals(PrStringView name, PrStringView text, uint64_t word) {
    if (name.size != text.size)
        return false;
    if (text.size <= 8)
        return ((loadUnaligned<uint64_t>(name.data) ^ word) & nameWordMask(text.size)) == 0;
    return memEquals(name.data, text.data, text.size);
}

// Returns true if `name` equals `text` with ASCII case folded. `word` holds the folded first eight bytes of `text`,
// which nameHashLower writes. `name` is lowercase, because the generator writes the names of such a table in lowercase.
inline bool nameEqualsLower(PrStringView name, PrStringView text, uint64_t word) {
    if (name.size != text.size)
        return false;
    if (text.size <= 8)
        return ((loadUnaligned<uint64_t>(name.data) ^ word) & nameWordMask(text.size)) == 0;
    return asciiEqualsFold(text, name);
}

} // namespace pr

PR_ASSUME_NONNULL_END
