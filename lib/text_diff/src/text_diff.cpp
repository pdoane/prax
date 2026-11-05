// Text diffs (text_diff.h): lines compared by their hashes and then their bytes, and the characters of each pair of
// changed lines compared in turn, each by its bytes.

#include "pr/text_diff.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/mem.h"
#include "pr/utf8.h"

#include "text_diff/src/text_diff_sequence.h"

namespace pr_impl {

namespace {

// A side of a comparison.
enum {
    TextDiffSide_Old,
    TextDiffSide_New,
    TextDiffSide_Count,
};
typedef uint8_t TextDiffSide;

struct TextDiff {
    PrTextDiffOptions options;
    TextDiffSequence sequence;
    RawArray<PrTextDiffChange> changes;
    RawArray<PrTextDiffLineChange> lineChanges;

    // By side: the keys of the lines, or of the characters of a line, and the byte offset of each character, with the
    // line's size last.
    RawArray<uint64_t> keys[TextDiffSide_Count];
    RawArray<uint32_t> offsets[TextDiffSide_Count];
    RawArray<PrTextDiffChange> characters;                       // the changes of characters of a pair of lines
    const PrTextDiffText* PR_NULLABLE texts[TextDiffSide_Count]; // during a comparison
};

// Writes a key of each character of `line` to `keys`, which its bytes make, and the offset of each to `offsets`, with
// the line's size last. An ill-formed byte is a character of its own.
void keyCharacters(PrStringView line, RawArray<uint64_t>& keys, RawArray<uint32_t>& offsets) {
    keys.clear();
    offsets.clear();
    for (size_t at = 0; at < line.size;) {
        uint32_t codePoint = 0;
        uint32_t size = prUtf8Decode(line, at, &codePoint);
        uint64_t key = uint64_t(size) << 32;
        for (uint32_t i = 0; i < size; ++i)
            key |= uint64_t(uint8_t(line.data[at + i])) << (8 * i);
        keys.push(key);
        offsets.push(uint32_t(at));
        at += size;
    }
    offsets.push(uint32_t(line.size));
}

// Adds the changes within `oldLine` and `newLine`.
void compareLines(TextDiff* diff, uint32_t oldLine, uint32_t newLine) {
    const PrTextDiffText* old = diff->texts[TextDiffSide_Old];
    const PrTextDiffText* young = diff->texts[TextDiffSide_New];
    PrStringView older = old->line(old->context, oldLine);
    PrStringView newer = young->line(young->context, newLine);
    if (older.size == newer.size && prMemEquals(older.data, newer.data, older.size))
        return;
    if (older.size > diff->options.maxLineBytes || newer.size > diff->options.maxLineBytes) {
        diff->lineChanges.push(PrTextDiffLineChange{
            .oldLine = oldLine,
            .newLine = newLine,
            .bytes = {0, uint32_t(older.size), 0, uint32_t(newer.size)},
        });
        return;
    }
    keyCharacters(older, diff->keys[TextDiffSide_Old], diff->offsets[TextDiffSide_Old]);
    keyCharacters(newer, diff->keys[TextDiffSide_New], diff->offsets[TextDiffSide_New]);
    TextDiffSequenceDesc desc{
        .oldKeys = diff->keys[TextDiffSide_Old].data,
        .oldCount = diff->keys[TextDiffSide_Old].count,
        .newKeys = diff->keys[TextDiffSide_New].data,
        .newCount = diff->keys[TextDiffSide_New].count,
    };
    diff->characters.clear();
    TextDiffPart whole{0, desc.oldCount, 0, desc.newCount};
    textDiffSequenceCompare(&diff->sequence, desc, whole, diff->characters);
    const uint32_t* oldOffsets = diff->offsets[TextDiffSide_Old].data;
    const uint32_t* newOffsets = diff->offsets[TextDiffSide_New].data;
    for (uint32_t i = 0; i < diff->characters.count; ++i) {
        const PrTextDiffChange& change = diff->characters.data[i];
        uint32_t oldEnd = change.oldStart + change.oldCount;
        uint32_t newEnd = change.newStart + change.newCount;
        diff->lineChanges.push(PrTextDiffLineChange{
            .oldLine = oldLine,
            .newLine = newLine,
            .bytes = {
                oldOffsets[change.oldStart], oldOffsets[oldEnd] - oldOffsets[change.oldStart],
                newOffsets[change.newStart], newOffsets[newEnd] - newOffsets[change.newStart],
            },
        });
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrTextDiff : TextDiff {};

PrTextDiffOptions prTextDiffOptionsDefault(void) {
    return PrTextDiffOptions{.maxCost = 4096, .maxLineBytes = 4096};
}

PrTextDiff* prTextDiffCreate(const PrTextDiffOptions* PR_NULLABLE options) {
    PrTextDiff* diff = allocZeroed<PrTextDiff>();
    diff->options = options != nullptr ? *options : prTextDiffOptionsDefault();
    diff->sequence.maxCost = diff->options.maxCost != 0 ? diff->options.maxCost : 1;
    return diff;
}

void prTextDiffDestroy(PrTextDiff* PR_NULLABLE diff) {
    if (diff == nullptr)
        return;
    textDiffSequenceReset(&diff->sequence);
    diff->changes.reset();
    diff->lineChanges.reset();
    for (uint32_t side = 0; side < TextDiffSide_Count; ++side) {
        diff->keys[side].reset();
        diff->offsets[side].reset();
    }
    diff->characters.reset();
    prMemoryFree(diff);
}

void prTextDiffCompare(PrTextDiff* diff, const PrTextDiffText* old, const PrTextDiffText* young,
        const PrTextDiffSync* PR_NULLABLE syncs, uint32_t syncCount) {
    diff->texts[TextDiffSide_Old] = old;
    diff->texts[TextDiffSide_New] = young;
    textDiffLineKeys(old, diff->keys[TextDiffSide_Old]);
    textDiffLineKeys(young, diff->keys[TextDiffSide_New]);
    TextDiffLines lines{old, young};
    TextDiffSequenceDesc desc{
        .oldKeys = diff->keys[TextDiffSide_Old].data,
        .oldCount = diff->keys[TextDiffSide_Old].count,
        .newKeys = diff->keys[TextDiffSide_New].data,
        .newCount = diff->keys[TextDiffSide_New].count,
        .equal = textDiffSameLines,
        .equalContext = &lines,
    };
    // The texts between sync points compare apart, and a sync point's lines pair as one change if they differ.
    diff->changes.clear();
    uint32_t oldAt = 0;
    uint32_t newAt = 0;
    for (uint32_t i = 0; i <= syncCount; ++i) {
        PrTextDiffSync sync{old->lineCount, young->lineCount};
        if (i < syncCount) {
            sync = syncs[i];
            PR_ASSERT(sync.oldLine >= oldAt && sync.oldLine < old->lineCount);
            PR_ASSERT(sync.newLine >= newAt && sync.newLine < young->lineCount);
        }
        textDiffSequenceCompare(&diff->sequence, desc, TextDiffPart{oldAt, sync.oldLine, newAt, sync.newLine},
            diff->changes);
        if (i == syncCount)
            break;
        bool same = desc.oldKeys[sync.oldLine] == desc.newKeys[sync.newLine] &&
            textDiffSameLines(&lines, sync.oldLine, sync.newLine);
        if (!same)
            diff->changes.push(PrTextDiffChange{sync.oldLine, 1, sync.newLine, 1});
        oldAt = sync.oldLine + 1;
        newAt = sync.newLine + 1;
    }
    diff->lineChanges.clear();
    if (diff->options.maxLineBytes != 0) {
        for (uint32_t i = 0; i < diff->changes.count; ++i) {
            PrTextDiffChange change = diff->changes.data[i];
            uint32_t paired = change.oldCount < change.newCount ? change.oldCount : change.newCount;
            for (uint32_t k = 0; k < paired; ++k)
                compareLines(diff, change.oldStart + k, change.newStart + k);
        }
    }
    diff->texts[TextDiffSide_Old] = nullptr;
    diff->texts[TextDiffSide_New] = nullptr;
}

uint32_t prTextDiffChangeCount(const PrTextDiff* diff) {
    return diff->changes.count;
}

const PrTextDiffChange* PR_NULLABLE prTextDiffChanges(const PrTextDiff* diff) {
    return diff->changes.data;
}

uint32_t prTextDiffLineChangeCount(const PrTextDiff* diff) {
    return diff->lineChanges.count;
}

const PrTextDiffLineChange* PR_NULLABLE prTextDiffLineChanges(const PrTextDiff* diff) {
    return diff->lineChanges.data;
}
