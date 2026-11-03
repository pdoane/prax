// The comparison of sequences (text_diff_sequence.h): Myers's search for the middle snake, which splits each part of
// the comparison in two, over a stack of parts in place of recursion.

#include "text_diff/src/text_diff_sequence.h"

#include "pr/assert.h"

namespace pr_impl {

namespace {

// The middle snake of a part, from (`x0`, `y0`) to (`x1`, `y1`) in the part's own coordinates.
struct DiffSnake {
    uint32_t x0;
    uint32_t y0;
    uint32_t x1;
    uint32_t y1;
};

bool same(const TextDiffSequenceDesc& desc, uint32_t a, uint32_t b) {
    if (desc.oldKeys[a] != desc.newKeys[b])
        return false;
    return desc.equal == nullptr || desc.equal(desc.equalContext, a, b);
}

// Adds a change, joined to the last if they meet.
void addChange(Array<PrTextDiffChange>& changes, uint32_t oldStart, uint32_t oldCount, uint32_t newStart,
        uint32_t newCount) {
    if (oldCount == 0 && newCount == 0)
        return;
    if (changes.count != 0) {
        PrTextDiffChange& last = changes.last();
        if (last.oldStart + last.oldCount == oldStart && last.newStart + last.newCount == newStart) {
            last.oldCount += oldCount;
            last.newCount += newCount;
            return;
        }
    }
    changes.push(PrTextDiffChange{oldStart, oldCount, newStart, newCount});
}

// Finds the middle snake of a part whose ends differ, within the cost. Returns false if the part's difference is past
// the cost.
bool middleSnake(TextDiffSequence* sequence, const TextDiffSequenceDesc& desc, const TextDiffPart& part, DiffSnake* out) {
    int64_t n = int64_t(part.oldEnd - part.oldStart);
    int64_t m = int64_t(part.newEnd - part.newStart);
    int64_t delta = n - m;
    bool odd = (delta & 1) != 0;
    int64_t limit = (n + m + 1) / 2;
    limit = limit < int64_t(sequence->maxCost) ? limit : int64_t(sequence->maxCost);
    int64_t offset = limit + 1;
    uint32_t size = uint32_t(2 * limit + 3);
    sequence->forward.resize(size);
    sequence->backward.resize(size);
    int64_t* vf = sequence->forward.data;
    int64_t* vb = sequence->backward.data;
    vf[offset + 1] = 0;
    vb[offset + 1] = 0;
    for (int64_t d = 0; d <= limit; ++d) {
        for (int64_t k = -d; k <= d; k += 2) {
            // The furthest reach on diagonal k comes down from k + 1 or across from k - 1.
            int64_t x = vf[offset + k - 1] + 1;
            if (k == -d || (k != d && vf[offset + k - 1] < vf[offset + k + 1]))
                x = vf[offset + k + 1];
            int64_t y = x - k;
            int64_t x0 = x;
            int64_t y0 = y;
            while (x < n && y < m && same(desc, part.oldStart + uint32_t(x), part.newStart + uint32_t(y))) {
                ++x;
                ++y;
            }
            vf[offset + k] = x;
            int64_t back = delta - k;
            if (odd && back >= -(d - 1) && back <= d - 1 && x + vb[offset + back] >= n) {
                *out = DiffSnake{uint32_t(x0), uint32_t(y0), uint32_t(x), uint32_t(y)};
                return true;
            }
        }
        for (int64_t k = -d; k <= d; k += 2) {
            // The furthest reach on diagonal k comes down from k + 1 or across from k - 1.
            int64_t x = vb[offset + k - 1] + 1;
            if (k == -d || (k != d && vb[offset + k - 1] < vb[offset + k + 1]))
                x = vb[offset + k + 1];
            int64_t y = x - k;
            int64_t x0 = x;
            int64_t y0 = y;
            while (x < n && y < m && same(desc, part.oldEnd - 1 - uint32_t(x), part.newEnd - 1 - uint32_t(y))) {
                ++x;
                ++y;
            }
            vb[offset + k] = x;
            int64_t front = delta - k;
            if (!odd && front >= -d && front <= d && x + vf[offset + front] >= n) {
                *out = DiffSnake{uint32_t(n - x), uint32_t(m - y), uint32_t(n - x0), uint32_t(m - y0)};
                return true;
            }
        }
    }
    return false;
}

} // namespace

void textDiffSequenceReset(TextDiffSequence* sequence) {
    sequence->parts.reset();
    sequence->forward.reset();
    sequence->backward.reset();
}

void textDiffSequenceCompare(TextDiffSequence* sequence, const TextDiffSequenceDesc& desc, Array<PrTextDiffChange>& changes) {
    PR_ASSERT(desc.oldCount == 0 || desc.oldKeys != nullptr);
    PR_ASSERT(desc.newCount == 0 || desc.newKeys != nullptr);
    PR_ASSERT(sequence->maxCost != 0);
    changes.clear();
    sequence->parts.push(TextDiffPart{0, desc.oldCount, 0, desc.newCount});
    while (sequence->parts.count != 0) {
        TextDiffPart part = sequence->parts.last();
        sequence->parts.pop();
        // What the part's ends share is equal, so the search starts and ends where the elements differ.
        while (part.oldStart < part.oldEnd && part.newStart < part.newEnd && same(desc, part.oldStart, part.newStart)) {
            ++part.oldStart;
            ++part.newStart;
        }
        while (part.oldStart < part.oldEnd && part.newStart < part.newEnd && same(desc, part.oldEnd - 1, part.newEnd - 1)) {
            --part.oldEnd;
            --part.newEnd;
        }
        uint32_t oldCount = part.oldEnd - part.oldStart;
        uint32_t newCount = part.newEnd - part.newStart;
        DiffSnake snake{};
        if (oldCount == 0 || newCount == 0 || !middleSnake(sequence, desc, part, &snake)) {
            addChange(changes, part.oldStart, oldCount, part.newStart, newCount);
            continue;
        }
        // The part before the snake is compared first, so the stack holds the part after it beneath.
        Array<TextDiffPart>& parts = sequence->parts;
        parts.push(TextDiffPart{part.oldStart + snake.x1, part.oldEnd, part.newStart + snake.y1, part.newEnd});
        parts.push(TextDiffPart{part.oldStart, part.oldStart + snake.x0, part.newStart, part.newStart + snake.y0});
    }
}

} // namespace pr_impl
