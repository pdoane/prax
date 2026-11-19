#include "pr/app.h"
#include "pr/app_user_preferences.h"
#include "pr/surface.h"

int prTestAppCApi(void);

// The public header compiled as C. Checks the structures a handler receives, and that C reaches the union's members by
// their own names.
int prTestAppCApi(void) {
    PrAppEvent key =
        {.kind = PrAppEventKind_KeyDown, .keyboard = {.key = PrAppKey_Escape, .code = PrAppKeyCode_Escape}};
    key.modifiers = PrAppModifierFlag_Shift | PrAppModifierFlag_Super;
    PrAppTextRange selection = {1, 3};
    PrAppEvent update =
        {.kind = PrAppEventKind_CompositionUpdate, .input = {.text = PR_SV("abc"), .selection = selection}};
    PrAppFrame frame = {.extent = {10, 20}, .display = {.pixelRatio = 2, .headroom = 1, .potentialHeadroom = 1}};
    prAppDestroy(NULL);
    return key.keyboard.key != PrAppKey_Escape || update.input.selection.end != 3 ||
        prAppKeyCodeName(key.keyboard.code).size != 6 || prAppEventText(&update).size != 3 ||
        frame.display.pixelRatio != 2 || (key.modifiers & 8) == 0;
}
