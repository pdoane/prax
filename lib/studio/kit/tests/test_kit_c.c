#include "pr/kit.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_choice.h"
#include "pr/kit_collection.h"
#include "pr/kit_data.h"
#include "pr/kit_dock.h"
#include "pr/kit_icon.h"
#include "pr/kit_input.h"
#include "pr/kit_menu.h"
#include "pr/kit_surface.h"
#include "pr/kit_text.h"
#include "pr/kit_theme.h"

int prTestKitCApi(void);

// The public headers compile as C, and a kit is created and destroyed for a tree with no text.
int prTestKitCApi(void) {
    PrLayoutTreeDesc desc = {.targetPixelRatio = 1};
    PrLayoutTree* tree = prLayoutTreeCreate(&desc);
    PrUi* ui = prUiCreate(tree);
    PrKitTheme theme = prKitThemeStudio(PrKitThemeVariant_Light, NULL);
    PrKit* kit = prKitCreate(ui, &theme);
    PrKitPlace place = {.width = prKitPx(10), .height = prKitFill()};
    int result = place.width.kind != PrKitSizeKind_Px || prKitTheme(kit)->types[PrKitTextRole_Caption].mono != true;
    prKitDestroy(kit);
    prUiDestroy(ui);
    prLayoutTreeDestroy(tree);
    return result;
}
