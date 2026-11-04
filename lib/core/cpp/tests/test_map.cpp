#include "pr/hash.h"
#include "pr/map.h"
#include "pr/set.h"
#include "pr/status.h"
#include "pr/string.h"

#include "tools/harness/test.h"

namespace pr_impl {
namespace {

// A key of a number and a name, which looks up by a view of its name, as a String looks up by PrStringView.
struct NamedKeyView {
    uint32_t number;
    PrStringView name;
};

struct NamedKey {
    using Lookup = NamedKeyView;

    uint32_t number = 0;
    String name;

    NamedKey() = default;
    NamedKey(const NamedKeyView& key) : number(key.number), name(key.name) {}
    operator NamedKeyView() const { return NamedKeyView{number, name}; }

    friend bool operator==(const NamedKey& a, const NamedKeyView& b) { return a.number == b.number && a.name == b.name; }
};

uint32_t hashKey(const NamedKeyView& key) {
    uint64_t hash = prHashBytes(key.name.data, key.name.size, key.number);
    return uint32_t(hash ^ (hash >> 32));
}
uint32_t hashKey(const NamedKey& key) {
    return hashKey(NamedKeyView(key));
}

} // namespace
} // namespace pr_impl

using namespace pr_impl;

// An integer or an enumeration hashes as its value.
static_assert(hashKey(7) == hashKey(7u));
static_assert(hashKey(PrStatus_Ok) == hashKey(int(PrStatus_Ok)));

// True if a `T` hashes as a key.
template <typename T>
constexpr bool kHashesAsKey = requires(T key) { hashKey(key); };

// A NUL-terminated string is not a key, whether its chars are const or not, and other pointers hash their addresses.
static_assert(!kHashesAsKey<const char*>);
static_assert(!kHashesAsKey<char*>);
static_assert(kHashesAsKey<const int*>);
static_assert(kHashesAsKey<PrStringView>);

TEST_CASE("a map sets, finds and erases entries, and looks a String up by a view") {
    Map<String, int> ages;
    ages.set("ada", 36);
    ages.set("alan", 41);
    ages["grace"] = 85;
    ages["ada"] += 1;
    CHECK(ages.count() == 3);
    REQUIRE(ages.get("ada") != nullptr);
    CHECK(*ages.get("ada") == 37);
    CHECK(ages.contains(String("grace")));
    CHECK(ages.get("linus") == nullptr);
    CHECK(ages.erase("ada"));
    CHECK(!ages.erase("ada"));
    CHECK(ages.count() == 2);
    // The last entry moved into the erased one's place, and is still found.
    CHECK(*ages.get("grace") == 85);
    CHECK(*ages.get("alan") == 41);
    REQUIRE(ages.keys().count() == 2);
    CHECK(ages.keys()[0] == "grace");
    CHECK(ages.values()[0] == 85);
}

TEST_CASE("a map's values change in place, and its entries iterate in order") {
    Map<String, int> counts;
    counts.set("a", 1);
    counts.set("b", 2);
    for (int& value : counts.values())
        value *= 10;
    String seen;
    int total = 0;
    for (auto [key, value] : counts.entries()) {
        seen.append(key);
        total += value;
        ++value;
    }
    CHECK(seen == "ab");
    CHECK(total == 30);
    CHECK(*counts.get("b") == 21);
}

TEST_CASE("a map stays indexed through growth, erasure and copies") {
    Map<uint32_t, uint32_t> squares;
    for (uint32_t i = 0; i < 1000; ++i)
        squares.set(i, i * i);
    for (uint32_t i = 0; i < 1000; i += 2)
        CHECK(squares.erase(i));
    Map<uint32_t, uint32_t> copy = squares;
    Map<uint32_t, uint32_t> moved = move(squares);
    CHECK(squares.isEmpty());
    for (uint32_t i = 0; i < 1000; ++i) {
        const uint32_t* found = copy.get(i);
        CHECK((found != nullptr) == (i % 2 == 1));
        if (found != nullptr)
            CHECK(*found == i * i);
        CHECK(moved.contains(i) == (i % 2 == 1));
    }
    // A copy's index is its own: adding to the copy leaves the original as it was.
    copy.set(0, 7);
    CHECK(!moved.contains(0));
    Map<uint32_t, uint32_t> assigned;
    assigned.set(5000, 1);
    assigned = copy;
    CHECK(!assigned.contains(5000));
    CHECK(*assigned.get(0) == 7);
}

TEST_CASE("a set adds each item once and erases it") {
    Set<String> seen;
    CHECK(seen.add("a"));
    CHECK(seen.add("b"));
    CHECK(!seen.add("a"));
    CHECK(seen.count() == 2);
    CHECK(seen.contains("b"));
    CHECK(seen.erase("a"));
    CHECK(!seen.contains("a"));
    Set<String> copy = seen;
    CHECK(copy.contains("b"));
    seen.clear();
    CHECK(seen.isEmpty());
    CHECK(copy.count() == 1);
    uint32_t items = 0;
    for (const String& item : copy) {
        CHECK(item == "b");
        ++items;
    }
    CHECK(items == 1);
}

TEST_CASE("a map and a set move a key in, and copy one that is not moved") {
    String longKey = "a key that is longer than the inline bytes of a string";
    Map<String, int> map;
    map.set(longKey, 1);
    CHECK(!longKey.isNull());
    // A key that the map holds already stays with its caller.
    String same = longKey;
    map.set(move(same), 2);
    CHECK(!same.isNull());
    CHECK(map.count() == 1);
    CHECK(*map.get(longKey) == 2);
    String other = "another key that is longer than the inline bytes of a string";
    const char* bytes = other.data();
    map[move(other)] = 3;
    CHECK(other.isNull());
    REQUIRE(map.keys().count() == 2);
    CHECK(map.keys()[1].data() == bytes);

    Set<String> set;
    String item = "an item that is longer than the inline bytes of a string";
    bytes = item.data();
    CHECK(set.add(move(item)));
    CHECK(item.isNull());
    CHECK(set.items()[0].data() == bytes);
    String again = set.items()[0];
    CHECK(!set.add(move(again)));
    CHECK(set.count() == 1);
}

TEST_CASE("a map looks a key up by the lookup type that the key names") {
    Map<NamedKey, int> sizes;
    sizes.set(NamedKeyView{1, "a"}, 10);
    sizes[NamedKeyView{2, "a"}] = 20;
    NamedKey key = NamedKeyView{1, "b"};
    sizes.set(move(key), 30);
    CHECK(sizes.count() == 3);
    CHECK(*sizes.get(NamedKeyView{1, "a"}) == 10);
    CHECK(*sizes.get(NamedKeyView{2, "a"}) == 20);
    CHECK(*sizes.get(NamedKeyView{1, "b"}) == 30);
    CHECK(sizes.get(NamedKeyView{2, "b"}) == nullptr);
    CHECK(sizes.erase(NamedKeyView{1, "a"}));
    CHECK(*sizes.get(NamedKeyView{1, "b"}) == 30);
}

TEST_CASE("a set stays indexed through growth and erasure") {
    Set<uint32_t> odd;
    for (uint32_t i = 0; i < 1000; ++i)
        CHECK(odd.add(i));
    for (uint32_t i = 0; i < 1000; i += 2)
        CHECK(odd.erase(i));
    CHECK(odd.count() == 500);
    for (uint32_t i = 0; i < 1000; ++i)
        CHECK(odd.contains(i) == (i % 2 == 1));
    odd.clear();
    CHECK(!odd.contains(1));
    CHECK(odd.add(1));
}
