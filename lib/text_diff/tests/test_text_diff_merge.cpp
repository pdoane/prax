// Three-way merges: the chunks of two sides' changes to a base, sync points, and the conflict markers of a merge's
// text, written and found again.

#include "pr/text_diff_merge.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

// A text of lines, which a merge reads one at a time.
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

    std::vector<std::string> slice(uint32_t start, uint32_t count) const {
        std::vector<std::string>::const_iterator first = lines.begin() + ptrdiff_t(start);
        return {first, first + ptrdiff_t(count)};
    }
};

// Splits `text` at its line feeds.
Lines linesOf(const std::string& text) {
    Lines out;
    size_t at = 0;
    for (size_t i = 0; i <= text.size(); ++i) {
        if (i == text.size() || text[i] == '\n') {
            out.lines.push_back(text.substr(at, i - at));
            at = i + 1;
        }
    }
    return out;
}

// A merge of three texts, and its results.
struct Merged {
    std::vector<PrTextDiffMergeChunk> chunks;
    uint32_t conflicts = 0;
    std::string text;
};

Merged merge(const Lines& base, const Lines& left, const Lines& right, const std::vector<PrTextDiffMergeSync>& syncs = {},
        const PrTextDiffMarkerOptions* options = nullptr) {
    PrTextDiffMerge* merge = prTextDiffMergeCreate(nullptr);
    PrTextDiffText texts[] = {base.text(), left.text(), right.text()};
    prTextDiffMergeCompare(merge, &texts[0], &texts[1], &texts[2], syncs.data(), uint32_t(syncs.size()));
    const PrTextDiffMergeChunk* chunks = prTextDiffMergeChunks(merge);
    Merged merged{
        .chunks = {chunks, chunks + prTextDiffMergeChunkCount(merge)},
        .conflicts = prTextDiffMergeConflictCount(merge),
    };
    PrRawString text = prTextDiffMergeText(merge, &texts[0], &texts[1], &texts[2], options);
    merged.text = std::string(prRawStringView(&text).data, prRawStringView(&text).size);
    prRawStringReset(&text);
    prTextDiffMergeDestroy(merge);
    return merged;
}

// Checks that the chunks cover each text whole, in order, and that each kind holds what it says.
void checkChunks(const Lines& base, const Lines& left, const Lines& right, const std::vector<PrTextDiffMergeChunk>& chunks) {
    uint32_t b = 0;
    uint32_t l = 0;
    uint32_t r = 0;
    for (const PrTextDiffMergeChunk& chunk : chunks) {
        REQUIRE(chunk.baseStart == b);
        REQUIRE(chunk.leftStart == l);
        REQUIRE(chunk.rightStart == r);
        std::vector<std::string> baseLines = base.slice(chunk.baseStart, chunk.baseCount);
        std::vector<std::string> leftLines = left.slice(chunk.leftStart, chunk.leftCount);
        std::vector<std::string> rightLines = right.slice(chunk.rightStart, chunk.rightCount);
        switch (chunk.kind) {
            case PrTextDiffMergeKind_Unchanged:
                CHECK(baseLines == leftLines);
                CHECK(baseLines == rightLines);
                break;
            case PrTextDiffMergeKind_Left:
                CHECK(baseLines == rightLines);
                CHECK(baseLines != leftLines);
                break;
            case PrTextDiffMergeKind_Right:
                CHECK(baseLines == leftLines);
                CHECK(baseLines != rightLines);
                break;
            case PrTextDiffMergeKind_Both:
                CHECK(leftLines == rightLines);
                break;
            case PrTextDiffMergeKind_Conflict:
                CHECK(leftLines != rightLines);
                break;
        }
        b += chunk.baseCount;
        l += chunk.leftCount;
        r += chunk.rightCount;
    }
    CHECK(b == base.lines.size());
    CHECK(l == left.lines.size());
    CHECK(r == right.lines.size());
}

uint32_t countOfKind(const std::vector<PrTextDiffMergeChunk>& chunks, PrTextDiffMergeKind kind) {
    uint32_t count = 0;
    for (const PrTextDiffMergeChunk& chunk : chunks)
        count += chunk.kind == kind ? 1 : 0;
    return count;
}

