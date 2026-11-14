// The parts of app_features that its pages share: the log, the names that it shows, and the building blocks of a page.

#include "examples/app_features/features.h"

#include "pr/float.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_choice.h"
#include "pr/kit_input.h"
#include "pr/kit_panel.h"
#include "pr/kit_text.h"
#include "pr/layout_query.h"
#include "pr/layout_unit.h"
#include "pr/print.h"

namespace features {

using namespace pr;

namespace {

// The most lines that the log keeps.
constexpr uint32_t kLogLines = 400;

} // namespace

Features& featuresOf(void* context) {
    return *static_cast<Features*>(context);
}

PrApp* appOf(const Features& f) {
    return prStudioAppApp(f.app);
}

PrAppWindow* PR_NULLABLE controlOf(const Features& f) {
    return f.control != nullptr ? prStudioWindowAppWindow(f.control) : nullptr;
}

void logLine(Features& f, PrStringView line) {
    if (f.log.count() == kLogLines)
        f.log.erase(0);
    f.log.push(String(line));
    if (PrAppWindow* control = controlOf(f))
        prAppWindowRequestFrame(control);
}

PrStringView statusName(PrStatus status) {
    PrStringView name = prStatusName(status);
    return name.isEmpty() ? PrStringView("Ok") : name;
}

void logEvent(Features& f, PrStringView who, const PrAppEvent& e) {
    PrStringView kind = prAppEventKindName(e.kind);
    switch (e.kind) {
        case PrAppEventKind_PointerMove:
        case PrAppEventKind_PointerDown:
        case PrAppEventKind_PointerUp:
        case PrAppEventKind_PointerCancel:
        case PrAppEventKind_PointerEnter:
        case PrAppEventKind_PointerLeave:
            logLine(f, format("{} {} ({:.0f}, {:.0f}) button {} movement ({:.1f}, {:.1f})", who, kind,
                e.pointer.position.x, e.pointer.position.y, e.pointer.button, e.pointer.movement.x,
                e.pointer.movement.y));
            break;
        case PrAppEventKind_KeyDown:
        case PrAppEventKind_KeyUp:
            logLine(f, format("{} {} {} \"{}\"{}", who, kind, prAppKeyCodeName(e.keyboard.code), e.keyboard.text,
                e.keyboard.repeat ? " repeat" : ""));
            break;
        case PrAppEventKind_Text:
        case PrAppEventKind_Paste:
        case PrAppEventKind_CompositionUpdate:
        case PrAppEventKind_CompositionEnd:
            logLine(f, format("{} {} \"{}\"", who, kind, e.input.text));
            break;
        case PrAppEventKind_Gesture:
            logLine(f, format("{} {} {} {} magnification {:.3f} rotation {:.1f} swipe ({:.0f}, {:.0f})", who, kind,
                prAppGestureKindName(e.gesture.kind), prAppGesturePhaseName(e.gesture.phase), e.gesture.magnification,
                e.gesture.rotation, e.gesture.swipe.x, e.gesture.swipe.y));
            break;
        case PrAppEventKind_TrackpadTouches:
            logLine(f, format("{} {} {} touches", who, kind, e.trackpad.count));
            break;
        case PrAppEventKind_DragEnter:
        case PrAppEventKind_DragLeave:
        case PrAppEventKind_Drop:
        case PrAppEventKind_DragEnd:
            logLine(f, format("{} {} forms {:#x} operations {:#x} operation {}{}", who, kind, e.drag.forms,
                e.drag.operations, prAppDragOperationName(e.drag.operation), e.drag.fromThisWindow ? " from this window" : ""));
            break;
        case PrAppEventKind_DragPromise:
            logLine(f, format("{} {} index {}", who, kind, e.promise.index));
            break;
        case PrAppEventKind_Timer:
        case PrAppEventKind_Hotkey:
        case PrAppEventKind_Notification:
        case PrAppEventKind_StatusItem:
        case PrAppEventKind_MenuOpening:
        case PrAppEventKind_MenuItem:
            logLine(f, format("{} {} tag {}", who, kind, e.tag));
            break;
        case PrAppEventKind_QuitRequested:
            logLine(f, format("{} {}{}", who, kind, e.quit.sessionEnding ? " as the session ends" : ""));
            break;
        case PrAppEventKind_Open:
            logLine(f, format("{} {} with {} arguments", who, kind, e.open.argumentCount));
            break;
        case PrAppEventKind_Sensor:
            logLine(f, format("{} {} {} ({:.2f}, {:.2f}, {:.2f}, {:.2f})", who, kind, prAppSensorName(e.sensor.sensor),
                e.sensor.values[0], e.sensor.values[1], e.sensor.values[2], e.sensor.values[3]));
            break;
        default:
            logLine(f, format("{} {}", who, kind));
            break;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Building blocks

void titleBar(void*, PrStudioWindow* window, PrLayoutNode bar) {
    PrKit* kit = prStudioWindowKit(window);
    prKitLabel(kit, bar, {.text = prAppWindowTitle(prStudioWindowAppWindow(window))}, {.role = PrKitTextRole_Strong});
}

PrLayoutNode section(PrKit* kit, PrLayoutNode parent, PrStringView title) {
    PrLayoutNode box = prKitPanel(kit, parent, {}, PrKitPanelStyle{
        .box = {.gap = kPrKitSpace3, .padding = prKitEdgesAll(kPrKitSpace4)},
        .background = PrKitBackground_Panel,
        .rules = PrKitRuleFlag_All,
    });
    prKitLabel(kit, box, {.text = title}, {.role = PrKitTextRole_Caption, .tone = PrKitTone_Muted});
    return box;
}

PrLayoutNode row(PrKit* kit, PrLayoutNode parent) {
    return prKitBox(kit, parent, {}, PrKitBoxStyle{
        .direction = PrKitDirection_Row,
        .align = PrKitAlign_Center,
        .wrap = true,
        .gap = kPrKitSpace3,
    });
}

bool button(PrKit* kit, PrLayoutNode parent, PrStringView text, bool disabled) {
    return prKitActivated(kit, prKitButton(kit, parent, {.text = text, .disabled = disabled}, {}));
}

void line(PrKit* kit, PrLayoutNode parent, PrStringView text) {
    prKitLabel(kit, parent, {.text = text}, {.role = PrKitTextRole_Mono, .wrap = PrKitWrap_Wrap});
}

void note(PrKit* kit, PrLayoutNode parent, PrStringView text) {
    prKitLabel(kit, parent, {.text = text}, {
        .role = PrKitTextRole_BodySmall,
        .tone = PrKitTone_Muted,
        .wrap = PrKitWrap_Wrap,
    });
}

bool segmented(PrKit* kit, PrLayoutNode parent, const PrStringView* labels, uint32_t count, uint32_t* selected) {
    PrLayoutNode choices = prKitSegmented(kit, parent, {}, {});
    bool changed = false;
    for (uint32_t i = 0; i < count; ++i) {
        if (prKitActivated(kit, prKitSegment(kit, choices, {.label = labels[i], .selected = *selected == i})) &&
                *selected != i) {
            *selected = i;
            changed = true;
        }
    }
    return changed;
}

bool textField(PrKit* kit, PrLayoutNode parent, PrUiKey key, String& text, PrStringView placeholder, PrAppTextInputKind kind) {
    PrKitTextField field{.key = key, .text = text, .placeholder = placeholder, .inputKind = kind};
    PrStringView changed{};
    if (!prKitTextChanged(kit, prKitTextField(kit, parent, field, {}), &changed))
        return false;
    text = String(changed);
    return true;
}

PrRect boundsOf(PrKit* kit, PrLayoutNode node) {
    return prLayoutRectToPx(prLayoutNodeBounds(kit->tree, node, PrLayoutCoordinates_Viewport));
}

PrPixelView iconPixels(Array<uint8_t>& pixels, uint32_t size, PrColor color) {
    pixels.resize(size * size * 4);
    float radius = float(size) / 2;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float dx = float(x) + 0.5f - radius;
            float dy = float(y) + 0.5f - radius;
            float distance = prF32Sqrt(dx * dx + dy * dy);
            // A disc whose edge is lighter, with a soft edge one pixel wide.
            float alpha = distance <= radius - 1 ? 1 : distance >= radius ? 0 : radius - distance;
            float ring = distance > radius * 0.7f ? 1.0f : 0.75f;
            uint8_t* pixel = &pixels[(y * size + x) * 4];
            pixel[0] = uint8_t(prF32Min(color.r * ring, 1.0f) * 255);
            pixel[1] = uint8_t(prF32Min(color.g * ring, 1.0f) * 255);
            pixel[2] = uint8_t(prF32Min(color.b * ring, 1.0f) * 255);
            pixel[3] = uint8_t(alpha * 255);
        }
    }
    return PrPixelView{pixels.data(), size_t(size) * 4, PrUVec2{size, size}};
}

} // namespace features
