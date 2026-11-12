#include "pr/array.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/text_diff.h"

#include "text_diff/src/text_diff_sequence.h"

#include "tools/harness/test.h"

extern "C" int prTestTextDiffCApi(void);

namespace pr_impl {

namespace {

using namespace pr;

// The keys of `text`, one to a byte.
Array<uint64_t> keysOf(PrStringView text) {
    Array<uint64_t> keys;
    for (char c : text)
        keys.push(uint64_t(c));
    return keys;
}

// The changes between two strings, one key to a byte.
Array<PrTextDiffChange> changesOf(PrStringView a, PrStringView b, uint32_t maxCost = 4096) {
    Array<uint64_t> oldKeys = keysOf(a);
    Array<uint64_t> newKeys = keysOf(b);
    TextDiffSequence sequence{.maxCost = maxCost};
    TextDiffSequenceDesc desc{
        .oldKeys = oldKeys.data(),
        .oldCount = oldKeys.count(),
        .newKeys = newKeys.data(),
        .newCount = newKeys.count(),
    };
    RawArray<PrTextDiffChange> found;
    textDiffSequenceCompare(&sequence, desc, TextDiffPart{0, desc.oldCount, 0, desc.newCount}, found);
    Array<PrTextDiffChange> changes;
    changes.append(found.data, found.count);
    found.reset();
    textDiffSequenceReset(&sequence);
    return changes;
}

// Applies the changes to `a`, taking each new run from `b`, and checks that what lies between them is equal.
String applied(PrStringView a, PrStringView b, const Array<PrTextDiffChange>& changes) {
    String out;
    uint32_t oldAt = 0;
    uint32_t newAt = 0;
    for (const PrTextDiffChange& change : changes) {
        CHECK(change.oldStart >= oldAt);
        CHECK(change.oldStart - oldAt == change.newStart - newAt);
        CHECK(prStringSlice(a, oldAt, change.oldStart - oldAt) == prStringSlice(b, newAt, change.newStart - newAt));
        out.append(prStringSlice(a, oldAt, change.oldStart - oldAt));
        out.append(prStringSlice(b, change.newStart, change.newCount));
        oldAt = change.oldStart + change.oldCount;
        newAt = change.newStart + change.newCount;
    }
    CHECK(prStringFrom(a, oldAt) == prStringFrom(b, newAt));
    out.append(prStringFrom(a, oldAt));
    return out;
}

// The number of elements that an edit script removes and adds.
uint32_t cost(const Array<PrTextDiffChange>& changes) {
    uint32_t total = 0;
    for (const PrTextDiffChange& change : changes)
        total += change.oldCount + change.newCount;
    return total;
}

// The length of a longest common subsequence, by dynamic programming.
uint32_t longestCommon(PrStringView a, PrStringView b) {
    const uint32_t rows = uint32_t(a.size) + 1;
    const uint32_t columns = uint32_t(b.size) + 1;
    Array<uint32_t> table(rows * columns);
    auto at = [&](uint32_t i, uint32_t j) -> uint32_t& { return table[i * columns + j]; };
    for (uint32_t i = 1; i < rows; ++i) {
        for (uint32_t j = 1; j < columns; ++j) {
            uint32_t across = at(i - 1, j) > at(i, j - 1) ? at(i - 1, j) : at(i, j - 1);
            at(i, j) = a.data[i - 1] == b.data[j - 1] ? at(i - 1, j - 1) + 1 : across;
        }
    }
    return at(rows - 1, columns - 1);
}

// A text of lines, which a diff reads one at a time.
struct Lines {
    Span<const PrStringView> lines;

