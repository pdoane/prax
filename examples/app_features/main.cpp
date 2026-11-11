// The application features: every feature of lib/app (pr/app.h) in a window of Studio (pr/studio_window.h), to try by
// hand.
//
// The control window has a sidebar of pages, grouped by area, the page that the sidebar selects, and a log of the
// events that reach the application and its windows. main() is the composition root, the model is Features, and the
// pages are in the pages_*.cpp files.

#include "pr/count_of.h"
#include "pr/font_bundle.h"
#include "pr/font_set.h"
#include "pr/integer.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_choice.h"
#include "pr/kit_surface.h"
#include "pr/kit_text.h"
#include "pr/print.h"
#include "pr/ui.h"

#include "examples/app_features/features.h"
#include "examples/app_features/pages_application.h"
#include "examples/app_features/pages_data.h"
#include "examples/app_features/pages_device.h"
#include "examples/app_features/pages_input.h"
#include "examples/app_features/pages_menus.h"
#include "examples/app_features/pages_window.h"

namespace {

using namespace features;
using namespace pr;

// The width of the sidebar and of the log, in px.
constexpr float kSidebarWidth = 220;
constexpr float kLogWidth = 440;

// An entry of the sidebar: its group, which heads the first entry of the group, its label, its page, and what builds
// the page.
struct SidebarEntry {
    const char* group;
    const char* label;
    Page page;
    void (*build)(Features& f, PrStudioWindow* window, PrLayoutNode parent);
};

// The pages, without those of features that the web lacks.
constexpr SidebarEntry kSidebar[] = {
#if !defined(__EMSCRIPTEN__)
    {"APPLICATION", "Lifecycle and quitting", Page_Lifecycle, pageLifecycle},
#endif
    {"APPLICATION", "Timers", Page_Timers, pageTimers},
    {"APPLICATION", "Open and single instance", Page_Open, pageOpen},
    {"WINDOWS", "Windows", Page_Windows, pageWindows},
    {"WINDOWS", "Dock and taskbar", Page_Taskbar, pageTaskbar},
    {"WINDOWS", "Popups and dialogs", Page_Popups, pagePopups},
    {"WINDOWS", "Backdrops and overlays", Page_Backdrops, pageBackdrops},
    {"WINDOWS", "Tabs between windows", Page_Tabs, pageTabs},
    {"INPUT", "Pointer and cursors", Page_Pointer, pagePointer},
    {"INPUT", "Keys and text", Page_Keys, pageKeys},
    {"INPUT", "Gestures and trackpad", Page_Gestures, pageGestures},
    {"DATA", "Clipboard", Page_Clipboard, pageClipboard},
    {"DATA", "Drag and drop", Page_Drags, pageDrags},
    {"DATA", "Files", Page_Files, pageFiles},
#if !defined(__EMSCRIPTEN__)
    {"MENUS", "Menus", Page_Menus, pageMenus},
    {"MENUS", "Status items", Page_StatusItems, pageStatusItems},
#endif
    {"SHELL", "Notifications and sharing", Page_Shell, pageShell},
    {"DEVICE", "Power and sleep", Page_Power, pagePower},
    {"DEVICE", "Sensors and haptics", Page_Sensors, pageSensors},
    {"DEVICE", "Displays and preferences", Page_Displays, pageDisplays},
};

void buildSidebar(Features& f, PrKit* kit, PrLayoutNode parent) {
    PrLayoutNode side = prKitSurface(kit, parent, {}, PrKitSurfaceStyle{
        .box = {
            .overflow = PrKitOverflow_ScrollY,
            .padding = {kPrKitSpace3, 0, kPrKitSpace4, 0},
            .place = {.width = prKitPx(kSidebarWidth), .flex = PrKitFlex_Fixed},
        },
        .background = PrKitBackground_App,
        .rules = PrKitRuleFlag_Right,
    });
    PrStringView group{};
    for (const SidebarEntry& entry : kSidebar) {
        if (group != entry.group) {
            group = entry.group;
            PrLayoutNode heading = prKitBox(kit, side, {}, PrKitBoxStyle{
                .padding = {kPrKitSpace5, kPrKitSpace4, kPrKitSpace2, kPrKitSpace4},
                .place = {.flex = PrKitFlex_Fixed},
            });
            prKitLabel(kit, heading, {.text = entry.group}, {.role = PrKitTextRole_Caption, .tone = PrKitTone_Faint});
        }
        bool active = f.page == entry.page;
        PrLayoutNode item = prKitSelectable(kit, side, {.selected = active}, PrKitBoxStyle{
            .direction = PrKitDirection_Row,
            .align = PrKitAlign_Center,
            .padding = prKitEdgesXY(0, kPrKitSpace4),
            .place = {.height = prKitPx(kPrKitHeightSm), .flex = PrKitFlex_Fixed},
        });
        prKitLabel(kit, item, {.text = entry.label}, {
            .tone = PrKitTone(active ? PrKitTone_Primary : PrKitTone_Secondary),
            .wrap = PrKitWrap_Clip,
        });
        if (prKitActivated(kit, item))
            f.page = entry.page;
    }
}

void buildLog(Features& f, PrKit* kit, PrLayoutNode parent) {
    PrLayoutNode panel = prKitSurface(kit, parent, {}, PrKitSurfaceStyle{
        .box = {
            .gap = kPrKitSpace3,
            .padding = prKitEdgesAll(kPrKitSpace4),
            .place = {.width = prKitPx(kLogWidth), .flex = PrKitFlex_Fixed},
        },
        .background = PrKitBackground_Panel,
        .rules = PrKitRuleFlag_Left,
    });
    PrLayoutNode header = row(kit, panel);
    prKitLabel(kit, header, {.text = "EVENTS"}, {.role = PrKitTextRole_Caption, .tone = PrKitTone_Muted});
    if (button(kit, header, "Clear"))
        f.log.clear();
    prKitToggle(kit, header, {.value = &f.logMoves, .label = "Moves"}, {});
    PrLayoutNode lines = prKitBox(kit, panel, {}, PrKitBoxStyle{
        .overflow = PrKitOverflow_ScrollY,
        .place = {.flex = PrKitFlex_Fill},
    });
    // The latest line is at the top, so that it shows without scrolling.
    for (uint32_t i = f.log.count(); i-- > 0;)
        prKitLabel(kit, lines, {.text = f.log[i]}, {
            .role = PrKitTextRole_Mono,
            .tone = PrKitTone_Secondary,
            .wrap = PrKitWrap_Wrap,
        });
}

void buildControl(void* context, PrStudioWindow* window, PrLayoutNode root, PrVec2) {
    Features& f = featuresOf(context);
    PrKit* kit = prStudioWindowKit(window);
    PrLayoutNode columns = prKitBox(kit, root, {}, PrKitBoxStyle{
        .direction = PrKitDirection_Row,
        .place = {.flex = PrKitFlex_Fill},
    });
    buildSidebar(f, kit, columns);
    PrLayoutNode page = prKitBox(kit, columns, {}, PrKitBoxStyle{
        .overflow = PrKitOverflow_ScrollY,
        .gap = kPrKitSpace4,
        .padding = prKitEdgesAll(kPrKitSpace4),
        .place = {.flex = PrKitFlex_Fill},
    });
    for (const SidebarEntry& entry : kSidebar) {
        if (entry.page == f.page)
            entry.build(f, window, page);
    }
    buildLog(f, kit, columns);
}

// Logs the control window's events, and gives them to the areas that take them.
bool controlEvent(void* context, PrStudioWindow* window, const PrAppEvent* e) {
    Features& f = featuresOf(context);
    bool noisy = e->kind == PrAppEventKind_PointerMove || e->kind == PrAppEventKind_DragOver ||
        e->kind == PrAppEventKind_Scroll || e->kind == PrAppEventKind_Gesture ||
        e->kind == PrAppEventKind_TrackpadTouches;
    if (!noisy || f.logMoves)
        logEvent(f, "control", *e);
    bool consumed = inputWindowEvent(f, window, *e);
    consumed = menusWindowEvent(f, *e) || consumed;
    consumed = dataWindowEvent(f, *e) || consumed;
    return consumed;
}

// Logs the application's events, and gives them to the areas that take them.
void appEvent(void* context, PrStudioApp*, const PrAppEvent* e) {
    Features& f = featuresOf(context);
    bool noisy = e->kind == PrAppEventKind_Sensor || (e->kind == PrAppEventKind_Timer && e->tag == kTimerRepeat);
    if (e->kind != PrAppEventKind_Wake && (!noisy || f.logMoves))
        logEvent(f, "app", *e);
    if (applicationEvent(f, *e) || windowsAppEvent(f, *e) || menusAppEvent(f, *e) || dataAppEvent(f, *e))
        return;
    deviceAppEvent(f, *e);
}

// The largest width or height that --extent accepts, in px.
constexpr uint64_t kExtentMax = 16384;

// Returns the extent that the arguments give after --extent, as 1024x768, or `fallback` if they give none, or give
// another form or a side of zero or past kExtentMax.
PrUVec2 extentArgument(const Array<String>& arguments, PrUVec2 fallback) {
    for (uint32_t i = 0; i + 1 < arguments.count(); ++i) {
        if (arguments[i] != "--extent")
            continue;
        PrStringView text = arguments[i + 1];
        uint64_t width = 0;
        uint64_t height = 0;
        size_t digits = prU64ParsePrefix(text.data, text.size, &width, 10);
        if (digits == 0 || digits + 1 >= text.size || text.data[digits] != 'x')
            return fallback;
        PrStringView rest{text.data + digits + 1, text.size - digits - 1};
        if (!prU64Parse(rest.data, rest.size, &height, 10))
            return fallback;
        bool valid = width != 0 && height != 0 && width <= kExtentMax && height <= kExtentMax;
        return valid ? PrUVec2{uint32_t(width), uint32_t(height)} : fallback;
    }
    return fallback;
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    static Features features;
    features.page = kSidebar[0].page;
    for (int i = 1; i < argc; ++i)
        features.launchArguments.push(String(argv[i]));
    for (uint32_t i = 0; i < 4; ++i)
        prKitTabsInsert(features.controlTabs.tabs, features.nextTab++, i);
    // The system's fonts, or Standard14 where the build has no system to ask, as on the web.
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    const PrFontBundle bundles[] = {PrFontBundle_Standard14};
    if (system != nullptr) {
        prFontSetSetSystem(fonts, system);
    } else if (prFontBundleInstall(fonts, bundles, uint32_t(countOf(bundles)), PR_FONT_BUNDLE_DIR) == 0) {
        printError("app_features: the fonts under {} could not be installed\n", PR_FONT_BUNDLE_DIR);
        prFontSetDestroy(fonts);
        return 1;
    }
    PrStudioWindowDesc control{
        .title = "App Features",
        .extent = extentArgument(features.launchArguments, PrUVec2{1360, 880}),
        .minExtent = {960, 560},
        .decoration = PrAppWindowDecoration_Custom,
        .open = [](void* context, PrStudioWindow* window) {
            Features& f = featuresOf(context);
            f.app = prStudioWindowStudioApp(window);
            f.control = window;
            return true;
        },
        .beginFrame = [](void* context, PrStudioWindow*, const PrAppFrame* frame) { featuresOf(context).frame = *frame; },
        .event = controlEvent,
        .build = buildControl,
        .titleBar = titleBar,
        .close = [](void* context, PrStudioWindow*) { featuresOf(context).control = nullptr; },
        .context = &features,
    };
    // A second launch hands its arguments to this one, which the Open page shows.
    PrAppOptions options = prAppOptionsDefault();
    options.singleInstance = "org.prax.app-features";
    // The application asks for Desktop windows, or Single for --single-window.
    options.windowModel = PrAppWindowModel_Desktop;
    for (const String& argument : features.launchArguments) {
        if (argument == "--single-window")
            options.windowModel = PrAppWindowModel_Single;
    }
    PrStudioAppDesc app{
        .fonts = fonts,
        .options = &options,
        .window = &control,
        .event = appEvent,
        .shutdown = [](void* context, PrStudioApp* studio) {
            Features& f = featuresOf(context);
            prAppCancel(prStudioAppApp(studio), &f);
            if (f.statusItem != nullptr)
                prAppStatusItemDestroy(f.statusItem);
            f.statusItem = nullptr;
        },
        .context = &features,
    };
    PrStatus status = prStudioAppRun(&app);
    prFontSetDestroy(fonts);
    prFontSystemDestroy(system);
    if (status == PrStatus_NotSupported)
        printError("app_features: no GPU device to render with\n");
    return status == PrStatus_Ok ? 0 : 1;
}
