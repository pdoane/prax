// The shell's and the device's pages: notifications, URLs and sharing, power and staying awake, sensors, haptics and
// orientations, and the displays and the user's preferences.

#include "examples/app_features/pages_device.h"

#include "pr/color_space.h"
#include "pr/count_of.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_choice.h"
#include "pr/kit_text.h"
#include "pr/print.h"
#include "pr/ui.h"

#include "examples/app_features/features.h"

namespace features {

using namespace pr;

namespace {

// The tag of the notifications that the Shell page posts.
constexpr uint64_t kNotificationTag = 11;

void shared(void* context, PrStatus status) {
    logLine(featuresOf(context), format("share: {}", statusName(status)));
}

} // namespace

void pageShell(Features& f, PrStudioWindow* window, PrLayoutNode parent) {
    PrKit* kit = prStudioWindowKit(window);
    PrApp* app = appOf(f);
    PrLayoutNode n = section(kit, parent, "NOTIFICATIONS");
    if (button(kit, n, "Post a notification")) {
        PrAppNotificationDesc desc{
            .title = "app_features",
            .body = "Click to bring the application forward.",
            .tag = kNotificationTag,
        };
        logLine(f, format("notification: {}", statusName(prAppNotificationPost(app, &desc))));
    }
    note(kit, n, "The first post asks for permission. A click sends Notification. An application outside a bundle "
        "cannot post on macOS, and Safari on iOS posts only from an installed web app.");

    PrLayoutNode u = section(kit, parent, "URLS");
    PrLayoutNode r = row(kit, u);
    textField(kit, r, 1, f.url, "URL");
    if (button(kit, r, "Open"))
        logLine(f, format("open {}: {}", f.url, statusName(prAppOpenUrl(app, f.url))));

    PrLayoutNode s = section(kit, parent, "SHARE");
    PrLayoutNode shares = row(kit, s);
    textField(kit, shares, 2, f.shareText, "Text to share");
    PrLayoutNode share = prKitButton(kit, shares, {.text = "Share"}, {});
    if (prKitActivated(kit, share)) {
        PrAppDataContent content{.forms = PrAppDataFormFlag_Text, .text = f.shareText};
        prAppWindowShare(prStudioWindowAppWindow(window), &content, boundsOf(kit, share), shared, &f);
    }
    note(kit, s, "The share sheet points at the button, and lists services such as Mail, Messages and AirDrop. On the "
        "web, Web Share shows the system's share sheet where the browser has one, as Safari and phones' browsers do.");
}

void pagePower(Features& f, PrStudioWindow* window, PrLayoutNode parent) {
    PrKit* kit = prStudioWindowKit(window);
    PrApp* app = appOf(f);
    PrLayoutNode p = section(kit, parent, "POWER");
    PrAppPower power = prAppPower(app);
    line(kit, p, format("source {}   level {}   charging {}   low power {}", prAppPowerSourceName(power.source),
        power.level < 0 ? String("none") : format("{:.0f}%", power.level * 100), power.charging ? "yes" : "no",
        power.lowPower ? "yes" : "no"));
    note(kit, p, "Unplug the power, or turn on Low Power Mode: PowerChanged arrives. On the web, only Chrome reports "
        "the battery, and no browser reports low power.");

    PrLayoutNode a = section(kit, parent, "STAYING AWAKE");
    PrLayoutNode r = row(kit, a);
    bool display = (f.awake & PrAppAwakeFlag_Display) != 0;
    bool system = (f.awake & PrAppAwakeFlag_System) != 0;
    bool changed = prKitActivated(kit, prKitToggle(kit, r, {.value = &display, .label = "Keep the display awake"}, {}));
    changed = prKitActivated(kit, prKitToggle(kit, r, {.value = &system, .label = "Keep the system awake"}, {})) || changed;
    if (changed) {
        f.awake = PrAppAwakeFlags((display ? PrAppAwakeFlag_Display : 0) | (system ? PrAppAwakeFlag_System : 0));
        prAppSetAwake(app, f.awake);
    }
    note(kit, a, "The web keeps only the display awake, through a Screen Wake Lock, while the window is visible.");

    PrLayoutNode n = section(kit, parent, "SLEEP AND THE SCREEN LOCK");
    line(kit, n, format("suspends {}   resumes {}   screen locks {}", f.suspends, f.resumes, f.locks));
    note(kit, n, "Sleep the computer or lock the screen, and come back: the counts follow Suspend, Resume and "
        "ScreenLocked. The web cannot tell either.");
}

void pageSensors(Features& f, PrStudioWindow* window, PrLayoutNode parent) {
    PrKit* kit = prStudioWindowKit(window);
    PrApp* app = appOf(f);
    PrLayoutNode s = section(kit, parent, "MOTION SENSORS");
    for (uint32_t i = 0; i < PrAppSensor_Count; ++i) {
        PrLayoutNode r = row(kit, s);
        PrStringView name = prAppSensorName(PrAppSensor(i));
        bool on = f.sensors[i];
        if (prKitActivated(kit, prKitToggle(kit, r, {.value = &on, .label = name}, {}))) {
            PrStatus status = prAppSensorEnable(app, PrAppSensor(i), on, 0.1);
            f.sensors[i] = on && status == PrStatus_Ok;
            logLine(f, format("{} {}: {}", name, on ? "on" : "off", statusName(status)));
        }
        const float* v = f.readings[i];
        line(kit, r, format("({:.2f}, {:.2f}, {:.2f}, {:.2f})", v[0], v[1], v[2], v[3]));
    }
    note(kit, s, "A desktop has no motion sensors, and answers NotSupported. A phone or a tablet's browser asks for "
        "permission.");

    PrLayoutNode h = section(kit, parent, "HAPTICS");
    PrLayoutNode haptics = row(kit, h);
    PrUi* ui = prStudioWindowUi(window);
    for (uint32_t i = 0; i < countOf(f.hapticHovered); ++i) {
        PrLayoutNode cell = prKitButton(kit, haptics, {.text = prAppHapticName(PrAppHaptic(i))}, {});
        // A pointer on a trackpad feels the haptic as it enters, and a touch as it activates.
        bool hovered = prUiIsHovered(ui, cell);
        if ((hovered && !f.hapticHovered[i]) || prKitActivated(kit, cell))
            prAppHapticPlay(app, PrAppHaptic(i));
        f.hapticHovered[i] = hovered;
    }
    note(kit, h, "Move the pointer onto a button with a finger on a Force Touch trackpad to feel it on a Mac, where the "
        "trackpad plays only a few distinct patterns. A phone vibrates when a button is tapped, and on the web only "
        "Android's does.");

    PrLayoutNode o = section(kit, parent, "ORIENTATIONS");
    const PrStringView names[] = {"Any", "Portraits", "Landscapes"};
    if (segmented(kit, o, names, 3, &f.orientations)) {
        const PrAppOrientationFlags kFlags[] = {
            PrAppOrientationFlag_All,
            PrAppOrientationFlag_Portrait | PrAppOrientationFlag_PortraitFlipped,
            PrAppOrientationFlag_Landscape | PrAppOrientationFlag_LandscapeFlipped,
        };
        prAppSetOrientations(app, kFlags[f.orientations]);
    }
    note(kit, o, "A device that turns keeps the windows in the orientations allowed. A browser locks only while the "
        "page is fullscreen. A desktop does not turn, so the choice changes nothing there.");
}

void pageDisplays(Features& f, PrStudioWindow* window, PrLayoutNode parent) {
    PrKit* kit = prStudioWindowKit(window);
    PrLayoutNode s = section(kit, parent, "DISPLAYS");
    PrAppDisplayInfo displays[8];
    uint32_t count = prAppDisplays(appOf(f), displays, uint32_t(countOf(displays)));
    for (uint32_t i = 0; i < count && i < countOf(displays); ++i) {
        const PrAppDisplayInfo& d = displays[i];
        line(kit, s, format("{}{} ({:x})", d.name.isEmpty() ? PrStringView("unnamed") : d.name, d.main ? ", main" : "", d.id));
        line(kit, s, format("  {:.0f} x {:.0f} at {}x, {:.0f} Hz, {}, work ({:.0f}, {:.0f})-({:.0f}, {:.0f})",
            d.extent.x, d.extent.y, d.display.pixelRatio, d.refreshRate, prAppOrientationName(d.orientation),
            d.workArea.left, d.workArea.top, d.workArea.right, d.workArea.bottom));
        line(kit, s, format("  headroom {:.1f} of {:.1f}, gamut {}", d.display.headroom, d.display.potentialHeadroom,
            prColorSpaceName(d.display.gamut)));
    }
    note(kit, s, "Connect a display, or change a display's arrangement or scale: DisplaysChanged arrives. The web sees "
        "only the display that the window is on, and its zoom changes the pixel ratio.");

    PrLayoutNode w = section(kit, parent, "THIS WINDOW'S FRAME");
    const PrAppFrame& frame = f.frame;
    line(kit, w, format("extent {:.0f} x {:.0f} at {}x, headroom {:.1f}", frame.extent.x, frame.extent.y,
        frame.display.pixelRatio, frame.display.headroom));
    line(kit, w,
        format("title bar {:.0f} high, insets {:.0f} and {:.0f}", frame.titleBar.height, frame.titleBar.leading,
            frame.titleBar.trailing));
    line(kit, w, format("safe area ({:.0f}, {:.0f}, {:.0f}, {:.0f})   keyboard ({:.0f}, {:.0f})-({:.0f}, {:.0f})",
        frame.safeArea.top, frame.safeArea.right, frame.safeArea.bottom, frame.safeArea.left, frame.keyboard.left,
        frame.keyboard.top, frame.keyboard.right, frame.keyboard.bottom));

    PrLayoutNode p = section(kit, parent, "THE USER'S PREFERENCES");
    const PrAppUserPreferences& preferences = frame.preferences;
    line(kit, p, format("color scheme: {}", prAppColorSchemeName(preferences.colorScheme)));
    line(kit, p, format("contrast: {}", prAppContrastName(preferences.contrast)));
    line(kit, p, format("reduced motion: {}", preferences.reducedMotion ? "yes" : "no"));
    line(kit, p, format("reduced transparency: {}", preferences.reducedTransparency ? "yes" : "no"));
    line(kit, p, format("text scale: {:.2f}", preferences.textScale));
    PrColor accent = preferences.accentColor;
    if (accent.a == 0)
        line(kit, p, "accent color: none");
    else
        line(kit, p, format("accent color: ({:.2f}, {:.2f}, {:.2f})", accent.r, accent.g, accent.b));
    note(kit, p, "Change the appearance, the contrast, the accent color or the text size in the system's settings: the "
        "next frame shows the change. macOS has no text size for every application, and the web scales text with its "
        "zoom, so the text scale stays 1 on both. On the web, only Firefox and Safari report the accent color.");
}

bool deviceAppEvent(Features& f, const PrAppEvent& e) {
    switch (e.kind) {
        case PrAppEventKind_Sensor:
            if (e.sensor.sensor < PrAppSensor_Count) {
                for (uint32_t i = 0; i < 4; ++i)
                    f.readings[e.sensor.sensor][i] = e.sensor.values[i];
            }
            break;
        case PrAppEventKind_Suspend:
            ++f.suspends;
            break;
        case PrAppEventKind_Resume:
            ++f.resumes;
            break;
        case PrAppEventKind_ScreenLocked:
            ++f.locks;
            break;
        case PrAppEventKind_Notification:
            if (PrAppWindow* control = controlOf(f))
                prAppWindowFocus(control);
            break;
        case PrAppEventKind_PowerChanged:
        case PrAppEventKind_DisplaysChanged:
            break;
        default:
            return false;
    }
    if (PrAppWindow* control = controlOf(f))
        prAppWindowRequestFrame(control);
    return true;
}

} // namespace features
