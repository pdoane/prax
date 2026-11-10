// The windows' pages: a window's state, place and look, the Dock or the taskbar, popups and dialogs, and backdrops and
// the overlay.

#include "examples/app_features/pages_window.h"

#include "pr/color.h"
#include "pr/count_of.h"
#include "pr/kit_box.h"
#include "pr/kit_choice.h"
#include "pr/kit_menu.h"
#include "pr/kit_surface.h"
#include "pr/kit_text.h"
#include "pr/layout_tree.h"
#include "pr/print.h"
#include "pr/ui.h"

#if defined(__EMSCRIPTEN__)
#include "pr/app_web.h"
#endif

#include "examples/app_features/features.h"
#include "examples/app_features/pages_data.h"

namespace features {

using namespace pr;

namespace {

// The global hotkey's chord, Control-Option-Space.
constexpr PrAppHotkey kOverlayChord{
    .code = PrAppKeyCode_Space,
    .modifiers = PrAppModifierFlag_Control | PrAppModifierFlag_Alt,
};

// Creates the button that sets a generated icon on `window`, or takes it away.
void iconButton(Features& f, PrKit* kit, PrLayoutNode parent, PrAppWindow* window) {
    if (!button(kit, row(kit, parent), f.iconSet ? "Use the application's icon" : "Use a generated icon"))
        return;
    f.iconSet = !f.iconSet;
    Array<uint8_t> pixels;
    prAppWindowSetIcon(window, f.iconSet ? iconPixels(pixels, 64, PrColor{0.2f, 0.6f, 1, 1}) : PrPixelView{});
}

// Logs a window's events, but its moves unless the log asks for them.
bool logWindowEvent(Features& f, PrStringView who, const PrAppEvent& e) {
    bool noisy = e.kind == PrAppEventKind_PointerMove || e.kind == PrAppEventKind_DragOver;
    if (!noisy || f.logMoves)
        logEvent(f, who, e);
    return false;
}

void buildSecond(void*, PrStudioWindow* window, PrLayoutNode root, PrVec2) {
    PrKit* kit = prStudioWindowKit(window);
    PrLayoutNode body = prKitBox(kit, root, {}, PrKitBoxStyle{
        .gap = kPrKitSpace4,
        .padding = prKitEdgesAll(kPrKitSpace4),
    });
    note(kit, body, "A second window of the application, which the Window menu lists.");
}

// ---------------------------------------------------------------------------------------------------------------------
// Backdrops and the overlay

void buildBackdrop(void* context, PrStudioWindow* window, PrLayoutNode root, PrVec2) {
    Features& f = featuresOf(context);
    PrKit* kit = prStudioWindowKit(window);
    // Over the desktop as it is, the window's background is half transparent, so that the content stays readable. A
    // blur is the background itself.
    PrColor background = kit->theme.palette.bg0;
    PrColor canvas{};
    if (f.chosenBackdrop == PrAppWindowBackdrop_Transparent)
        canvas = PrColor{background.r, background.g, background.b, 0.5f};
    prLayoutTreeSetCanvasColor(kit->tree, prColor16FromColor(canvas));
    PrLayoutNode body = prKitBox(kit, root, {}, PrKitBoxStyle{
        .gap = kPrKitSpace4,
        .padding = prKitEdgesAll(kPrKitSpace5),
        .place = {.flex = PrKitFlex_Fill},
    });
    prKitLabel(kit, body, {.text = "Backdrop"}, {.role = PrKitTextRole_Title});
    note(kit, body, "The window's surface has no background, so what lies behind it shows through.");
    const PrStringView names[] = {"Opaque", "Transparent", "Blur"};
    uint32_t chosen = f.chosenBackdrop;
    if (segmented(kit, body, names, 3, &chosen)) {
        f.chosenBackdrop = PrAppWindowBackdrop(chosen);
        prAppWindowSetBackdrop(prStudioWindowAppWindow(window), f.chosenBackdrop);
    }
    PrLayoutNode chip = prKitSurface(kit, body, {}, PrKitSurfaceStyle{
        .box = {.padding = {kPrKitSpace3, kPrKitSpace4, kPrKitSpace3, kPrKitSpace4}},
        .background = PrKitBackground_Hud,
    });
    prKitLabel(kit, chip, {.text = "A HUD chip over the backdrop"}, {});
}

void openBackdrop(Features& f) {
    PrStudioWindowDesc desc{
        .title = "Backdrop",
        .extent = {480, 320},
        .decoration = PrAppWindowDecoration_Custom,
        .backdrop = f.chosenBackdrop,
        .build = buildBackdrop,
        .titleBar = titleBar,
        .close = [](void* context, PrStudioWindow*) { featuresOf(context).backdrop = nullptr; },
        .context = &f,
    };
    prStudioWindowCreate(f.app, &desc, &f.backdrop);
}

void buildOverlay(void* context, PrStudioWindow* window, PrLayoutNode root, PrVec2) {
    Features& f = featuresOf(context);
    PrKit* kit = prStudioWindowKit(window);
    PrLayoutNode body = prKitBox(kit, root, {}, PrKitBoxStyle{
        .gap = kPrKitSpace3,
        .padding = prKitEdgesAll(kPrKitSpace5),
    });
    prKitLabel(kit, body, {.text = "Overlay"}, {.role = PrKitTextRole_Heading});
    PrAppWindow* app = prStudioWindowAppWindow(window);
    note(kit, body, prAppWindowIsActive(app) ? "Has the keyboard: type, or press Escape to hide." :
        "Control-Option-Space toggles this overlay and focuses it.");
    line(kit, body, format("typed: {}", f.typed));
}

void openOverlay(Features& f) {
    PrStudioWindowDesc desc{
        .title = "Overlay",
        .extent = {380, 150},
        .role = PrAppWindowRole_Overlay,
        .backdrop = PrAppWindowBackdrop_Blur,
        .hidden = true,
        .pointerPassthrough = f.passthrough,
        .placement = PrAppWindowPlacement_Center,
        .event = [](void* context, PrStudioWindow* window, const PrAppEvent* e) {
            Features& f = featuresOf(context);
            logWindowEvent(f, "overlay", *e);
            if (e->kind == PrAppEventKind_KeyDown && e->keyboard.key == PrAppKey_Escape) {
                prAppWindowSetVisible(prStudioWindowAppWindow(window), false);
                return true;
            }
            if (e->kind == PrAppEventKind_Text)
                f.typed.append(e->input.text);
            return false;
        },
        .build = buildOverlay,
        .close = [](void* context, PrStudioWindow*) { featuresOf(context).overlay = nullptr; },
        .context = &f,
    };
    prStudioWindowCreate(f.app, &desc, &f.overlay);
}

// ---------------------------------------------------------------------------------------------------------------------
// Popups and dialogs

// A popup menu of five items. Choosing an item closes it, and so does a press elsewhere, which asks it to close: the
// window takes no CloseRequested, so Studio closes it.
void buildPopup(void* context, PrStudioWindow* window, PrLayoutNode root, PrVec2) {
    Features& f = featuresOf(context);
    PrKit* kit = prStudioWindowKit(window);
    PrLayoutNode body = prKitSurface(kit, root, {}, PrKitSurfaceStyle{
        .box = {.gap = kPrKitSpace1, .padding = prKitEdgesAll(kPrKitSpace2), .place = {.flex = PrKitFlex_Fill}},
        .background = PrKitBackground_Raised,
        .rules = PrKitRuleFlag_All,
    });
    for (uint32_t i = 1; i <= 5; ++i) {
        if (button(kit, body, format("Item {}", i))) {
            logLine(f, format("popup item {} chosen", i));
            prStudioWindowDestroy(window);
        }
    }
}

void openPopup(Features& f) {
    if (f.popup != nullptr)
        prStudioWindowDestroy(f.popup);
    f.popupPlacement.anchor = f.popupAnchor;
    PrStudioWindowDesc desc{
        .title = "Popup",
        .extent = {160, 190},
        .role = PrAppWindowRole_Popup,
        .parent = f.control,
        .popup = f.popupPlacement,
        .event = [](void* context, PrStudioWindow*, const PrAppEvent* e) {
            return logWindowEvent(featuresOf(context), "popup", *e);
        },
        .build = buildPopup,
        .close = [](void* context, PrStudioWindow*) { featuresOf(context).popup = nullptr; },
        .context = &f,
    };
    prStudioWindowCreate(f.app, &desc, &f.popup);
}

// A dialog that closes with its button, or with its close button, whose CloseRequested the window does not take.
void buildDialog(void*, PrStudioWindow* window, PrLayoutNode root, PrVec2) {
    PrKit* kit = prStudioWindowKit(window);
    PrLayoutNode body = prKitBox(kit, root, {}, PrKitBoxStyle{
        .gap = kPrKitSpace4,
        .padding = prKitEdgesAll(kPrKitSpace5),
    });
    prKitLabel(kit, body, {.text = prAppWindowTitle(prStudioWindowAppWindow(window))}, {.role = PrKitTextRole_Heading});
    note(kit, body, "A dialog stays above the control window and follows it. A modal dialog takes the control window's "
        "input while it shows, as a sheet on macOS.");
    if (button(kit, body, "Close"))
        prStudioWindowDestroy(window);
}

bool logDialogEvent(void* context, PrStudioWindow*, const PrAppEvent* e) {
    return logWindowEvent(featuresOf(context), "dialog", *e);
}

void openDialog(Features& f) {
    PrStudioWindowDesc desc{
        .title = f.modal ? "Modal dialog" : "Dialog",
        .extent = {420, 200},
        .role = PrAppWindowRole_Dialog,
        .parent = f.control,
        .modal = f.modal,
        .event = logDialogEvent,
        .build = buildDialog,
        .close = [](void* context, PrStudioWindow*) { featuresOf(context).dialog = nullptr; },
        .context = &f,
    };
    prStudioWindowCreate(f.app, &desc, &f.dialog);
}

// A window without decoration, which has no title bar and closes from its content. Its field shows that it takes the
// keyboard.
void buildBorderless(void* context, PrStudioWindow* window, PrLayoutNode root, PrVec2) {
    Features& f = featuresOf(context);
    PrKit* kit = prStudioWindowKit(window);
    PrLayoutNode body = prKitBox(kit, root, {}, PrKitBoxStyle{
        .gap = kPrKitSpace4,
        .padding = prKitEdgesAll(kPrKitSpace5),
    });
    prKitLabel(kit, body, {.text = "No decoration"}, {.role = PrKitTextRole_Heading});
    note(kit, body, "The window has no title bar and no buttons of the system's. It takes the keyboard all the same.");
    textField(kit, body, 1, f.borderlessText, "Type here");
    if (button(kit, body, "Close"))
        prStudioWindowDestroy(window);
}

void openBorderless(Features& f) {
    PrStudioWindowDesc desc{
        .title = "Borderless",
        .extent = {360, 220},
        .decoration = PrAppWindowDecoration_None,
        .placement = PrAppWindowPlacement_Center,
        .event = logDialogEvent,
        .build = buildBorderless,
        .close = [](void* context, PrStudioWindow*) { featuresOf(context).borderless = nullptr; },
        .context = &f,
    };
    prStudioWindowCreate(f.app, &desc, &f.borderless);
}

} // namespace

void openSecond(Features& f) {
    PrStudioWindowDesc desc{
        .title = "Second Window",
        .extent = {520, 240},
        .decoration = PrAppWindowDecoration_Custom,
        .event = [](void* context, PrStudioWindow*, const PrAppEvent* e) {
            return logWindowEvent(featuresOf(context), "second", *e);
        },
        .build = buildSecond,
        .titleBar = titleBar,
        .close = [](void* context, PrStudioWindow*) { featuresOf(context).second = nullptr; },
        .context = &f,
    };
    prStudioWindowCreate(f.app, &desc, &f.second);
}

void toggleOverlay(Features& f) {
    if (f.overlay == nullptr)
        openOverlay(f);
    if (f.overlay == nullptr)
        return;
    PrAppWindow* overlay = prStudioWindowAppWindow(f.overlay);
    if (prAppWindowIsVisible(overlay)) {
        prAppWindowSetVisible(overlay, false);
    } else {
        f.typed.clear();
        prAppWindowFocus(overlay);
    }
}

void pageWindows(Features& f, PrStudioWindow* window, PrLayoutNode parent) {
    PrKit* kit = prStudioWindowKit(window);
    PrAppWindow* app = prStudioWindowAppWindow(window);

    PrLayoutNode s = section(kit, parent, "THIS WINDOW");
    PrAppDisplayPosition position{};
    if (prAppWindowPosition(app, &position))
        line(kit, s, format("{} on display {:x} at ({:.0f}, {:.0f})", prAppWindowStateName(prAppWindowState(app)),
            position.display, position.position.x, position.position.y));
    line(kit, s, format("active: {}   visible: {}   occluded: {}", prAppWindowIsActive(app) ? "yes" : "no",
        prAppWindowIsVisible(app) ? "yes" : "no", prAppWindowIsOccluded(app) ? "yes" : "no"));
    bool multiple = prAppWindowModel(appOf(f)) == PrAppWindowModel_Multiple;
    line(kit, s, format("window model: {}", multiple ? "Multiple" : "Single"));
    PrLayoutNode states = row(kit, s);
    for (PrAppWindowState state = PrAppWindowState_Normal; state <= PrAppWindowState_Fullscreen; ++state) {
        if (button(kit, states, prAppWindowStateName(state)))
            prAppWindowSetState(app, state);
    }
#if defined(__EMSCRIPTEN__)
    // The page itself covers the machine's display, beyond what Fullscreen does for a window of the page.
    bool page = prAppWebIsHostFullscreen(appOf(f));
    if (button(kit, states, page ? "Return the page" : "Page on the whole display"))
        prAppWebSetHostFullscreen(appOf(f), !page);
#endif
    PrLayoutNode titles = row(kit, s);
    line(kit, titles, "title:");
    if (textField(kit, titles, 1, f.title, "Title"))
        prAppWindowSetTitle(app, f.title);
    note(kit, s, "Edit the title, and the title bar, the Window menu and the application switcher follow it. The "
        "application asks for Multiple windows, or Single with --single-window, and --extent 1100x700 sets the control "
        "window's extent. On the web, a Single window fills the page, and its Fullscreen covers the whole display. "
        "Multiple windows float in the page, which is their display: Fullscreen fills the page, and the page's own "
        "button puts it on the whole display. The page's title follows the window that has the keyboard.");

    PrLayoutNode size = section(kit, parent, "SIZE AND PLACE");
    PrLayoutNode sizes = row(kit, size);
    if (button(kit, sizes, "960 x 640"))
        prAppWindowSetExtent(app, {960, 640});
    if (button(kit, sizes, "1200 x 860"))
        prAppWindowSetExtent(app, {1200, 860});
    if (button(kit, sizes, "Move to (40, 40)") && prAppWindowPosition(app, &position)) {
        position.position = PrVec2{40, 40};
        prAppWindowSetPosition(app, &position);
    }
    if (prKitActivated(kit, prKitToggle(kit, sizes, {.value = &f.aspect, .label = "Keep 3:2 while resizing"}, {})))
        prAppWindowSetAspectRatio(app, f.aspect ? 1.5f : 0);

    PrLayoutNode look = section(kit, parent, "LOOK AND ORDER");
    PrLayoutNode fade = row(kit, look);
    line(kit, fade, "opacity:");
    const PrStringView opacities[] = {"100%", "85%", "60%"};
    const float kOpacities[] = {1, 0.85f, 0.6f};
    uint32_t chosen = f.opacity == 1 ? 0 : f.opacity > 0.7f ? 1 : 2;
    if (segmented(kit, fade, opacities, 3, &chosen)) {
        f.opacity = kOpacities[chosen];
        prAppWindowSetOpacity(app, f.opacity);
    }
    PrLayoutNode order = row(kit, look);
    PrKitCheck above{.value = &f.keepAbove, .label = "Keep above other applications"};
    if (prKitActivated(kit, prKitToggle(kit, order, above, {})))
        prAppWindowSetKeepAbove(app, f.keepAbove);
    if (button(kit, order, "Request attention in 3 s"))
        prAppTimerSet(appOf(f), kTimerAttention, 3, 0);
    note(kit, look, "Opacity fades the whole window, its content and its frame alike. A backdrop, on the Backdrops page, "
        "instead clears only the background behind the content. Switch to another application before the attention "
        "request fires: the Dock icon bounces, or the taskbar button flashes.");
}

void pageTaskbar(Features& f, PrStudioWindow* window, PrLayoutNode parent) {
    PrKit* kit = prStudioWindowKit(window);
    PrAppWindow* app = prStudioWindowAppWindow(window);

    PrLayoutNode badge = section(kit, parent, "BADGE");
    PrLayoutNode r = row(kit, badge);
    if (textField(kit, r, 1, f.badge, "Badge, such as 3"))
        prAppSetBadge(appOf(f), f.badge);
    note(kit, badge, "The badge shows on the Dock icon, as the overlay of the taskbar button, or on the icon of an "
        "installed web app, through the Badging API. An empty badge clears it.");

    PrLayoutNode icon = section(kit, parent, "ICON");
    iconButton(f, kit, icon, app);
    note(kit, icon, "Windows shows the window's icon in its title bar and taskbar button, and the web as the favicon. "
        "macOS shows only the application's icon, so the button changes nothing there.");

    PrLayoutNode progress = section(kit, parent, "PROGRESS");
    PrLayoutNode stateRow = row(kit, progress);
    line(kit, stateRow, "state:");
    const PrStringView states[] = {"None", "Normal", "Indeterminate", "Paused", "Error"};
    uint32_t state = f.progressState;
    bool changed = segmented(kit, stateRow, states, 5, &state);
    PrLayoutNode amountRow = row(kit, progress);
    line(kit, amountRow, "value:");
    const PrStringView amounts[] = {"0%", "25%", "50%", "75%", "100%"};
    uint32_t amount = uint32_t(f.progress * 4 + 0.5f);
    changed = segmented(kit, amountRow, amounts, 5, &amount) || changed;
    if (changed) {
        f.progressState = PrAppProgressState(state);
        f.progress = float(amount) / 4;
        prAppWindowSetProgress(app, f.progressState, f.progress);
    }
    note(kit, progress, "The bar shows over the Dock icon, or in the taskbar button. Paused is yellow and Error red. An "
        "indeterminate bar is striped in the Dock, which draws the icon only when it changes, and moves in the "
        "taskbar.");

    PrLayoutNode document = section(kit, parent, "DOCUMENT");
    PrLayoutNode chosen = row(kit, document);
    if (button(kit, chosen, "Choose a file"))
        showDialog(f, PrAppFileDialogKind_Open, 0);
    line(kit, chosen, f.lastPath.isEmpty() ? PrStringView("no file yet") : PrStringView(f.lastPath));
    PrLayoutNode docs = row(kit, document);
    if (button(kit, docs, "Show this document", f.lastPath.isEmpty()))
        prAppWindowSetDocument(app, f.lastPath, f.documentEdited);
    if (button(kit, docs, "No document"))
        prAppWindowSetDocument(app, "", false);
    PrLayoutNode edited = prKitToggle(kit, docs, {.value = &f.documentEdited, .label = "Unsaved changes"}, {});
    if (prKitActivated(kit, edited) && !f.lastPath.isEmpty())
        prAppWindowSetDocument(app, f.lastPath, f.documentEdited);
    PrLayoutNode recent = row(kit, document);
    if (button(kit, recent, "Add to recent documents", f.lastPath.isEmpty()))
        prAppRecentDocumentAdd(appOf(f), f.lastPath);
    if (button(kit, recent, "Clear recent documents"))
        prAppRecentDocumentsClear(appOf(f));
    note(kit, document, "macOS shows the document's icon in a system title bar, and the unsaved dot in the close "
        "button. This window draws its own title bar, so it shows only the dot. Recent documents are listed in the Dock "
        "icon's menu and in File > Open Recent.");
}

void pagePopups(Features& f, PrStudioWindow* window, PrLayoutNode parent) {
    PrKit* kit = prStudioWindowKit(window);
    PrLayoutNode s = section(kit, parent, "POPUPS");
    const PrStringView sides[] = {"Below", "Above", "After", "Before"};
    uint32_t side = f.popupPlacement.side;
    if (segmented(kit, s, sides, 4, &side))
        f.popupPlacement.side = PrAppPopupSide(side);
    const PrStringView aligns[] = {"Start", "Center", "End"};
    uint32_t align = f.popupPlacement.align;
    if (segmented(kit, s, aligns, 3, &align))
        f.popupPlacement.align = PrAppPopupAlign(align);
    PrLayoutNode adjust = row(kit, s);
    bool flip = (f.popupPlacement.adjust & PrAppPopupAdjustFlag_Flip) != 0;
    bool slide = (f.popupPlacement.adjust & PrAppPopupAdjustFlag_Slide) != 0;
    bool resize = (f.popupPlacement.adjust & PrAppPopupAdjustFlag_Resize) != 0;
    prKitToggle(kit, adjust, {.value = &flip, .label = "Flip"}, {});
    prKitToggle(kit, adjust, {.value = &slide, .label = "Slide"}, {});
    prKitToggle(kit, adjust, {.value = &resize, .label = "Resize"}, {});
    f.popupPlacement.adjust = PrAppPopupAdjustFlags((flip ? PrAppPopupAdjustFlag_Flip : 0) |
        (slide ? PrAppPopupAdjustFlag_Slide : 0) | (resize ? PrAppPopupAdjustFlag_Resize : 0));
    PrLayoutNode opener = prKitButton(kit, s, {.text = "Open popup"}, {});
    f.popupAnchor = boundsOf(kit, opener);
    if (prKitActivated(kit, opener))
        openPopup(f);
    note(kit, s, "The popup opens against the button. Move the window to a display's edge to see it flip and slide. "
        "On the web, it stays inside the page, and flips and slides at its edges. A press outside it, or switching to "
        "another application, asks it to close. A Single application has no popup or dialog windows.");

    PrLayoutNode d = section(kit, parent, "DIALOGS AND DECORATION");
    PrLayoutNode r = row(kit, d);
    prKitToggle(kit, r, {.value = &f.modal, .label = "Modal"}, {});
    if (button(kit, r, f.dialog == nullptr ? "Open dialog" : "Close dialog")) {
        if (f.dialog == nullptr)
            openDialog(f);
        else
            prStudioWindowDestroy(f.dialog);
    }
    if (button(kit, r, f.borderless == nullptr ? "Open window without decoration" : "Close window without decoration")) {
        if (f.borderless == nullptr)
            openBorderless(f);
        else
            prStudioWindowDestroy(f.borderless);
    }
}

void pageBackdrops(Features& f, PrStudioWindow* window, PrLayoutNode parent) {
    PrKit* kit = prStudioWindowKit(window);
    PrLayoutNode b = section(kit, parent, "BACKDROP");
    if (button(kit, b, f.backdrop == nullptr ? "Open backdrop window" : "Close backdrop window")) {
        if (f.backdrop == nullptr)
            openBackdrop(f);
        else
            prStudioWindowDestroy(f.backdrop);
    }
    note(kit, b, "The backdrop window shows the desktop through a transparent window, or blurred behind it.");

    PrLayoutNode s = section(kit, parent, "OVERLAY AND GLOBAL HOTKEY");
    PrLayoutNode r = row(kit, s);
    bool shown = f.overlay != nullptr && prAppWindowIsVisible(prStudioWindowAppWindow(f.overlay));
    if (button(kit, r, shown ? "Hide overlay" : "Show and focus overlay"))
        toggleOverlay(f);
    if (f.overlay != nullptr && !shown && button(kit, r, "Show overlay without focus"))
        prAppWindowSetVisible(prStudioWindowAppWindow(f.overlay), true);
    PrLayoutNode passthrough = prKitToggle(kit, r, {.value = &f.passthrough, .label = "Pointer passes through"}, {});
    if (prKitActivated(kit, passthrough) && f.overlay != nullptr)
        prAppWindowSetPointerPassthrough(prStudioWindowAppWindow(f.overlay), f.passthrough);
    PrLayoutNode keys = row(kit, s);
    PrApp* app = appOf(f);
    if (button(kit, keys, f.hotkeyRegistered ? "Unregister Control-Option-Space" : "Register Control-Option-Space")) {
        if (f.hotkeyRegistered) {
            prAppHotkeyUnregister(app, kOverlayHotkey);
            f.hotkeyRegistered = false;
        } else {
            f.hotkeyStatus = prAppHotkeyRegister(app, kOverlayChord, kOverlayHotkey);
            f.hotkeyRegistered = f.hotkeyStatus == PrStatus_Ok;
            f.hotkeyTried = true;
        }
    }
    if (f.hotkeyRegistered)
        line(kit, keys, "registered");
    else if (f.hotkeyTried && f.hotkeyStatus != PrStatus_Ok)
        line(kit, keys, format("registration failed: {}", statusName(f.hotkeyStatus)));
    else
        line(kit, keys, "not registered");
    note(kit, s, "Switch to another application and press the chord: the overlay shows over it, and takes the "
        "keyboard without activating this application.");
}

bool windowsAppEvent(Features& f, const PrAppEvent& e) {
    if (e.kind == PrAppEventKind_Timer && e.tag == kTimerAttention) {
        if (PrAppWindow* control = controlOf(f))
            prAppWindowRequestAttention(control);
        return true;
    }
    if (e.kind == PrAppEventKind_Hotkey && e.tag == kOverlayHotkey) {
        toggleOverlay(f);
        return true;
    }
    return false;
}

} // namespace features