std::vector<PrTextDiffConflict> conflictsOf(const std::string& text) {
    Lines lines = linesOf(text);
    PrTextDiffText read = lines.text();
    std::vector<PrTextDiffConflict> conflicts(prTextDiffConflicts(&read, nullptr, 0));
    prTextDiffConflicts(&read, conflicts.data(), uint32_t(conflicts.size()));
    return conflicts;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("text diff merge: changes of different lines merge, and the text takes each side's") {
    Lines base = linesOf("a\nb\nc\nd\ne");
    Lines left = linesOf("A\nb\nc\nd\ne");
    Lines right = linesOf("a\nb\nc\nD\ne\nf");
    Merged merged = merge(base, left, right);
    checkChunks(base, left, right, merged.chunks);
    CHECK(merged.conflicts == 0);
    CHECK(countOfKind(merged.chunks, PrTextDiffMergeKind_Left) == 1);
    CHECK(countOfKind(merged.chunks, PrTextDiffMergeKind_Right) == 2);
    CHECK(merged.text == "A\nb\nc\nD\ne\nf");
}

TEST_CASE("text diff merge: a change that both sides made alike is no conflict") {
    Lines base = linesOf("a\nb\nc");
    Lines left = linesOf("a\nB\nc");
    Lines right = linesOf("a\nB\nc");
    Merged merged = merge(base, left, right);
    checkChunks(base, left, right, merged.chunks);
    CHECK(merged.conflicts == 0);
    CHECK(countOfKind(merged.chunks, PrTextDiffMergeKind_Both) == 1);
    CHECK(merged.text == "a\nB\nc");
}

TEST_CASE("text diff merge: different changes of one line conflict, and the markers hold each text") {
    Lines base = linesOf("a\nb\nc");
    Lines left = linesOf("a\nleft\nc");
    Lines right = linesOf("a\nright\nc");
    PrTextDiffMarkerOptions options = prTextDiffMarkerOptionsDefault();
    options.leftLabel = "ours";
    options.baseLabel = "base";
    options.rightLabel = "theirs";
    Merged merged = merge(base, left, right, {}, &options);
    checkChunks(base, left, right, merged.chunks);
    CHECK(merged.conflicts == 1);
    CHECK(merged.text == "a\n<<<<<<< ours\nleft\n||||||| base\nb\n=======\nright\n>>>>>>> theirs\nc");
    options.style = PrTextDiffMarkerStyle_Merge;
    options.leftLabel = "";
    options.rightLabel = "";
    CHECK(merge(base, left, right, {}, &options).text == "a\n<<<<<<<\nleft\n=======\nright\n>>>>>>>\nc");
}

TEST_CASE("text diff merge: changes that meet in the base conflict, and so do insertions at one place") {
    // The left changes line 1 and the right line 2, which meet.
    Lines base = linesOf("a\nb\nc\nd");
    Merged meeting = merge(base, linesOf("a\nB\nc\nd"), linesOf("a\nb\nC\nd"));
    CHECK(meeting.conflicts == 1);
    // Both insert after line 1, differently.
    Merged inserted = merge(base, linesOf("a\nx\nb\nc\nd"), linesOf("a\ny\nb\nc\nd"));
    CHECK(inserted.conflicts == 1);
    REQUIRE(countOfKind(inserted.chunks, PrTextDiffMergeKind_Conflict) == 1);
    for (const PrTextDiffMergeChunk& chunk : inserted.chunks) {
        if (chunk.kind == PrTextDiffMergeKind_Conflict)
            CHECK(chunk.baseCount == 0);
    }
}

TEST_CASE("text diff merge: a conflict leaves out the lines that begin and end both sides alike") {
    Lines base = linesOf("a\nold\nz");
    Lines left = linesOf("a\nsame 1\nleft\nsame 2\nz");
    Lines right = linesOf("a\nsame 1\nright\nsame 2\nz");
    Merged merged = merge(base, left, right);
    checkChunks(base, left, right, merged.chunks);
    REQUIRE(merged.conflicts == 1);
    CHECK(countOfKind(merged.chunks, PrTextDiffMergeKind_Both) == 2);
    for (const PrTextDiffMergeChunk& chunk : merged.chunks) {
        if (chunk.kind != PrTextDiffMergeKind_Conflict)
            continue;
        CHECK(chunk.baseCount == 1);
        CHECK(chunk.leftCount == 1);
        CHECK(left.lines[chunk.leftStart] == "left");
        CHECK(right.lines[chunk.rightStart] == "right");
    }
    CHECK(merged.text == "a\nsame 1\n<<<<<<<\nleft\n|||||||\nold\n=======\nright\n>>>>>>>\nsame 2\nz");
}

TEST_CASE("text diff merge: a sync point splits a conflict, so that its lines pair") {
    // Without the sync point, the changes meet and are one conflict.
    Lines base = linesOf("a\nb\nc");
    Lines left = linesOf("A\nB\nc");
    Lines right = linesOf("a\nb2\nC");
    Merged plain = merge(base, left, right);
    CHECK(plain.conflicts == 1);
    // With line 1 of each synced, the left's change of line 0 and the right's of line 2 stand apart, and only line 1
    // conflicts.
    Merged synced = merge(base, left, right, {{1, 1, 1}});
    checkChunks(base, left, right, synced.chunks);
    CHECK(synced.conflicts == 1);
    CHECK(countOfKind(synced.chunks, PrTextDiffMergeKind_Left) == 1);
    CHECK(countOfKind(synced.chunks, PrTextDiffMergeKind_Right) == 1);
    for (const PrTextDiffMergeChunk& chunk : synced.chunks) {
        if (chunk.kind != PrTextDiffMergeKind_Conflict)
            continue;
        CHECK(chunk.baseStart == 1);
        CHECK(chunk.baseCount == 1);
        CHECK(chunk.leftCount == 1);
        CHECK(chunk.rightCount == 1);
    }
}

TEST_CASE("text diff merge: merges of random texts cover each text, and their conflicts are found in their text") {
    uint32_t seed = 4242;
    auto next = [&]() {
        seed = seed * 1103515245u + 12345u;
        return seed >> 16;
    };
    auto edited = [&](const Lines& from) {
        Lines out;
        for (const std::string& line : from.lines) {
            uint32_t roll = next() % 10;
            if (roll == 0)
                continue;
            if (roll == 1)
                out.lines.push_back(std::string(1, char('m' + next() % 4)));
            out.lines.push_back(roll == 2 ? std::string(1, char('w' + next() % 4)) : line);
        }
        return out;
    };
    for (uint32_t round = 0; round < 300; ++round) {
        Lines base;
        uint32_t count = next() % 30;
        for (uint32_t i = 0; i < count; ++i)
            base.lines.push_back(std::string(1, char('a' + next() % 6)));
        Lines left = edited(base);
        Lines right = edited(base);
        Merged merged = merge(base, left, right);
        checkChunks(base, left, right, merged.chunks);
        std::vector<PrTextDiffConflict> found = conflictsOf(merged.text);
        REQUIRE(found.size() == merged.conflicts);
        uint32_t k = 0;
        Lines text = linesOf(merged.text);
        for (const PrTextDiffMergeChunk& chunk : merged.chunks) {
            if (chunk.kind != PrTextDiffMergeKind_Conflict)
                continue;
            const PrTextDiffConflict& conflict = found[k++];
            CHECK(conflict.hasBase);
            CHECK(text.slice(conflict.leftStart, conflict.leftCount) == left.slice(chunk.leftStart, chunk.leftCount));
            CHECK(text.slice(conflict.baseStart, conflict.baseCount) == base.slice(chunk.baseStart, chunk.baseCount));
            CHECK(text.slice(conflict.rightStart, conflict.rightCount) ==
                right.slice(chunk.rightStart, chunk.rightCount));
        }
    }
}

TEST_CASE("text diff merge: conflicts are found by their markers, and markers that do not close one are text") {
    std::vector<PrTextDiffConflict> found = conflictsOf(
        "a\n<<<<<<< ours\nl\n=======\nr1\nr2\n>>>>>>> theirs\nb\n<<<<<<<\nnever closed\n<<<<<<<\nx\n|||||||\n"
            "base\n=======\n>>>>>>>\n<<<<<<<< too long\n=======\n>>>>>>>");
    REQUIRE(found.size() == 2);
    CHECK(found[0].start == 1);
    CHECK(found[0].end == 7);
    CHECK_FALSE(found[0].hasBase);
    CHECK(found[0].leftStart == 2);
    CHECK(found[0].leftCount == 1);
    CHECK(found[0].rightStart == 4);
    CHECK(found[0].rightCount == 2);
    CHECK(found[1].start == 10);
    CHECK(found[1].hasBase);
    CHECK(found[1].leftCount == 1);
    CHECK(found[1].baseStart == 13);
    CHECK(found[1].baseCount == 1);
    CHECK(found[1].rightCount == 0);
    CHECK(found[1].end == 16);
}
