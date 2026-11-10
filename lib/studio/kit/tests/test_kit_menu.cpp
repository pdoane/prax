#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_menu.h"
#include "pr/kit_text.h"

#include "tools/harness/kit_fixture.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

namespace {

void secondaryClick(KitFixture& f, PrVec2 at) {
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .pointer = {.position = at, .button = 1}});
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .pointer = {.position = at, .button = 1}});
}

bool inside(PrRect outer, PrRect inner) {
    return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right &&
        inner.bottom <= outer.bottom;
}

// A button that opens a popover with a menu of three items, the last of them disabled, beside a button that takes
// presses.
struct MenuScene {
    KitFixture& f;
    PrLayoutNode trigger = kPrLayoutNodeNone;
    PrLayoutNode other = kPrLayoutNodeNone;
    PrLayoutNode popover = kPrLayoutNodeNone;
    PrLayoutNode menu = kPrLayoutNodeNone;
    PrLayoutNode items[3] = {};
    uint32_t runs[3] = {};
    bool open = false;

    explicit MenuScene(KitFixture& fixture) : f(fixture) {
        f.build = [this](PrLayoutNode root) { build(root); };
    }

    void build(PrLayoutNode root) {
        PrKit* kit = f.kit;
        PrLayoutNode row = prKitBox(kit, root, {}, {.direction = PrKitDirection_Row, .gap = kPrKitSpace4});
        PrKitButton content{.text = "File"};
        trigger = prKitButton(kit, row, content, {});
        if (prKitActivated(kit, trigger))
            prKitToggleOpen(kit, trigger);
        PrKitButton otherContent{.text = "Other"};
        other = prKitButton(kit, row, otherContent, {});
        popover = prKitPopover(kit, trigger, {}, {});
        menu = kPrLayoutNodeNone;
        for (PrLayoutNode& item : items)
            item = kPrLayoutNodeNone;
        if (popover != kPrLayoutNodeNone) {
            PrKitMenuStyle style{.minWidth = 200};
            menu = prKitMenu(kit, popover, {}, style);
            prKitMenuLabel(kit, menu, {.text = "SCENE"});
            PrKitMenuItem newScene{.text = "New Scene", .shortcut = "Cmd N", .icon = PrKitIconKind_Plus};
            items[0] = prKitMenuItem(kit, menu, newScene, {});
            prKitMenuSeparator(kit, menu, {});
            PrKitMenuItem grid{.text = "Grid", .checked = true};
            items[1] = prKitMenuItem(kit, menu, grid, {});
            PrKitMenuItem quit{.text = "Delete", .disabled = true};
            PrKitMenuItemStyle danger{.variant = PrKitMenuItemVariant_Danger};
            items[2] = prKitMenuItem(kit, menu, quit, danger);
            for (uint32_t i = 0; i < 3; ++i)
                runs[i] += prKitActivated(kit, items[i]) ? 1 : 0;
        }
        open = prKitIsOpen(kit, trigger);
    }

    void openMenu() {
        f.frame();
        f.click(f.center(trigger));
        f.frame();
        REQUIRE(open);
        REQUIRE(popover != kPrLayoutNodeNone);
        f.frame();
    }
};

// A menu bar of three menus, each with one item.
struct BarScene {
    KitFixture& f;
    PrLayoutNode items[3] = {};
    bool open[3] = {};
    PrLayoutNode popovers[3] = {};

    explicit BarScene(KitFixture& fixture) : f(fixture) {
        f.build = [this](PrLayoutNode root) {
            PrLayoutNode bar = prKitMenuBar(f.kit, root, {});
            const char* names[] = {"File", "Edit", "View"};
            for (uint32_t i = 0; i < 3; ++i) {
                items[i] = prKitMenuBarItem(f.kit, bar, {.text = names[i]});
                popovers[i] = prKitPopover(f.kit, items[i], {}, {});
                if (popovers[i] != kPrLayoutNodeNone) {
                    PrLayoutNode menu = prKitMenu(f.kit, popovers[i], {}, {});
                    PrKitMenuItem item{.text = names[i]};
                    prKitMenuItem(f.kit, menu, item, {});
                }
                open[i] = prKitIsOpen(f.kit, items[i]);
            }
        };
    }
};

