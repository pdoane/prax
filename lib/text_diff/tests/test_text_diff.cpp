#include "pr/text_diff.h"

#include "text_diff/src/text_diff_sequence.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

extern "C" int prTestTextDiffCApi(void);

namespace pr_impl {

namespace {

// The changes between two strings, one key to a byte.
std::vector<PrTextDiffChange> changesOf(const std::string& a, const std::string& b, uint32_t maxCost = 4096) {
    std::vector<uint64_t> oldKeys(a.begin(), a.end());
    std::vector<uint64_t> newKeys(b.begin(), b.end());
    TextDiffSequence sequence{.maxCost = maxCost};
    TextDiffSequenceDesc desc{
        .oldKeys = oldKeys.data(),
        .oldCount = uint32_t(oldKeys.size()),
        .newKeys = newKeys.data(),
        .newCount = uint32_t(newKeys.size()),
    };
    RawArray<PrTextDiffChange> found;
    textDiffSequenceCompare(&sequence, desc, TextDiffPart{0, desc.oldCount, 0, desc.newCount}, found);
    std::vector<PrTextDiffChange> changes(found.data, found.data + found.count);
    found.reset();
    textDiffSequenceReset(&sequence);
    return changes;
}

// Applies the changes to `a`, taking each new run from `b`, and checks that what lies between them is equal.
std::string applied(const std::string& a, const std::string& b, const std::vector<PrTextDiffChange>& changes) {
    std::string out;
    uint32_t oldAt = 0;
    uint32_t newAt = 0;
    for (const PrTextDiffChange& change : changes) {
        CHECK(change.oldStart >= oldAt);
        CHECK(change.oldStart - oldAt == change.newStart - newAt);
        CHECK(a.substr(oldAt, change.oldStart - oldAt) == b.substr(newAt, change.newStart - newAt));
        out += a.substr(oldAt, change.oldStart - oldAt);
        out += b.substr(change.newStart, change.newCount);
        oldAt = change.oldStart + change.oldCount;
        newAt = change.newStart + change.newCount;
    }
    CHECK(a.substr(oldAt) == b.substr(newAt));
    out += a.substr(oldAt);
    return out;
}

// The number of elements that an edit script removes and adds.
uint32_t cost(const std::vector<PrTextDiffChange>& changes) {
    uint32_t total = 0;
    for (const PrTextDiffChange& change : changes)
        total += change.oldCount + change.newCount;
    return total;
}

// The length of a longest common subsequence, by dynamic programming.
uint32_t longestCommon(const std::string& a, const std::string& b) {
    std::vector<std::vector<uint32_t>> table(a.size() + 1, std::vector<uint32_t>(b.size() + 1));
    for (size_t i = 1; i <= a.size(); ++i) {
        for (size_t j = 1; j <= b.size(); ++j) {
            uint32_t across = table[i - 1][j] > table[i][j - 1] ? table[i - 1][j] : table[i][j - 1];
            table[i][j] = a[i - 1] == b[j - 1] ? table[i - 1][j - 1] + 1 : across;
        }
    }
    return table[a.size()][b.size()];
}

// A text of lines, which a diff reads one at a time.
struct Lines {
    std::vector<std::string> lines;

