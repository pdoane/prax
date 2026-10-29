#include "pr/net_sockets.h"

int prTestNetCApi(void);

// The public headers compiled as C. A network starts its loop and stops it.
int prTestNetCApi(void) {
    PrNet* net = prNetSocketsCreate();
    prNetDestroy(net);
    prNetDestroy(NULL);
    prNetStreamDestroy(NULL);
    return 0;
}
