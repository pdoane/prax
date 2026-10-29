#include "pr/http_builtin.h"

int prTestHttpCApi(void);

// The public headers compiled as C. Destroying nothing does nothing.
int prTestHttpCApi(void) {
    prHttpDestroy(NULL);
    prHttpExchangeDestroy(NULL);
    return 0;
}
