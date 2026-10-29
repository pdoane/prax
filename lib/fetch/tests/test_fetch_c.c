#include "pr/fetch_builtin.h"

int prTestFetchCApi(void);

// The public headers compiled as C. Destroying nothing does nothing.
int prTestFetchCApi(void) {
    prFetchNetworkDestroy(NULL);
    prFetchDestroy(NULL);
    return 0;
}