// A button that opens a modal of two buttons, over a root with one more button.
struct ModalScene {
    KitFixture& f;
    PrLayoutNode trigger = kPrLayoutNodeNone;
    PrLayoutNode content = kPrLayoutNodeNone;
    PrLayoutNode buttons[2] = {};
    bool open = false;

    explicit ModalScene(KitFixture& fixture) : f(fixture) {
        f.build = [this](PrLayoutNode root) {
            PrLayoutNode row = prKitBox(f.kit, root, {}, {.direction = PrKitDirection_Row, .gap = 0});
            PrKitButton label{.text = "Open"};
            trigger = prKitButton(f.kit, row, label, {});
            if (prKitActivated(f.kit, trigger))
                prKitSetOpen(f.kit, trigger, true);
            content = prKitModal(f.kit, trigger, {}, {});
            if (content != kPrLayoutNodeNone) {
                PrLayoutNode panel =
                    prKitBox(f.kit, content, {}, {.direction = PrKitDirection_Row, .gap = kPrKitSpace4});
                PrKitButton ok{.text = "OK"};
                buttons[0] = prKitButton(f.kit, panel, ok, {});
                PrKitButton cancel{.text = "Cancel"};
                buttons[1] = prKitButton(f.kit, panel, cancel, {});
            }
            open = prKitIsOpen(f.kit, trigger);
        };
    }

    void openModal() {
        f.frame();
        f.click(f.center(trigger));
        f.frame();
        f.frame();
        REQUIRE(content != kPrLayoutNodeNone);
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a click on a trigger opens its popover in the overlay at the trigger's bottom left") {
    KitFixture f;
    MenuScene scene(f);
    f.frame();
    CHECK(scene.popover == kPrLayoutNodeNone);
    scene.openMenu();
    PrRect trigger = f.bounds(scene.trigger);
    PrRect popover = f.bounds(scene.popover);
    CHECK(popover.left == trigger.left);
    CHECK(popover.top == trigger.bottom);
    CHECK(popover.right - popover.left >= 200.0f);
    CHECK(inside(f.bounds(scene.popover), f.bounds(scene.items[0])));
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a select opens its menu, an option's activation closes it, and Up and Down report a step") {
    KitFixture f;
    uint32_t chosen = 1;
    PrLayoutNode field = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        const PrStringView names[] = {"Transform", "Collider", "Camera"};
        field = prKitSelect(f.kit, root, {.label = names[chosen]}, {});
        int32_t step = 0;
        if (prKitStepped(f.kit, field, &step))
            chosen = uint32_t(int32_t(chosen) + step);
        PrLayoutNode menu = prKitSelectMenu(f.kit, field);
        if (menu == kPrLayoutNodeNone)
            return;
        for (uint32_t i = 0; i < 3; ++i) {
            if (prKitActivated(f.kit, prKitMenuItem(f.kit, menu, {.text = names[i], .checked = chosen == i}, {})))
                chosen = i;
        }
    };
    f.frame();
    CHECK_FALSE(prKitIsOpen(f.kit, field));
    f.click(f.center(field));
    f.frame();
    f.frame();
    CHECK(prKitIsOpen(f.kit, field));
    // The list's items are in the overlay, below the field.
    PrRect box = f.bounds(field);
    PrVec2 third{(box.left + box.right) / 2, box.bottom + 2 + kPrKitHeightMd * 2.5f};
    f.click(third);
    f.frame();
    CHECK(chosen == 2);
    CHECK_FALSE(prKitIsOpen(f.kit, field));
    // The choice gave the field focus, and with its menu closed, Up steps the choice back.
    f.key(PrAppKey_Up);
    f.frame();
    CHECK(chosen == 1);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a drag's ghost follows the pointer in the overlay, and is gone when the drag ends") {
    KitFixture f;
    PrLayoutNode source = kPrLayoutNodeNone;
    PrLayoutNode ghost = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrKitButton content{.text = "player_base.mesh"};
        source = prKitButton(f.kit, root, content, {});
        prUiDragStart(f.ui, source, 4, 7, 0, 0);
        PrKitDragGhost look{.text = "player_base.mesh", .icon = PrKitIconKind_Cube};
        ghost = prKitDragGhost(f.kit, look);
    };
    f.frame();
    CHECK(ghost == kPrLayoutNodeNone);
    PrVec2 at = f.center(source);
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .pointer = {.position = at}});
    f.frame();
    f.move(PrVec2{at.x + 100, at.y + 100});
    f.frame();
    f.frame();
    REQUIRE(ghost != kPrLayoutNodeNone);
    PrRect box = f.bounds(ghost);
    CHECK(box.left > at.x + 100);
    CHECK(box.left < at.x + 130);
    f.push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .pointer = {.position = PrVec2{at.x + 100, at.y + 100}}});
    f.frame();
    CHECK(ghost == kPrLayoutNodeNone);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("an item's click activates it and closes its menu") {
    KitFixture f;
    MenuScene scene(f);
    scene.openMenu();
    f.click(f.center(scene.items[1]));
    f.frame();
    CHECK(scene.runs[1] == 1);
    CHECK_FALSE(scene.open);
    CHECK(scene.popover == kPrLayoutNodeNone);
    f.frame();
    CHECK(scene.runs[1] == 1);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a disabled item takes no click and leaves its menu open") {
    KitFixture f;
    MenuScene scene(f);
    scene.openMenu();
    f.click(f.center(scene.items[2]));
    f.frame();
    CHECK(scene.runs[2] == 0);
    CHECK(scene.open);
}

