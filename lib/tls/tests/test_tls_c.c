#include "pr/tls.h"

int prTestTlsCApi(void);

// The public headers compiled as C. A session is its stream.
int prTestTlsCApi(void) {
    PrTlsSession* session = NULL;
    PrNetStream* stream = prTlsSessionAsStream(session);
    prTlsDestroy(NULL);
    prNetStreamDestroy(stream);
    return stream != NULL;
}
