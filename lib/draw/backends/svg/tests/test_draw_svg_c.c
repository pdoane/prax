#include "pr/draw_svg.h"

int prTestDrawSvgCApi(void);

static void discard(void* context, uint32_t output, PrByteView bytes) {
    (void)context;
    (void)output;
    (void)bytes;
}

// Tests that the public header compiles as C. An empty document is written.
int prTestDrawSvgCApi(void) {
    PrDrawWriter* writer = prDrawSvgCreate(NULL);
    PrDrawDocumentDesc desc = {.write = discard};
    prDrawWriterWrite(writer, &desc, NULL, 0);
    prDrawWriterDestroy(writer);
    return 0;
}