TEST_CASE("Escape closes a popover, and focus returns to its trigger") {
    KitFixture f;
    MenuScene scene(f);
    scene.openMenu();
    CHECK(prUiFocus(f.ui) == scene.items[0]);
    f.key(PrAppKey_Escape);
    f.frame();
    CHECK_FALSE(scene.open);
    CHECK(scene.popover == kPrLayoutNodeNone);
    CHECK(prUiFocus(f.ui) == scene.trigger);
}

TEST_CASE("a press outside a popover closes it, and one inside does not") {
    KitFixture f;
    MenuScene scene(f);
    scene.openMenu();
    PrRect popover = f.bounds(scene.popover);
    f.click(PrVec2{popover.left + 4, popover.top + 4});
    f.frame();
    CHECK(scene.open);
    f.click(PrVec2{600, 500});
    f.frame();
    CHECK_FALSE(scene.open);
    CHECK(scene.popover == kPrLayoutNodeNone);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a press on another button closes a popover and reaches the button") {
    KitFixture f;
    MenuScene scene(f);
    uint32_t others = 0;
    f.build = [&](PrLayoutNode root) {
        scene.build(root);
        others += prKitActivated(f.kit, scene.other) ? 1 : 0;
    };
    scene.openMenu();
    f.click(f.center(scene.other));
    f.frame();
    CHECK_FALSE(scene.open);
    CHECK(others == 1);
}

TEST_CASE("a second click on a trigger closes its popover") {
    KitFixture f;
    MenuScene scene(f);
    scene.openMenu();
    f.click(f.center(scene.trigger));
    f.frame();
    CHECK_FALSE(scene.open);
}

TEST_CASE("Down moves focus to the next item, and Enter activates it") {
    KitFixture f;
    MenuScene scene(f);
    scene.openMenu();
    f.key(PrAppKey_Down);
    f.frame();
    CHECK(prUiFocus(f.ui) == scene.items[1]);
    // The disabled item takes no focus, so Down wraps to the first.
    f.key(PrAppKey_Down);
    f.frame();
    CHECK(prUiFocus(f.ui) == scene.items[0]);
    f.key(PrAppKey_Up);
    f.frame();
    CHECK(prUiFocus(f.ui) == scene.items[1]);
    f.key(PrAppKey_Enter);
    f.frame();
    CHECK(scene.runs[1] == 1);
    CHECK_FALSE(scene.open);
}