    PrTextDiffText text() const {
        return PrTextDiffText{
            .lineCount = lines.count(),
            .line = [](void* context, uint32_t index) { return static_cast<const Lines*>(context)->lines[index]; },
            .context = const_cast<Lines*>(this),
        };
    }
};

// A comparison of two texts, and its results.
struct Compared {
    Array<PrTextDiffChange> changes;
    Array<PrTextDiffLineChange> lineChanges;
};

// Compares the lines of `a` with those of `b`.
Compared compare(Span<const PrStringView> a, Span<const PrStringView> b, const PrTextDiffOptions* options = nullptr,
        Span<const PrTextDiffSync> syncs = {}) {
    PrTextDiff* diff = prTextDiffCreate(options);
    Lines oldLines{a};
    Lines newLines{b};
    PrTextDiffText old = oldLines.text();
    PrTextDiffText young = newLines.text();
    prTextDiffCompare(diff, &old, &young, syncs.data(), syncs.count());
    Compared compared;
    compared.changes.append(prTextDiffChanges(diff), prTextDiffChangeCount(diff));
    compared.lineChanges.append(prTextDiffLineChanges(diff), prTextDiffLineChangeCount(diff));
    prTextDiffDestroy(diff);
    return compared;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("text diff: the public header compiles as C") {
    CHECK(prTestTextDiffCApi() == 0);
}

TEST_CASE("text diff: equal sequences have no changes, and an empty side is one change") {
    CHECK(changesOf("abc", "abc").isEmpty());
    CHECK(changesOf("", "").isEmpty());
    Array<PrTextDiffChange> added = changesOf("", "xyz");
    REQUIRE(added.count() == 1);
    CHECK(added[0].oldCount == 0);
    CHECK(added[0].newCount == 3);
    Array<PrTextDiffChange> removed = changesOf("xyz", "");
    REQUIRE(removed.count() == 1);
    CHECK(removed[0].oldCount == 3);
}

TEST_CASE("text diff: a change in the middle is one change, between the equal ends") {
    PrStringView a = "the quick fox";
    PrStringView b = "the slow fox";
    Array<PrTextDiffChange> changes = changesOf(a, b);
    CHECK(applied(a, b, changes) == b);
    CHECK(cost(changes) == a.size + b.size - 2 * longestCommon(a, b));
}

TEST_CASE("text diff: scripts of random sequences are shortest and turn the old into the new") {
    uint32_t seed = 12345;
    auto next = [&]() {
        seed = seed * 1103515245u + 12345u;
        return seed >> 16;
    };
    for (uint32_t round = 0; round < 300; ++round) {
        String a;
        String b;
        uint32_t na = next() % 40;
        uint32_t nb = next() % 40;
        for (uint32_t i = 0; i < na; ++i)
            a.append(char('a' + next() % 4));
        for (uint32_t i = 0; i < nb; ++i)
            b.append(char('a' + next() % 4));
        Array<PrTextDiffChange> changes = changesOf(a, b);
        CHECK(applied(a, b, changes) == b);
        CHECK(cost(changes) == a.count() + b.count() - 2 * longestCommon(a, b));
    }
}

TEST_CASE("text diff: a difference past the cost is reported coarsely, and still turns the old into the new") {
    String a;
    String b;
    for (uint32_t i = 0; i < 400; ++i) {
        a.append('a');
        b.append(i % 7 == 0 ? 'a' : 'b');
    }
    Array<PrTextDiffChange> changes = changesOf(a, b, 8);
    CHECK(applied(a, b, changes) == b);
    CHECK(cost(changes) >= a.count() + b.count() - 2 * longestCommon(a, b));
}

TEST_CASE("text diff: the equal function confirms keys that collide") {
    // Every element shares a key, and only the function tells them apart.
    struct Pair {
        PrStringView a;
        PrStringView b;
    } pair{"abcd", "abxd"};
    const uint64_t keys[] = {7, 7, 7, 7};
    TextDiffSequence sequence{.maxCost = 4096};
    TextDiffSequenceDesc desc{
        .oldKeys = keys,
        .oldCount = 4,
        .newKeys = keys,
        .newCount = 4,
        .equal = [](void* context, uint32_t i, uint32_t j) {
            const Pair* p = static_cast<const Pair*>(context);
            return p->a.data[i] == p->b.data[j];
        },
        .equalContext = &pair,
    };
    RawArray<PrTextDiffChange> changes;
    textDiffSequenceCompare(&sequence, desc, TextDiffPart{0, 4, 0, 4}, changes);
    REQUIRE(changes.count == 1);
    CHECK(changes.data[0].oldStart == 2);
    CHECK(changes.data[0].oldCount == 1);
    CHECK(changes.data[0].newCount == 1);
    changes.reset();
    textDiffSequenceReset(&sequence);
}

TEST_CASE("text diff: lines change, and the bytes within each pair of changed lines") {
    const PrStringView a[] = {"int x = 1;", "same", "gone"};
    const PrStringView b[] = {"int y = 1;", "same", "new one", "added"};
    Compared compared = compare(a, b);
    REQUIRE(compared.changes.count() == 2);
    CHECK(compared.changes[0].oldStart == 0);
    CHECK(compared.changes[0].oldCount == 1);
    CHECK(compared.changes[0].newCount == 1);
    CHECK(compared.changes[1].oldStart == 2);
    CHECK(compared.changes[1].oldCount == 1);
    CHECK(compared.changes[1].newStart == 2);
    CHECK(compared.changes[1].newCount == 2);
    // Line 0 changes x to y. Line 2 pairs "gone" with "new one", and "added" has no pair.
    REQUIRE(compared.lineChanges.count() >= 2);
    const PrTextDiffLineChange& first = compared.lineChanges[0];
    CHECK(first.oldLine == 0);
    CHECK(first.newLine == 0);
    CHECK(first.bytes.oldStart == 4);
    CHECK(first.bytes.oldCount == 1);
    CHECK(first.bytes.newStart == 4);
    CHECK(first.bytes.newCount == 1);
    for (uint32_t i = 1; i < compared.lineChanges.count(); ++i) {
        CHECK(compared.lineChanges[i].oldLine == 2);
        CHECK(compared.lineChanges[i].newLine == 2);
    }
}

TEST_CASE("text diff: a change within a line spans whole characters") {
    // é and è share their first byte, which still belongs to the change.
    const PrStringView a[] = {"caf\xC3\xA9!"};
    const PrStringView b[] = {"caf\xC3\xA8!"};
    Compared compared = compare(a, b);
    REQUIRE(compared.lineChanges.count() == 1);
    CHECK(compared.lineChanges[0].bytes.oldStart == 3);
    CHECK(compared.lineChanges[0].bytes.oldCount == 2);
    CHECK(compared.lineChanges[0].bytes.newStart == 3);
    CHECK(compared.lineChanges[0].bytes.newCount == 2);
}

TEST_CASE("text diff: a pair with a line past the limit changes whole, and a limit of zero compares lines alone") {
    const PrStringView a[] = {"abcdef"};
    const PrStringView b[] = {"abcxef"};
    PrTextDiffOptions options = prTextDiffOptionsDefault();
    options.maxLineBytes = 4;
    Compared whole = compare(a, b, &options);
    REQUIRE(whole.lineChanges.count() == 1);
    CHECK(whole.lineChanges[0].bytes.oldStart == 0);
    CHECK(whole.lineChanges[0].bytes.oldCount == 6);
    CHECK(whole.lineChanges[0].bytes.newCount == 6);
    options.maxLineBytes = 0;
    Compared lines = compare(a, b, &options);
    CHECK(lines.changes.count() == 1);
    CHECK(lines.lineChanges.isEmpty());
}

TEST_CASE("text diff: a part compares alone, and its changes join only each other") {
    // The parts meet at index 2, where each has a change, which stay apart.
    const uint64_t oldKeys[] = {1, 2, 3, 4};
    const uint64_t newKeys[] = {1, 9, 8, 4};
    TextDiffSequence sequence{.maxCost = 4096};
    TextDiffSequenceDesc desc{.oldKeys = oldKeys, .oldCount = 4, .newKeys = newKeys, .newCount = 4};
    RawArray<PrTextDiffChange> changes;
    textDiffSequenceCompare(&sequence, desc, TextDiffPart{0, 2, 0, 2}, changes);
    textDiffSequenceCompare(&sequence, desc, TextDiffPart{2, 4, 2, 4}, changes);
    REQUIRE(changes.count == 2);
    CHECK(changes.data[0].oldStart == 1);
    CHECK(changes.data[0].oldCount == 1);
    CHECK(changes.data[1].oldStart == 2);
    CHECK(changes.data[1].oldCount == 1);
    changes.reset();
    textDiffSequenceReset(&sequence);
}

TEST_CASE("text diff: a sync point pairs its lines, and the texts on either side compare apart") {
    // The shortest script keeps "b" in place and pairs nothing else. A sync point of old line 1 with new line 3 says
    // that "b" moved down past the lines added above it.
    const PrStringView a[] = {"a", "b", "c"};
    const PrStringView b[] = {"a", "x", "y", "b2", "c"};
    Compared plain = compare(a, b);
    REQUIRE(plain.changes.count() == 1);
    CHECK(plain.changes[0].oldStart == 1);
    CHECK(plain.changes[0].oldCount == 1);
    CHECK(plain.changes[0].newCount == 3);

    const PrTextDiffSync syncs[] = {{1, 3}};
    Compared synced = compare(a, b, nullptr, syncs);
    REQUIRE(synced.changes.count() == 2);
    CHECK(synced.changes[0].oldStart == 1);
    CHECK(synced.changes[0].oldCount == 0);
    CHECK(synced.changes[0].newStart == 1);
    CHECK(synced.changes[0].newCount == 2);
    // The sync point's lines differ, so they are a change of their own, beside the one before it.
    CHECK(synced.changes[1].oldStart == 1);
    CHECK(synced.changes[1].oldCount == 1);
    CHECK(synced.changes[1].newStart == 3);
    CHECK(synced.changes[1].newCount == 1);
    REQUIRE(synced.lineChanges.count() == 1);
    CHECK(synced.lineChanges[0].oldLine == 1);
    CHECK(synced.lineChanges[0].newLine == 3);
    CHECK(synced.lineChanges[0].bytes.oldCount == 0);
    CHECK(synced.lineChanges[0].bytes.newStart == 1);
    CHECK(synced.lineChanges[0].bytes.newCount == 1);
}

TEST_CASE("text diff: a sync point of equal lines is no change, and moves lines that the script would match") {
    // Without the sync point, the script matches the two "same" lines in place.
    const PrStringView a[] = {"same", "one", "two"};
    const PrStringView b[] = {"one", "two", "same"};
    const PrTextDiffSync syncs[] = {{0, 2}};
    Compared synced = compare(a, b, nullptr, syncs);
    REQUIRE(synced.changes.count() == 2);
    CHECK(synced.changes[0].oldStart == 0);
    CHECK(synced.changes[0].oldCount == 0);
    CHECK(synced.changes[0].newCount == 2);
    CHECK(synced.changes[1].oldStart == 1);
    CHECK(synced.changes[1].oldCount == 2);
    CHECK(synced.changes[1].newStart == 3);
    CHECK(synced.changes[1].newCount == 0);
}
