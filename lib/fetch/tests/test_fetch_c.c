#include "pr/fetch_builtin.h"
#include "pr/fetch_load.h"

int prTestFetchCApi(void);

// The public headers compiled as C. Destroying nothing does nothing.
int prTestFetchCApi(void) {
    prFetchNetworkDestroy(NULL);
    prFetchDestroy(NULL);
    prFetchLoadDestroy(NULL);
    return 0;
}