TEST_CASE("a popover that does not fit below its trigger opens above it, inside the overlay") {
    KitFixture f;
    PrLayoutNode trigger = kPrLayoutNodeNone;
    PrLayoutNode popover = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        prKitSpacer(f.kit, root, {});
        PrLayoutNode row = prKitBox(f.kit, root, {}, {.direction = PrKitDirection_Row, .gap = 0});
        prKitSpacer(f.kit, row, {});
        PrKitButton content{.text = "Corner"};
        trigger = prKitButton(f.kit, row, content, {});
        if (prKitActivated(f.kit, trigger))
            prKitToggleOpen(f.kit, trigger);
        PrKitPopoverStyle style{.minWidth = 240};
        popover = prKitPopover(f.kit, trigger, {}, style);
        if (popover != kPrLayoutNodeNone) {
            PrLayoutNode menu = prKitMenu(f.kit, popover, {}, {});
            for (uint32_t i = 0; i < 4; ++i) {
                PrKitMenuItem item{.text = "Item"};
                prKitMenuItem(f.kit, menu, item, {});
            }
        }
    };
    f.frame();
    f.click(f.center(trigger));
    f.frame();
    f.frame();
    REQUIRE(popover != kPrLayoutNodeNone);
    PrRect box = f.bounds(popover);
    CHECK(box.bottom == f.bounds(trigger).top);
    CHECK(box.right == 800.0f);
    CHECK(inside(PrRect{0, 0, 800, 600}, box));
}

TEST_CASE("triggers keep their own open state") {
    KitFixture f;
    PrLayoutNode a = kPrLayoutNodeNone;
    PrLayoutNode b = kPrLayoutNodeNone;
    bool openA = false;
    bool openB = false;
    f.build = [&](PrLayoutNode root) {
        PrLayoutNode row = prKitBox(f.kit, root, {}, {.direction = PrKitDirection_Row, .gap = 0});
        PrKitButton content{.text = "A"};
        a = prKitButton(f.kit, row, content, {});
        b = prKitButton(f.kit, row, content, {});
        if (prKitActivated(f.kit, a))
            prKitSetOpen(f.kit, a, true);
        openA = prKitIsOpen(f.kit, a);
        openB = prKitIsOpen(f.kit, b);
    };
    f.frame();
    f.click(f.center(a));
    f.frame();
    CHECK(openA);
    CHECK_FALSE(openB);
}

