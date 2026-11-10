// Menus and status items: the contract around each backend's functions.

#include "pr/app_menu.h"

#include "pr/app_backend.h"
#include "pr/assert.h"

namespace pr_impl {

namespace {

// Checks that each menu of the tree lists its items.
void checkMenu(const PrAppMenu& menu) {
    PR_ASSERT(menu.items != nullptr || menu.count == 0);
    for (uint32_t i = 0; i < menu.count; ++i) {
        if (menu.items[i].submenu != nullptr)
            checkMenu(*menu.items[i].submenu);
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

void prAppSetMenuBar(PrApp* app, const PrAppMenu* PR_NULLABLE menu) {
    if (menu != nullptr)
        checkMenu(*menu);
    if (app->setMenuBarFn != nullptr)
        app->setMenuBarFn(app, menu);
}

void prAppWindowSetMenuBar(PrAppWindow* window, const PrAppMenu* PR_NULLABLE menu) {
    if (menu != nullptr)
        checkMenu(*menu);
    if (window->setMenuBarFn != nullptr)
        window->setMenuBarFn(window, menu);
}

void prAppWindowShowMenu(PrAppWindow* window, const PrAppMenu* menu, PrVec2 position) {
    checkMenu(*menu);
    if (window->showMenuFn != nullptr)
        window->showMenuFn(window, menu, position);
}

PrStatus prAppStatusItemCreate(PrApp* app, const PrAppStatusItemDesc* desc, PrAppStatusItem* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (desc->menu != nullptr)
        checkMenu(*desc->menu);
    if (app->statusItemCreateFn == nullptr)
        return PrStatus_NotSupported;
    return app->statusItemCreateFn(app, desc, out);
}

void prAppStatusItemDestroy(PrAppStatusItem* PR_NULLABLE item) {
    if (item != nullptr)
        item->destroyFn(item);
}

void prAppStatusItemSet(PrAppStatusItem* item, const PrAppStatusItemDesc* desc) {
    if (desc->menu != nullptr)
        checkMenu(*desc->menu);
    item->setFn(item, desc);
}
