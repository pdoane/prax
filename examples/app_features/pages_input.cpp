// The input's pages: the pointer and its modes, the cursors, the keys and their labels, the kinds of text input, and
// the gestures and touches of a trackpad.

#include "examples/app_features/pages_input.h"

#include "pr/count_of.h"
#include "pr/integer.h"
#include "pr/kit_box.h"
#include "pr/kit_choice.h"
#include "pr/kit_panel.h"
#include "pr/kit_text.h"
#include "pr/print.h"
#include "pr/ui.h"

#include "examples/app_features/features.h"

namespace features {

using namespace pr;

namespace {

// The keys whose labels the Keys page shows: letters and punctuation that layouts move, a digit, and keys that type
// nothing.
constexpr PrAppKeyCode kLabeledKeys[] = {
    PrAppKeyCode_KeyQ, PrAppKeyCode_KeyW, PrAppKeyCode_KeyA, PrAppKeyCode_KeyZ, PrAppKeyCode_KeyY,
    PrAppKeyCode_KeyM, PrAppKeyCode_Digit2, PrAppKeyCode_Minus, PrAppKeyCode_Semicolon, PrAppKeyCode_Quote,
    PrAppKeyCode_BracketLeft, PrAppKeyCode_Backquote, PrAppKeyCode_Enter, PrAppKeyCode_Space, PrAppKeyCode_Tab,
};

// Returns the names of `modifiers`, joined by +, or none.
String modifierNames(PrAppModifierFlags modifiers) {
    String names;
    for (PrAppModifierFlags rest = modifiers; rest != 0; rest = PrAppModifierFlags(rest & (rest - 1))) {
        if (!names.isEmpty())
            names.append("+");
        names.append(prAppModifierName(PrAppModifierFlags(1u << prU64LowestBit(rest))));
    }
    return names.isEmpty() ? String("none") : names;
}

} // namespace

void pagePointer(Features& f, PrStudioWindow* window, PrLayoutNode parent) {
    PrKit* kit = prStudioWindowKit(window);
    PrAppWindow* app = prStudioWindowAppWindow(window);
    PrLayoutNode s = section(kit, parent, "POINTER");
    line(kit, s, format("over the window: {}   at ({:.0f}, {:.0f})", f.inside ? "yes" : "no", f.pointer.x,
        f.pointer.y));
    line(kit, s, format("movement ({:.1f}, {:.1f})   travelled ({:.0f}, {:.0f})", f.movement.x, f.movement.y,
        f.travelled.x, f.travelled.y));
    PrAppDisplayPosition screen{};
    if (prAppPointerPosition(appOf(f), &screen))
        line(kit, s, format("on display {:x} at ({:.0f}, {:.0f})", screen.display, screen.position.x,
            screen.position.y));
    PrLayoutNode r = row(kit, s);
    PrAppPointerMode current = prAppWindowPointerMode(app);
    PrLayoutNode modes = prKitSegmented(kit, r, {}, {});
    for (PrAppPointerMode mode = PrAppPointerMode_Free; mode <= PrAppPointerMode_LockedRaw; ++mode) {
        PrKitChoice choice{.label = prAppPointerModeName(mode), .selected = current == mode};
        if (prKitActivated(kit, prKitSegment(kit, modes, choice))) {
            f.travelled = PrVec2{};
            prAppWindowSetPointerMode(app, mode);
        }
    }
    if (button(kit, r, "Reset travelled"))
        f.travelled = PrVec2{};
    // The pointer's position on the screen comes with no event, so each frame asks for the next while it is followed.
    prKitToggle(kit, r, {.value = &f.trackScreen, .label = "Follow on screen"}, {});
    if (f.trackScreen)
        prUiRequestFrame(prStudioWindowUi(window));
    prKitToggle(kit, r, {.value = &f.logMoves, .label = "Log moves"}, {});
    note(kit, s, "Confined keeps the pointer in the window. Locked hides the cursor, which stays, and moves report only "
        "movement; Raw without the system's acceleration. Escape frees the pointer. The web locks the pointer through "
        "Pointer Lock, cannot confine it, and knows its position only over the window.");

    PrLayoutNode cursors = section(kit, parent, "CURSORS");
    note(kit, cursors, "Hover each cell for its cursor. The web shows the CSS cursor of each name.");
    PrLayoutNode grid = row(kit, cursors);
    PrUi* ui = prStudioWindowUi(window);
    for (uint32_t i = 0; i < PrAppCursor_Count; ++i) {
        PrLayoutNode cell = prKitPanel(kit, grid, {}, PrKitPanelStyle{
            .box = {.padding = prKitEdgesAll(kPrKitSpace3)},
            .background = PrKitBackground_Raised,
            .rules = PrKitRuleFlag_All,
        });
        prKitLabel(kit, cell, {.text = prAppCursorName(PrAppCursor(i))}, {.role = PrKitTextRole_BodySmall});
        prUiSetCursor(ui, cell, PrAppCursor(i));
    }
}

void pageKeys(Features& f, PrStudioWindow* window, PrLayoutNode parent) {
    PrKit* kit = prStudioWindowKit(window);
    PrLayoutNode s = section(kit, parent, "LAST KEY");
    if (f.lastKey.kind == PrAppEventKind_KeyDown || f.lastKey.kind == PrAppEventKind_KeyUp) {
        const PrAppKeyboardEvent& k = f.lastKey.keyboard;
        PrStringView name = prAppKeyName(k.key);
        String key = name.isEmpty() && k.key > ' ' && k.key < 127 ? format("{:c}", char(k.key)) : String(name);
        line(kit, s, format("{} {}  key {}  code {}  text \"{}\"{}{}", prAppEventKindName(f.lastKey.kind),
            prAppKeyCodeLabel(appOf(f), k.code), key, prAppKeyCodeName(k.code), f.lastKeyText,
            k.repeat ? "  repeat" : "", k.composing ? "  composing" : ""));
        line(kit, s, format("modifiers {}", modifierNames(f.lastKey.modifiers)));
    } else {
        note(kit, s, "Press a key while this window has the keyboard.");
    }

    PrLayoutNode labels = section(kit, parent, "KEY LABELS IN THE CURRENT LAYOUT");
    PrLayoutNode grid = row(kit, labels);
    for (PrAppKeyCode code : kLabeledKeys)
        line(kit, grid, format("{}={}", prAppKeyCodeName(code), prAppKeyCodeLabel(appOf(f), code)));
    note(kit, labels, "Switch the keyboard layout, as to French or German: KeyboardLayoutChanged arrives, and the labels "
        "follow the layout. On the web, only Chrome reports labels, through its Keyboard Map.");

    PrLayoutNode fields = section(kit, parent, "KINDS OF TEXT INPUT");
    for (uint32_t i = 0; i < countOf(f.fields); ++i) {
        PrAppTextInputKind kind = PrAppTextInputKind(i);
        PrLayoutNode r = row(kit, fields);
        line(kit, r, format("{:<9}", prAppTextInputKindName(kind)));
        textField(kit, r, 100 + i, f.fields[i], prAppTextInputKindName(kind), kind);
    }
    note(kit, fields, "A mobile system or browser shows the keyboard of each kind. A password bypasses the input "
        "method, and other processes cannot read its keys while it is edited. Compose with an input method, as "
        "Japanese, to log CompositionUpdate and CompositionEnd.");
}

void pageGestures(Features& f, PrStudioWindow* window, PrLayoutNode parent) {
    PrKit* kit = prStudioWindowKit(window);
    PrLayoutNode s = section(kit, parent, "GESTURES");
    line(kit, s, format("scale {:.2f}   rotation {:.1f} degrees   smart magnifies {}", f.scale, f.rotation,
        f.smartMagnifies));
    line(kit, s, format("last swipe ({:.0f}, {:.0f})", f.swipe.x, f.swipe.y));
    if (button(kit, s, "Reset")) {
        f.scale = 1;
        f.rotation = 0;
        f.swipe = PrVec2{};
        f.smartMagnifies = 0;
    }
    note(kit, s, "Pinch, rotate with two fingers, double-tap with two fingers, and swipe between pages, as the system's "
        "settings allow. A browser reports a pinch as Control and a wheel, Safari reports rotation too, and the "
        "browser keeps the swipe between pages.");

    PrLayoutNode t = section(kit, parent, "TRACKPAD TOUCHES");
    if (f.touches.isEmpty())
        note(kit, t, "Rest fingers on the trackpad while the pointer is over this window.");
    for (const PrAppTrackpadTouch& touch : f.touches)
        line(kit, t, format("touch {} at ({:.2f}, {:.2f})", touch.id, touch.position.x, touch.position.y));
}

bool inputWindowEvent(Features& f, PrStudioWindow* window, const PrAppEvent& e) {
    PrAppWindow* app = prStudioWindowAppWindow(window);
    switch (e.kind) {
        case PrAppEventKind_PointerEnter:
            f.inside = true;
            f.pointer = e.pointer.position;
            break;
        case PrAppEventKind_PointerLeave:
            f.inside = false;
            break;
        case PrAppEventKind_PointerMove:
            f.pointer = e.pointer.position;
            f.movement = e.pointer.movement;
            f.travelled = PrVec2{f.travelled.x + f.movement.x, f.travelled.y + f.movement.y};
            break;
        case PrAppEventKind_KeyDown:
        case PrAppEventKind_KeyUp:
            f.lastKey = e;
            f.lastKeyText = String(e.keyboard.text);
            f.lastKey.keyboard.text = PrStringView{};
            // Escape frees a pointer that the window holds.
            if (e.kind == PrAppEventKind_KeyDown && e.keyboard.key == PrAppKey_Escape &&
                    prAppWindowPointerMode(app) != PrAppPointerMode_Free) {
                prAppWindowSetPointerMode(app, PrAppPointerMode_Free);
                return true;
            }
            break;
        case PrAppEventKind_Gesture:
            switch (e.gesture.kind) {
                case PrAppGestureKind_Magnify:
                    f.scale *= 1 + e.gesture.magnification;
                    break;
                case PrAppGestureKind_Rotate:
                    f.rotation += e.gesture.rotation;
                    break;
                case PrAppGestureKind_SmartMagnify:
                    ++f.smartMagnifies;
                    break;
                default:
                    f.swipe = e.gesture.swipe;
                    break;
            }
            break;
        case PrAppEventKind_TrackpadTouches:
            f.touches.clear();
            for (uint32_t i = 0; i < e.trackpad.count; ++i)
                f.touches.push(e.trackpad.touches[i]);
            break;
        default:
            break;
    }
    return false;
}

} // namespace features