TEST_CASE("the pointer moving onto a menu bar's item passes the open menu to it in one round") {
    KitFixture f;
    BarScene scene(f);
    f.frame();
    f.click(f.center(scene.items[0]));
    f.frame();
    REQUIRE(scene.open[0]);
    f.move(f.center(scene.items[2]));
    f.frame();
    CHECK_FALSE(scene.open[0]);
    CHECK(scene.open[2]);
    CHECK(scene.popovers[0] == kPrLayoutNodeNone);
    CHECK(scene.popovers[2] != kPrLayoutNodeNone);
    f.move(f.center(scene.items[1]));
    f.frame();
    CHECK(scene.open[1]);
    CHECK_FALSE(scene.open[2]);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("the pointer crossing a menu bar with no menu open opens none") {
    KitFixture f;
    BarScene scene(f);
    f.frame();
    f.move(f.center(scene.items[1]));
    f.frame();
    CHECK_FALSE(scene.open[0]);
    CHECK_FALSE(scene.open[1]);
    CHECK_FALSE(scene.open[2]);
}

TEST_CASE("a secondary click opens a context menu at the pointer, and an item's activation closes it") {
    KitFixture f;
    PrLayoutNode host = kPrLayoutNodeNone;
    PrLayoutNode menu = kPrLayoutNodeNone;
    PrLayoutNode item = kPrLayoutNodeNone;
    uint32_t deletes = 0;
    f.build = [&](PrLayoutNode root) {
        PrKitBoxStyle box{.place = {.flex = PrKitFlex_Fill}};
        host = prKitBox(f.kit, root, {}, box);
        menu = prKitContextMenu(f.kit, host, {}, {});
        item = kPrLayoutNodeNone;
        if (menu != kPrLayoutNodeNone) {
            PrKitMenuItem content{.text = "Delete"};
            PrKitMenuItemStyle danger{.variant = PrKitMenuItemVariant_Danger};
            item = prKitMenuItem(f.kit, menu, content, danger);
            deletes += prKitActivated(f.kit, item) ? 1 : 0;
        }
    };
    f.frame();
    f.frame();
    secondaryClick(f, PrVec2{320, 240});
    f.frame();
    f.frame();
    REQUIRE(menu != kPrLayoutNodeNone);
    PrRect box = f.bounds(menu);
    // The menu sits inside its panel's one px border.
    CHECK(box.left == 321.0f);
    CHECK(box.top == 241.0f);
    f.click(f.center(item));
    f.frame();
    CHECK(deletes == 1);
    CHECK(menu == kPrLayoutNodeNone);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("Escape and a press outside close a context menu") {
    KitFixture f;
    PrLayoutNode host = kPrLayoutNodeNone;
    PrLayoutNode menu = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrKitBoxStyle box{.place = {.flex = PrKitFlex_Fill}};
        host = prKitBox(f.kit, root, {}, box);
        menu = prKitContextMenu(f.kit, host, {}, {});
        if (menu != kPrLayoutNodeNone) {
            PrKitMenuItem content{.text = "Rename"};
            prKitMenuItem(f.kit, menu, content, {});
        }
    };
    f.frame();
    f.frame();
    secondaryClick(f, PrVec2{100, 100});
    f.frame();
    f.frame();
    REQUIRE(menu != kPrLayoutNodeNone);
    f.key(PrAppKey_Escape);
    f.frame();
    CHECK(menu == kPrLayoutNodeNone);

    secondaryClick(f, PrVec2{100, 100});
    f.frame();
    f.frame();
    REQUIRE(menu != kPrLayoutNodeNone);
    f.click(PrVec2{700, 500});
    f.frame();
    CHECK(menu == kPrLayoutNodeNone);
}

TEST_CASE("a tooltip shows once the pointer has rested on its anchor for the delay") {
    KitFixture f;
    PrLayoutNode anchor = kPrLayoutNodeNone;
    PrLayoutNode tip = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrLayoutNode row = prKitBox(f.kit, root, {}, {.direction = PrKitDirection_Row, .gap = 0});
        anchor = prKitButton(f.kit, row, {.icon = PrKitIconKind_Grid},
            {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Md});
        PrKitTooltip content{.text = "Show Grid", .shortcut = "G"};
        tip = prKitTooltip(f.kit, anchor, content);
    };
    f.frame();
    f.move(f.center(anchor));
    f.frame(0.1);
    CHECK(tip == kPrLayoutNodeNone);
    CHECK(prUiOutput(f.ui).wantsFrame);
    f.frame(0.2);
    CHECK(tip == kPrLayoutNodeNone);
    f.frame(0.4);
    REQUIRE(tip != kPrLayoutNodeNone);
    f.frame();
    CHECK(f.bounds(tip).top > f.bounds(anchor).bottom);
    f.move(PrVec2{700, 500});
    f.frame();
    CHECK(tip == kPrLayoutNodeNone);
}

TEST_CASE("a modal holds focus in its content: Tab cycles its buttons") {
    KitFixture f;
    ModalScene scene(f);
    scene.openModal();
    CHECK(prUiFocus(f.ui) == scene.buttons[0]);
    for (uint32_t i = 0; i < 5; ++i) {
        f.key(PrAppKey_Tab);
        f.frame();
        PrLayoutNode focus = prUiFocus(f.ui);
        CHECK((focus == scene.buttons[0] || focus == scene.buttons[1]));
    }
    CHECK(prUiFocus(f.ui) == scene.buttons[1]);
    f.key(PrAppKey_Escape);
    f.frame();
    CHECK_FALSE(scene.open);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a modal's scrim takes presses from what is under it, and a press on it closes the modal") {
    KitFixture f;
    ModalScene scene(f);
    scene.openModal();
    f.click(f.center(scene.buttons[0]));
    f.frame();
    CHECK(scene.open);
    // The trigger is under the scrim, so the press closes the modal and does not reopen it.
    f.click(f.center(scene.trigger));
    f.frame();
    CHECK_FALSE(scene.open);
    CHECK(scene.content == kPrLayoutNodeNone);
}

