#include "pr/app.h"
#include "pr/app_surface.h"
#include "pr/app_user_preferences.h"

int prTestAppCApi(void);

// The public header compiled as C. Checks the structures a handler receives.
int prTestAppCApi(void) {
    PrAppEvent event = {0};
    event.kind = PrAppEventKind_KeyDown;
    event.key = PrAppKey_Escape;
    event.code = PrAppKeyCode_Escape;
    event.modifiers = PrAppModifierFlag_Shift | PrAppModifierFlag_Super;
    PrAppTextRange selection = {1, 3};
    event.selection = selection;
    PrAppFrame frame = {.extent = {10, 20}, .display = {.pixelRatio = 2, .headroom = 1, .potentialHeadroom = 1}};
    prAppDestroy(NULL);
    return event.key != PrAppKey_Escape || event.selection.end != 3 || prAppKeyCodeName(event.code).size != 6 ||
        frame.display.pixelRatio != 2 || (event.modifiers & 8) == 0;
}
