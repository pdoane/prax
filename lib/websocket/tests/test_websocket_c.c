#include "pr/websocket.h"
#include "pr/websocket_builtin.h"

int prTestWebSocketCApi(void);

// The public headers compiled as C. Destroying nothing does nothing, and the backends name themselves.
int prTestWebSocketCApi(void) {
    prWebSocketNetworkDestroy(NULL);
    prWebSocketDestroy(NULL);
    int result = prWebSocketNetworkBackendFromName(PR_SV("builtin")) != PrWebSocketNetworkBackend_Builtin;
    result |= !prWebSocketNetworkBackendIsBuilt(PrWebSocketNetworkBackend_Default);
    return result;
}