TEST_CASE("a dialog's close button closes it, and its footer sits in its body") {
    KitFixture f;
    PrLayoutNode trigger = kPrLayoutNodeNone;
    PrLayoutNode body = kPrLayoutNodeNone;
    PrLayoutNode footer = kPrLayoutNodeNone;
    bool open = false;
    f.build = [&](PrLayoutNode root) {
        PrLayoutNode row = prKitBox(f.kit, root, {}, {.direction = PrKitDirection_Row, .gap = 0});
        PrKitButton label{.text = "Rename"};
        trigger = prKitButton(f.kit, row, label, {});
        if (prKitActivated(f.kit, trigger))
            prKitSetOpen(f.kit, trigger, true);
        PrKitDialog dialog{.title = "Rename Entity"};
        PrKitDialogStyle style{.width = 360};
        body = prKitDialog(f.kit, trigger, dialog, style);
        footer = kPrLayoutNodeNone;
        if (body != kPrLayoutNodeNone) {
            prKitLabel(f.kit, body, {.text = "Pick a name"}, {});
            footer = prKitDialogFooter(f.kit, body);
            PrKitButton ok{.text = "OK"};
            prKitButton(f.kit, footer, ok, {});
        }
        open = prKitIsOpen(f.kit, trigger);
    };
    f.frame();
    f.click(f.center(trigger));
    f.frame();
    f.frame();
    REQUIRE(body != kPrLayoutNodeNone);
    PrRect card = f.bounds(body);
    CHECK(card.right - card.left == doctest::Approx(358.0));
    CHECK(inside(card, f.bounds(footer)));
    // The close button is the dialog's first focusable node, and the modal moved focus onto it.
    PrLayoutNode close = prUiFocus(f.ui);
    REQUIRE(close != kPrLayoutNodeNone);
    f.click(f.center(close));
    f.frame();
    CHECK_FALSE(open);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a dialog opens with focus on the node that its autofocus names, and focus then stays where the user puts it") {
    KitFixture f;
    PrLayoutNode trigger = kPrLayoutNodeNone;
    PrLayoutNode cancel = kPrLayoutNodeNone;
    PrLayoutNode ok = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        trigger = prKitButton(f.kit, root, {.text = "Rename"}, {});
        if (prKitActivated(f.kit, trigger))
            prKitSetOpen(f.kit, trigger, true);
        PrLayoutNode body = prKitDialog(f.kit, trigger, {.title = "Rename Entity"}, {});
        if (body == kPrLayoutNodeNone)
            return;
        PrLayoutNode footer = prKitDialogFooter(f.kit, body);
        cancel = prKitButton(f.kit, footer, {.text = "Cancel"}, {});
        ok = prKitButton(f.kit, footer, {.text = "OK"}, {.variant = PrKitButtonVariant_Primary});
        prKitModalAutofocus(f.kit, ok);
    };
    f.frame();
    f.click(f.center(trigger));
    f.frame();
    f.frame();
    CHECK(prUiFocus(f.ui) == ok);
    f.key(PrAppKey_Tab, PrAppModifierFlag_Shift);
    f.frame();
    f.frame();
    CHECK(prUiFocus(f.ui) == cancel);
}

