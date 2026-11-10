// The application's pages: its lifecycle, its presence, hiding and quitting, its timers, and the requests to open files
// and URLs.

#include "examples/app_features/pages_application.h"

#include "pr/kit_choice.h"
#include "pr/print.h"

#include "examples/app_features/features.h"
#include "examples/app_features/pages_menus.h"

namespace features {

using namespace pr;

namespace {

// How long a hide that undoes itself lasts, in seconds.
constexpr double kHideSeconds = 3;

// Logs the files and URLs of an open request's source, which the reads keep until they answer.
void openedFiles(void* context, PrStatus status, const PrAppDataFile* PR_NULLABLE files, uint32_t count) {
    Features& f = featuresOf(context);
    for (uint32_t i = 0; status == PrStatus_Ok && i < count; ++i)
        f.openItems.push(format("file {} {}", files[i].name, files[i].path));
}

void openedUrls(void* context, PrStatus status, const PrStringView* PR_NULLABLE urls, uint32_t count) {
    Features& f = featuresOf(context);
    for (uint32_t i = 0; status == PrStatus_Ok && i < count; ++i)
        f.openItems.push(format("url {}", urls[i]));
}

} // namespace

void hideFor(Features& f, Hiding hiding) {
    if (f.hiding != Hiding_None)
        return;
    f.hiding = hiding;
    if (hiding == Hiding_App)
        prAppSetHidden(appOf(f), true);
    else if (PrAppWindow* control = controlOf(f))
        prAppWindowSetVisible(control, false);
    prAppTimerSet(appOf(f), kTimerUnhide, kHideSeconds, 0);
}

void unhide(Features& f) {
    if (f.hiding == Hiding_None)
        return;
    if (f.hiding == Hiding_App)
        prAppSetHidden(appOf(f), false);
    // Showing a window leaves the keyboard where it is, so the control window takes it as well.
    if (PrAppWindow* control = controlOf(f))
        prAppWindowFocus(control);
    f.hiding = Hiding_None;
    logLine(f, "shown again");
}

void askToQuit(Features& f, bool sessionEnding) {
    f.quitPending = true;
    f.quitSessionEnding = sessionEnding;
    f.page = Page_Lifecycle;
    if (PrAppWindow* control = controlOf(f))
        prAppWindowFocus(control);
}

void pageLifecycle(Features& f, PrStudioWindow* window, PrLayoutNode parent) {
    PrKit* kit = prStudioWindowKit(window);
    PrApp* app = appOf(f);
    if (f.quitPending) {
        PrLayoutNode ask = section(kit, parent, "QUIT REQUESTED");
        note(kit, ask, f.quitSessionEnding ? "The session was ending, and the application kept it from ending." :
            "The user asked the application to quit, and the application kept running to ask.");
        PrLayoutNode answers = row(kit, ask);
        if (button(kit, answers, "Quit")) {
            f.quitPending = false;
            prAppStop(app);
        }
        if (button(kit, answers, "Keep running"))
            f.quitPending = false;
    }

    PrLayoutNode s = section(kit, parent, "LIFECYCLE");
    line(kit, s, format("lifecycle: {}", prAppLifecycleName(prAppLifecycle(app))));
    note(kit, s, "Switch to another application for Inactive, and minimize or hide this one for Background. Each change "
        "logs LifecycleChanged. LowMemory, LocaleChanged and KeyboardLayoutChanged are logged as they come.");

    PrLayoutNode presence = section(kit, parent, "PRESENCE");
    const PrStringView presences[] = {"Regular", "Accessory"};
    uint32_t chosen = f.accessory ? 1 : 0;
    if (segmented(kit, presence, presences, 2, &chosen)) {
        f.accessory = chosen == 1;
        prAppSetPresence(app, f.accessory ? PrAppPresence_Accessory : PrAppPresence_Regular);
        // The status item brings the application back once it has no other presence.
        if (f.accessory && f.statusItem == nullptr)
            showStatusItem(f);
    }
    note(kit, presence, "An Accessory application leaves the Dock, the taskbar and the application switcher, as a menu "
        "bar utility does. Its windows and status items stay. Accessory adds the status item, whose click brings the "
        "control window back.");

    PrLayoutNode hides = section(kit, parent, "HIDING");
    PrLayoutNode r = row(kit, hides);
    bool hidden = f.hiding != Hiding_None;
    if (button(kit, r, "Hide window for 3 s", hidden))
        hideFor(f, Hiding_Window);
    if (button(kit, r, "Hide app for 3 s", hidden))
        hideFor(f, Hiding_App);
    line(kit, hides, format("application hidden: {}", prAppIsHidden(app) ? "yes" : "no"));
    note(kit, hides, "The control window takes the keyboard as it comes back. On macOS, a hidden application comes back "
        "behind the active one, which keeps the keyboard, since an application cannot take activation from another.");

    PrLayoutNode quit = section(kit, parent, "QUITTING");
    prKitToggle(kit, quit, {.value = &f.confirmQuit, .label = "Ask before quitting"}, {});
    note(kit, quit, "Quit from the menu bar, the Dock or the taskbar, or log out. The request arrives as QuitRequested, "
        "and the application quits as the event returns. To ask first, it cancels the quit and this page asks whether to quit.");
}

void pageTimers(Features& f, PrStudioWindow* window, PrLayoutNode parent) {
    PrKit* kit = prStudioWindowKit(window);
    PrApp* app = appOf(f);
    PrLayoutNode s = section(kit, parent, "TIMERS");
    PrLayoutNode r = row(kit, s);
    if (button(kit, r, "Fire once in 1 s"))
        prAppTimerSet(app, kTimerOnce, 1.0, 0);
    if (button(kit, r, f.repeating ? "Stop repeating" : "Repeat every 0.5 s")) {
        f.repeating = !f.repeating;
        if (f.repeating)
            prAppTimerSet(app, kTimerRepeat, 0.5, 0.5);
        else
            prAppTimerCancel(app, kTimerRepeat);
    }
    if (button(kit, r, "Reset counts")) {
        f.onceFired = 0;
        f.repeatFired = 0;
    }
    line(kit, s, format("once: {}   repeating: {}", f.onceFired, f.repeatFired));
    note(kit, s, "Setting a tag again replaces its timer. Timers keep firing during a live resize and while a menu is "
        "open. A browser fires the timers of a hidden tab at most once a second.");
}

void pageOpen(Features& f, PrStudioWindow* window, PrLayoutNode parent) {
    PrKit* kit = prStudioWindowKit(window);
    PrLayoutNode launch = section(kit, parent, "LAUNCH");
    if (f.launchArguments.isEmpty())
        line(kit, launch, "no arguments");
    for (const String& argument : f.launchArguments)
        line(kit, launch, format("argument {}", argument));
    note(kit, launch, "The arguments that started the program. The web takes them from the page's query, as in "
        "?--renderer=cpu or ?arg=--renderer&arg=cpu.");

    PrLayoutNode s = section(kit, parent, "OPEN REQUESTS");
    line(kit, s, format("requests: {}", f.opens));
    for (const String& argument : f.openArguments)
        line(kit, s, format("argument {}", argument));
    for (const String& item : f.openItems)
        line(kit, s, item);
    note(kit, s, "app_features runs as a single instance. Launch it again, with arguments, and the second launch hands "
        "them to this one as an Open event and exits. On macOS, app_features.app also receives the files dropped on its "
        "Dock icon, and the URLs of its scheme: run open prax-features://hello in a terminal. An installed web app "
        "receives the files that the system opens with it, through File Handling, and its later launches, through the "
        "Launch Handler.");
}

bool applicationEvent(Features& f, const PrAppEvent& e) {
    switch (e.kind) {
        case PrAppEventKind_Timer:
            if (e.tag == kTimerUnhide) {
                unhide(f);
                return true;
            }
            if (e.tag == kTimerOnce)
                ++f.onceFired;
            else if (e.tag == kTimerRepeat)
                ++f.repeatFired;
            else
                return false;
            if (PrAppWindow* control = controlOf(f))
                prAppWindowRequestFrame(control);
            return true;
        case PrAppEventKind_QuitRequested:
            // Without a cancel, the run stops as the event returns.
            if (f.confirmQuit) {
                prAppQuitCancel(appOf(f));
                askToQuit(f, e.quit.sessionEnding);
            }
            return true;
        case PrAppEventKind_Open: {
            ++f.opens;
            f.openArguments.clear();
            f.openItems.clear();
            for (uint32_t i = 0; i < e.open.argumentCount; ++i)
                f.openArguments.push(String(e.open.arguments[i]));
            if (e.open.source != kPrAppDataSourceNone) {
                prAppDataReadFiles(appOf(f), e.open.source, openedFiles, &f);
                prAppDataReadUrls(appOf(f), e.open.source, openedUrls, &f);
            }
            f.page = Page_Open;
            return true;
        }
        default:
            return false;
    }
}

} // namespace features
