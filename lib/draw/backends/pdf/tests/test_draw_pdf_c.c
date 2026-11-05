#include "pr/draw_pdf.h"

int prTestDrawPdfCApi(void);

static void discard(void* context, uint32_t output, PrByteView bytes) {
    (void)context;
    (void)output;
    (void)bytes;
}

// Tests that the public header compiles as C. An empty document is written.
int prTestDrawPdfCApi(void) {
    PrDrawWriter* writer = prDrawPdfCreate(NULL);
    PrDrawDocumentDesc desc = {.write = discard};
    prDrawWriterWrite(writer, &desc, NULL, 0);
    prDrawWriterDestroy(writer);
    return 0;
}