TEST_CASE("a command palette's selection moves by Up and Down, and Enter runs the selected command") {
    KitFixture f;
    PrLayoutNode trigger = kPrLayoutNodeNone;
    PrLayoutNode list = kPrLayoutNodeNone;
    uint32_t ran = 0;
    uint32_t runs = 0;
    bool open = false;
    f.build = [&](PrLayoutNode root) {
        trigger = root;
        list = prKitCommandPalette(f.kit, trigger, {.prompt = "Type a command", .count = 3});
        if (list != kPrLayoutNodeNone) {
            const char* names[] = {"New Scene", "Save", "Build and Run"};
            for (uint32_t i = 0; i < 3; ++i) {
                PrKitCommandItem item{.text = names[i], .category = "File", .icon = PrKitIconKind_Script};
                if (prKitActivated(f.kit, prKitCommandItem(f.kit, list, item))) {
                    ran = i;
                    ++runs;
                }
            }
            prKitCommandPaletteFooter(f.kit, list);
        }
        open = prKitIsOpen(f.kit, trigger);
    };
    f.frame();
    prKitSetOpen(f.kit, trigger, true);
    f.frame();
    f.frame();
    REQUIRE(list != kPrLayoutNodeNone);
    // The palette's card holds focus, and the selection stops at the last item.
    CHECK(prUiFocus(f.ui) == prLayoutNodeParent(f.tree, list));
    f.key(PrAppKey_Down);
    f.frame();
    f.key(PrAppKey_Down);
    f.frame();
    f.key(PrAppKey_Down);
    f.frame();
    f.key(PrAppKey_Up);
    f.frame();
    f.key(PrAppKey_Enter);
    f.frame();
    CHECK(runs == 1);
    CHECK(ran == 1);
    CHECK_FALSE(open);
    // The activation closes the palette after the round that created it, and requests the frame that removes it.
    CHECK(prUiOutput(f.ui).wantsFrame);
    f.frame();
    CHECK(list == kPrLayoutNodeNone);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a command palette's item runs on a click and closes the palette") {
    KitFixture f;
    PrLayoutNode trigger = kPrLayoutNodeNone;
    PrLayoutNode second = kPrLayoutNodeNone;
    uint32_t runs = 0;
    bool open = false;
    f.build = [&](PrLayoutNode root) {
        PrLayoutNode row = prKitBox(f.kit, root, {}, {.direction = PrKitDirection_Row, .gap = 0});
        PrKitButton label{.text = "Search"};
        trigger = prKitButton(f.kit, row, label, {});
        if (prKitActivated(f.kit, trigger))
            prKitSetOpen(f.kit, trigger, true);
        PrLayoutNode list = prKitCommandPalette(f.kit, trigger, {.count = 2});
        second = kPrLayoutNodeNone;
        if (list != kPrLayoutNodeNone) {
            for (uint32_t i = 0; i < 2; ++i) {
                PrKitCommandItem item{.text = "Command", .shortcut = "Cmd S"};
                PrLayoutNode node = prKitCommandItem(f.kit, list, item);
                runs += prKitActivated(f.kit, node) ? 1 : 0;
                if (i == 1)
                    second = node;
            }
        }
        open = prKitIsOpen(f.kit, trigger);
    };
    f.frame();
    f.click(f.center(trigger));
    f.frame();
    f.frame();
    REQUIRE(second != kPrLayoutNodeNone);
    f.click(f.center(second));
    f.frame();
    CHECK(runs == 1);
    CHECK_FALSE(open);
}

TEST_CASE("a frame with an open menu that changes nothing lays nothing out and makes no node") {
    KitFixture f;
    MenuScene scene(f);
    scene.openMenu();
    // The trigger's hover fades out over frames of its own.
    f.move(f.center(scene.items[0]));
    f.frame(0.5);
    f.frame(0.5);
    f.frame();
    PrLayoutTreeStats stats = prLayoutTreeStats(f.tree);
    CHECK(stats.frameNodesMade == 0);
    CHECK(stats.blocksLaidOut == 0);
    CHECK_FALSE(prUiOutput(f.ui).wantsFrame);
    CHECK(kitVerified(f.tree));
}

TEST_CASE("a frame with an open palette and a shown tooltip that changes nothing lays nothing out") {
    KitFixture f;
    PrLayoutNode anchor = kPrLayoutNodeNone;
    f.build = [&](PrLayoutNode root) {
        PrLayoutNode row = prKitBox(f.kit, root, {}, {.direction = PrKitDirection_Row, .gap = 0});
        anchor = prKitButton(f.kit, row, {.icon = PrKitIconKind_Grid},
            {.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Md});
        PrKitTooltip tip{.text = "Grid"};
        prKitTooltip(f.kit, anchor, tip);
        PrLayoutNode list = prKitCommandPalette(f.kit, root, {.prompt = "Search", .count = 1});
        if (list != kPrLayoutNodeNone) {
            prKitCommandItem(f.kit, list, {.text = "Save"});
            prKitCommandPaletteFooter(f.kit, list);
        }
    };
    f.frame();
    f.move(f.center(anchor));
    f.frame();
    f.frame(1.0);
    f.frame();
    prKitSetOpen(f.kit, f.root, true);
    f.frame();
    f.frame();
    f.frame();
    PrLayoutTreeStats stats = prLayoutTreeStats(f.tree);
    CHECK(stats.frameNodesMade == 0);
    CHECK(stats.blocksLaidOut == 0);
    CHECK(kitVerified(f.tree));
}
