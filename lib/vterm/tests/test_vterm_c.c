#include "pr/vterm.h"
#include "pr/vterm_graphics.h"
#include "pr/vterm_input.h"

int prTestVtermCApi(void);

// Compiles the public headers as C. A character written to a terminal lands in the first cell.
int prTestVtermCApi(void) {
    PrVtermDesc desc = {.extent = {4, 2}, .scrollback = 8};
    PrVterm* vterm = prVtermCreate(&desc);
    const uint8_t text[] = {'x'};
    prVtermWrite(vterm, (PrByteView){text, 1});
    PrVtermLine line = prVtermLine(vterm, 0);
    int failed = line.cellCount != 4 || line.cells[0].codePoint != 'x';
    prVtermInputFocus(vterm, true);
    failed = failed || prVtermPlacements(vterm, 0, 2, NULL, 0) != 0;
    prVtermDestroy(vterm);
    prVtermDestroy(NULL);
    return failed;
}
