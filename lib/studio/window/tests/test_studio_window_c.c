#include "pr/studio_window.h"

int prTestStudioWindowCApi(void);

// The public header compiled as C. A description of a window names its callbacks.
int prTestStudioWindowCApi(void) {
    PrStudioWindowDesc desc = {0};
    return desc.build == NULL && desc.open == NULL ? 0 : 1;
}
