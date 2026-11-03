#include "pr/text_diff.h"

int prTestTextDiffCApi(void);

// The public header compiled as C. A diff of two empty texts has no changes.
int prTestTextDiffCApi(void) {
    PrTextDiff* diff = prTextDiffCreate(NULL);
    PrTextDiffText empty = {0};
    prTextDiffCompare(diff, &empty, &empty);
    uint32_t count = prTextDiffChangeCount(diff) + prTextDiffLineChangeCount(diff);
    prTextDiffDestroy(diff);
    prTextDiffDestroy(NULL);
    return count == 0 ? 0 : 1;
}
