#include "pr/studio_app.h"
#include "pr/studio_window.h"

int prTestStudioAppCApi(void);

// The public headers compiled as C. The descriptions of an application and a window name their callbacks.
int prTestStudioAppCApi(void) {
    PrStudioWindowDesc window = {0};
    PrStudioAppDesc app = {.window = &window, .lastWindow = PrStudioAppLastWindow_Run};
    return window.build == NULL && window.open == NULL && app.start == NULL && app.window == &window ? 0 : 1;
}