    PrTextDiffText text() const {
        return PrTextDiffText{
            .lineCount = uint32_t(lines.size()),
            .line = [](void* context, uint32_t index) {
                return PrStringView(static_cast<const Lines*>(context)->lines[index]);
            },
            .context = const_cast<Lines*>(this),
        };
    }
};

// A comparison of two texts, and its results.
struct Compared {
    std::vector<PrTextDiffChange> changes;
    std::vector<PrTextDiffLineChange> lineChanges;
};

Compared compare(const Lines& a, const Lines& b, const PrTextDiffOptions* options = nullptr,
        const std::vector<PrTextDiffSync>& syncs = {}) {
    PrTextDiff* diff = prTextDiffCreate(options);
    PrTextDiffText old = a.text();
    PrTextDiffText young = b.text();
    prTextDiffCompare(diff, &old, &young, syncs.data(), uint32_t(syncs.size()));
    Compared compared{
        .changes = {prTextDiffChanges(diff), prTextDiffChanges(diff) + prTextDiffChangeCount(diff)},
        .lineChanges = {prTextDiffLineChanges(diff), prTextDiffLineChanges(diff) + prTextDiffLineChangeCount(diff)},
    };
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
    CHECK(changesOf("abc", "abc").empty());
    CHECK(changesOf("", "").empty());
    std::vector<PrTextDiffChange> added = changesOf("", "xyz");
    REQUIRE(added.size() == 1);
    CHECK(added[0].oldCount == 0);
    CHECK(added[0].newCount == 3);
    std::vector<PrTextDiffChange> removed = changesOf("xyz", "");
    REQUIRE(removed.size() == 1);
    CHECK(removed[0].oldCount == 3);
}

TEST_CASE("text diff: a change in the middle is one change, between the equal ends") {
    std::string a = "the quick fox";
    std::string b = "the slow fox";
    std::vector<PrTextDiffChange> changes = changesOf(a, b);
    CHECK(applied(a, b, changes) == b);
    CHECK(cost(changes) == a.size() + b.size() - 2 * longestCommon(a, b));
}

TEST_CASE("text diff: scripts of random sequences are shortest and turn the old into the new") {
    uint32_t seed = 12345;
    auto next = [&]() {
        seed = seed * 1103515245u + 12345u;
        return seed >> 16;
    };
    for (uint32_t round = 0; round < 300; ++round) {
        std::string a;
        std::string b;
        uint32_t na = next() % 40;
        uint32_t nb = next() % 40;
        for (uint32_t i = 0; i < na; ++i)
            a += char('a' + next() % 4);
        for (uint32_t i = 0; i < nb; ++i)
            b += char('a' + next() % 4);
        std::vector<PrTextDiffChange> changes = changesOf(a, b);
        CHECK(applied(a, b, changes) == b);
        CHECK(cost(changes) == a.size() + b.size() - 2 * longestCommon(a, b));
    }
}

TEST_CASE("text diff: a difference past the cost is reported coarsely, and still turns the old into the new") {
    std::string a(400, 'a');
    std::string b(400, 'b');
    for (size_t i = 0; i < a.size(); i += 7)
        b[i] = 'a';
    std::vector<PrTextDiffChange> changes = changesOf(a, b, 8);
    CHECK(applied(a, b, changes) == b);
    CHECK(cost(changes) >= a.size() + b.size() - 2 * longestCommon(a, b));
}

TEST_CASE("text diff: the equal function confirms keys that collide") {
    // Every element shares a key, and only the function tells them apart.
    std::string a = "abcd";
    std::string b = "abxd";
    std::vector<uint64_t> keys(4, 7);
    struct Pair {
        const std::string* a;
        const std::string* b;
    } pair{&a, &b};
    TextDiffSequence sequence{.maxCost = 4096};
    TextDiffSequenceDesc desc{
        .oldKeys = keys.data(),
        .oldCount = 4,
        .newKeys = keys.data(),
        .newCount = 4,
        .equal = [](void* context, uint32_t i, uint32_t j) {
            const Pair* p = static_cast<const Pair*>(context);
            return (*p->a)[i] == (*p->b)[j];
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
    Lines a{{"int x = 1;", "same", "gone"}};
    Lines b{{"int y = 1;", "same", "new one", "added"}};
    Compared compared = compare(a, b);
    REQUIRE(compared.changes.size() == 2);
    CHECK(compared.changes[0].oldStart == 0);
    CHECK(compared.changes[0].oldCount == 1);
    CHECK(compared.changes[0].newCount == 1);
    CHECK(compared.changes[1].oldStart == 2);
    CHECK(compared.changes[1].oldCount == 1);
    CHECK(compared.changes[1].newStart == 2);
    CHECK(compared.changes[1].newCount == 2);
    // Line 0 changes x to y. Line 2 pairs "gone" with "new one", and "added" has no pair.
    REQUIRE(compared.lineChanges.size() >= 2);
    const PrTextDiffLineChange& first = compared.lineChanges[0];
    CHECK(first.oldLine == 0);
    CHECK(first.newLine == 0);
    CHECK(first.bytes.oldStart == 4);
    CHECK(first.bytes.oldCount == 1);
    CHECK(first.bytes.newStart == 4);
    CHECK(first.bytes.newCount == 1);
    for (size_t i = 1; i < compared.lineChanges.size(); ++i) {
        CHECK(compared.lineChanges[i].oldLine == 2);
        CHECK(compared.lineChanges[i].newLine == 2);
    }
}

TEST_CASE("text diff: a change within a line spans whole characters") {
    // é and è share their first byte, which still belongs to the change.
    Lines a{{"caf\xC3\xA9!"}};
    Lines b{{"caf\xC3\xA8!"}};
    Compared compared = compare(a, b);
    REQUIRE(compared.lineChanges.size() == 1);
    CHECK(compared.lineChanges[0].bytes.oldStart == 3);
    CHECK(compared.lineChanges[0].bytes.oldCount == 2);
    CHECK(compared.lineChanges[0].bytes.newStart == 3);
    CHECK(compared.lineChanges[0].bytes.newCount == 2);
}

TEST_CASE("text diff: a pair with a line past the limit changes whole, and a limit of zero compares lines alone") {
    Lines a{{"abcdef"}};
    Lines b{{"abcxef"}};
    PrTextDiffOptions options = prTextDiffOptionsDefault();
    options.maxLineBytes = 4;
    Compared whole = compare(a, b, &options);
    REQUIRE(whole.lineChanges.size() == 1);
    CHECK(whole.lineChanges[0].bytes.oldStart == 0);
    CHECK(whole.lineChanges[0].bytes.oldCount == 6);
    CHECK(whole.lineChanges[0].bytes.newCount == 6);
    options.maxLineBytes = 0;
    Compared lines = compare(a, b, &options);
    CHECK(lines.changes.size() == 1);
    CHECK(lines.lineChanges.empty());
}

TEST_CASE("text diff: a part compares alone, and its changes join only each other") {
    // The parts meet at index 2, where each has a change, which stay apart.
    std::vector<uint64_t> oldKeys{1, 2, 3, 4};
    std::vector<uint64_t> newKeys{1, 9, 8, 4};
    TextDiffSequence sequence{.maxCost = 4096};
    TextDiffSequenceDesc desc{.oldKeys = oldKeys.data(), .oldCount = 4, .newKeys = newKeys.data(), .newCount = 4};
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
    Lines a{{"a", "b", "c"}};
    Lines b{{"a", "x", "y", "b2", "c"}};
    Compared plain = compare(a, b);
    REQUIRE(plain.changes.size() == 1);
    CHECK(plain.changes[0].oldStart == 1);
    CHECK(plain.changes[0].oldCount == 1);
    CHECK(plain.changes[0].newCount == 3);

    Compared synced = compare(a, b, nullptr, {{1, 3}});
    REQUIRE(synced.changes.size() == 2);
    CHECK(synced.changes[0].oldStart == 1);
    CHECK(synced.changes[0].oldCount == 0);
    CHECK(synced.changes[0].newStart == 1);
    CHECK(synced.changes[0].newCount == 2);
    // The sync point's lines differ, so they are a change of their own, beside the one before it.
    CHECK(synced.changes[1].oldStart == 1);
    CHECK(synced.changes[1].oldCount == 1);
    CHECK(synced.changes[1].newStart == 3);
    CHECK(synced.changes[1].newCount == 1);
    REQUIRE(synced.lineChanges.size() == 1);
    CHECK(synced.lineChanges[0].oldLine == 1);
    CHECK(synced.lineChanges[0].newLine == 3);
    CHECK(synced.lineChanges[0].bytes.oldCount == 0);
    CHECK(synced.lineChanges[0].bytes.newStart == 1);
    CHECK(synced.lineChanges[0].bytes.newCount == 1);
}

TEST_CASE("text diff: a sync point of equal lines is no change, and moves lines that the script would match") {
    // Without the sync point, the script matches the two "same" lines in place.
    Lines a{{"same", "one", "two"}};
    Lines b{{"one", "two", "same"}};
    Compared synced = compare(a, b, nullptr, {{0, 2}});
    REQUIRE(synced.changes.size() == 2);
    CHECK(synced.changes[0].oldStart == 0);
    CHECK(synced.changes[0].oldCount == 0);
    CHECK(synced.changes[0].newCount == 2);
    CHECK(synced.changes[1].oldStart == 1);
    CHECK(synced.changes[1].oldCount == 2);
    CHECK(synced.changes[1].newStart == 3);
    CHECK(synced.changes[1].newCount == 0);
}
