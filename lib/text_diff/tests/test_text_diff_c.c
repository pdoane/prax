#include "pr/text_diff.h"
#include "pr/text_diff_merge.h"

int prTestTextDiffCApi(void);

// The public headers compiled as C. A diff of two empty texts has no changes, and a merge of three has no chunks.
int prTestTextDiffCApi(void) {
    PrTextDiff* diff = prTextDiffCreate(NULL);
    PrTextDiffText empty = {0};
    prTextDiffCompare(diff, &empty, &empty, NULL, 0);
    uint32_t count = prTextDiffChangeCount(diff) + prTextDiffLineChangeCount(diff);
    prTextDiffDestroy(diff);
    prTextDiffDestroy(NULL);
    PrTextDiffMerge* merge = prTextDiffMergeCreate(NULL);
    prTextDiffMergeCompare(merge, &empty, &empty, &empty, NULL, 0);
    count += prTextDiffMergeChunkCount(merge) + prTextDiffConflicts(&empty, NULL, 0);
    prTextDiffMergeDestroy(merge);
    prTextDiffMergeDestroy(NULL);
    return count == 0 ? 0 : 1;
}
