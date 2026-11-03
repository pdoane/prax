#include "pr/dap.h"

int prTestDapCApi(void);

static void dropBytes(void* context, PrByteView bytes) {
    (void)context;
    (void)bytes;
}

// The public headers compiled as C. A client writes its initialize request as it is created.
int prTestDapCApi(void) {
    PrDapClientDesc desc = {0};
    desc.adapterId = PR_SV("test");
    desc.write = dropBytes;
    PrDapClient* client = prDapClientCreate(&desc);
    int state = prDapClientState(client);
    prDapClientDestroy(client);
    prDapClientDestroy(NULL);
    return state == PrDapClientState_Initializing ? 0 : 1;
}
