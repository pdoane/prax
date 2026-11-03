#include "pr/lsp.h"

int prTestLspCApi(void);

static void dropBytes(void* context, PrByteView bytes) {
    (void)context;
    (void)bytes;
}

// The public headers compiled as C. A client writes its initialize request as it is created.
int prTestLspCApi(void) {
    PrLspClientDesc desc = {0};
    desc.write = dropBytes;
    PrLspClient* client = prLspClientCreate(&desc);
    int state = prLspClientState(client);
    prLspClientDestroy(client);
    prLspClientDestroy(NULL);
    prLspDocumentDestroy(NULL);
    return state == PrLspClientState_Initializing ? 0 : 1;
}
