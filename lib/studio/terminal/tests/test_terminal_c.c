#include "pr/terminal.h"

int prTestTerminalCApi(void);

// Compiles the public header as C. Destroying a null terminal does nothing.
int prTestTerminalCApi(void) {
    PrTerminalDesc desc = {0};
    desc.scrollback = kPrTerminalScrollbackDefault;
    prTerminalDestroy(NULL);
    return desc.argumentCount == 0 ? 0 : 1;
}
