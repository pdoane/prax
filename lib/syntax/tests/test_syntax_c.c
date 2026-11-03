#include "pr/syntax.h"

int prTestSyntaxCApi(void);

// The public header compiles as C, and a registry interns a capture name.
int prTestSyntaxCApi(void) {
    PrSyntaxRegistry* registry = prSyntaxRegistryCreate();
    PrSyntaxCategory capture = prSyntaxRegistryCategoryIntern(registry, PR_SV("keyword"));
    uint32_t count = prSyntaxRegistryCategoryCount(registry);
    prSyntaxRegistryDestroy(registry);
    return capture == 0 && count == 1 ? 0 : 1;
}
