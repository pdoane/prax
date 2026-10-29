// Checks that the public headers compile and link as C.

#include "pr/xhr.h"

int prTestXhrCApi(void);

int prTestXhrCApi(void) {
    PrXhrProgressEventInit init = prXhrProgressEventInitDefault();
    init.loaded = 1;
    PrXhrProgressEvent* event = prXhrProgressEventCreate(PR_SV("progress"), &init);
    PrDomEvent* base = prXhrProgressEventAsEvent(event);
    int failures = prXhrProgressEventQuery(base) == event ? 0 : 1;
    failures += prXhrProgressEventLoaded(event) == 1 ? 0 : 1;
    failures += prXhrProgressEventLengthComputable(event) ? 1 : 0;
    prDomEventRelease(base);
    return failures;
}
