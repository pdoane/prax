// Tests of the terminal app's interface, driven the way its window drives it: the tabs, their commands, and closing a
// tab when its program exits.

#include "pr/color.h"
#include "pr/file.h"
#include "pr/function.h"
#include "pr/layout_tree.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/thread.h"
#include "pr/time.h"
#include "pr/ui.h"
#include "pr/url_file.h"

#include "apps/terminal/terminal_app.h"
#include "tools/harness/kit_fixture.h"

#include "tools/harness/test.h"

using namespace harness;
using namespace terminal;

namespace {

// How long a test waits for a program, in milliseconds.
constexpr double kPatience = 10000;

// A window of the app in a kit of its own, set up as its Studio window sets it up. The first window makes the app,
// whose tabs run `program` with `arguments`, and a window made from another joins that window's app.
struct Window {
    KitFixture f;
    TerminalApp own;
    TerminalApp& app;
    TerminalWindow t;

    explicit Window(PrStringView program, std::initializer_list<PrStringView> arguments) : app(own) {
        app.fonts = f.fonts;
        app.program = program;
        for (PrStringView argument : arguments)
            app.arguments.push(String(argument));
        openApp(app);
        open();
    }

    explicit Window(Window& first) : app(first.app) { open(); }

    // Closes the window, and the app if the window made it. A window closes before the window whose app it joined.
    ~Window() {
        closeWindow(t);
        if (&app == &own)
            closeApp(own);
    }

    void open() {
        t.kit = f.kit;
        t.ui = f.ui;
        openWindow(app, t);
        REQUIRE(openTab(t) == PrStatus_Ok);
        f.build = [this](PrLayoutNode root) { buildWindow(t, root, PrVec2{800, 600}); };
        settle();
    }

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Runs a frame, which wakes the app first, as the terminals' programs do. A terminal that asked for focus receives
    // it in the frame after.
    void settle() {
        serviceTabs(app);
        f.frame();
        f.frame();
    }

    // The height of the tab strip at the last layout, in px.
    float stripHeight() {
        PrRect strip = f.bounds(t.strip);
        return strip.bottom - strip.top;
    }

    void run(PrStringView id) {
        REQUIRE(prCommandRegistryRun(t.commands, id, nullptr, nullptr) == PrStatus_Ok);
        settle();
    }

    // Runs frames until `done` returns true. Fails the test if that takes longer than kPatience.
    void frameUntil(const Function<bool()>& done) {
        double end = prTimeMonotonicMilliseconds() + kPatience;
        while (!done()) {
            REQUIRE(prTimeMonotonicMilliseconds() < end);
            prThreadSleep(2);
            settle();
        }
    }

    // The terminals of the window's tabs, and the index of the one that shows.
    Array<PrTerminal*> tabs() { return tabsOf(t); }
    uint32_t shown() const { return prKitTabsIndex(t.tabs, prKitTabsSelected(t.tabs)); }

    PrRect shownBounds() { return f.bounds(prTerminalNode(tabs()[shown()])); }

    // Renders the last frame onto a canvas prepared as `canvas` says, and returns the color of the pixel at `at`.
    PrColor8 pixel(PrVec2 at, KitCanvas canvas = KitCanvas_Cleared) {
        std::vector<uint8_t> rgba;
        REQUIRE(f.render(&rgba, canvas));
        size_t i = (size_t(at.y) * KitFixture::kRenderWidth + size_t(at.x)) * 4;
        return PrColor8{rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
    }
};

// Returns true if each color channel of `a` is within 2 of the same channel of `color`, in 8 bits. Alpha is ignored.
bool near(PrColor8 a, PrColor color) {
    auto close = [](uint8_t x, float y) {
        int32_t value = int32_t(y * 255 + 0.5f);
        return (int32_t(x) > value ? int32_t(x) - value : value - int32_t(x)) <= 2;
    };
    return close(a.r, color.r) && close(a.g, color.g) && close(a.b, color.b);
}

} // namespace

TEST_CASE("terminal app: one terminal fills the window, with no tab strip") {
    Window a("sh", {"-c", "sleep 5"});
    REQUIRE(a.tabs().count() == 1);
    CHECK(a.stripHeight() == 0);
    PrRect bounds = a.shownBounds();
    CHECK(bounds.top == 0);
    CHECK(bounds.bottom == doctest::Approx(600.0));
    CHECK(prTerminalHasFocus(a.tabs()[0]));
}

TEST_CASE("terminal app: a new tab shows the strip and takes focus, and the commands move between the tabs") {
    Window a("sh", {"-c", "sleep 5"});
    a.run("tab.new");
    REQUIRE(a.tabs().count() == 2);
    CHECK(a.shown() == 1);
    CHECK(a.stripHeight() > 0);
    CHECK(a.shownBounds().top == doctest::Approx(double(a.stripHeight())));
    CHECK(prTerminalHasFocus(a.tabs()[1]));

    a.run("tab.next");
    CHECK(a.shown() == 0);
    CHECK(prTerminalHasFocus(a.tabs()[0]));
    a.run("tab.previous");
    CHECK(a.shown() == 1);

    a.run("tab.close");
    REQUIRE(a.tabs().count() == 1);
    CHECK(a.shown() == 0);
    CHECK(a.stripHeight() == 0);
    CHECK(prTerminalHasFocus(a.tabs()[0]));
}

TEST_CASE("terminal app: the window clears to the shown terminal's background, which its view paints none of") {
    Window a("sh", {"-c", "sleep 10"});
    a.run("tab.new");
    REQUIRE(a.tabs().count() == 2);
    // The first tab's program sets its own background color.
    PrStringView magenta = "\x1b]11;#ff00ff\x07";
    prVtermWrite(prTerminalVtermLock(a.tabs()[0]), PrByteView{prStringBytes(magenta), magenta.size});
    prTerminalVtermUnlock(a.tabs()[0]);
    for (uint32_t shown : {1u, 0u}) {
        if (a.shown() != shown)
            a.run("tab.next");
        REQUIRE(a.shown() == shown);
        PrTerminal* terminal = a.tabs()[shown];
        PrColor background = prTerminalBackground(terminal);
        // The canvas holds the color in half floats, as the layout does.
        PrColor canvas = a.f.canvas();
        background = prColorFromColor16(prColor16FromColor(background));
        CHECK(canvas.r == background.r);
        CHECK(canvas.g == background.g);
        CHECK(canvas.b == background.b);
        // A blank cell, the gutter, and the leftover space past the last whole cell show only the clear color.
        PrRect view = a.shownBounds();
        PrVec2 cell = prTerminalCellExtent(terminal);
        PrVec2 origin = prTerminalScreenOrigin(terminal);
        PrVec2 points[] = {
            PrVec2{view.left + origin.x + cell.x * 40.5f, view.top + cell.y * 10.5f},
            PrVec2{view.left + origin.x / 2, view.top + cell.y * 10.5f},
            PrVec2{view.right - 1, view.bottom - 1},
        };
        for (PrVec2 at : points) {
            CHECK(a.pixel(at, KitCanvas_Unfilled).a == 0);
            CHECK(near(a.pixel(at), background));
        }
        // The tab strip paints its own surface over the clear color.
        PrVec2 strip{799, a.stripHeight() / 2};
        CHECK(a.pixel(strip, KitCanvas_Unfilled).a == 255);
        CHECK(near(a.pixel(strip), a.f.kit->theme.palette.bg1));
    }
    CHECK(near(a.pixel(PrVec2{400, 300}), PrColor{1, 0, 1, 1}));
}

TEST_CASE("terminal app: a click on a tab's notification shows the tab and gives it focus") {
    Window a("sh", {"-c", "sleep 5"});
    a.run("tab.new");
    a.run("tab.new");
    REQUIRE(a.tabs().count() == 3);
    uint64_t first = prTerminalNotificationTag(a.tabs()[0]);
    CHECK(first != prTerminalNotificationTag(a.tabs()[1]));
    CHECK(prTerminalNotificationTag(a.tabs()[1]) != prTerminalNotificationTag(a.tabs()[2]));
    CHECK(a.shown() == 2);

    PrAppEvent click{.kind = PrAppEventKind_Notification, .tag = first};
    CHECK(handleEvent(a.app, click));
    a.settle();
    CHECK(a.shown() == 0);
    CHECK(prTerminalHasFocus(a.tabs()[0]));

    // A click on the notification of a tab that has since closed leaves the shown tab as it is.
    PrAppEvent stale{.kind = PrAppEventKind_Notification, .tag = 1000};
    CHECK(handleEvent(a.app, stale));
    CHECK(a.shown() == 0);
    PrAppEvent key{.kind = PrAppEventKind_KeyDown, .keyboard = {.key = 'A'}};
    CHECK_FALSE(handleEvent(a.app, key));
}

TEST_CASE("terminal app: a tab closes once its program exits") {
    Window a("sh", {"-c", "sleep 5"});
    a.app.program = "sh";
    a.app.arguments.clear();
    a.app.arguments.push(String("-c"));
    a.app.arguments.push(String("exit 0"));
    a.run("tab.new");
    a.frameUntil([&] { return a.tabs().count() == 1; });
    CHECK(a.shown() == 0);
}

TEST_CASE("terminal app: a tab that does not show reads all that its program writes, and closes once it exits") {
    // The program writes more than the process and the pseudoterminal can buffer, after the second tab shows.
    Window a("sh", {"-c", "sleep 0.3; head -c 8000000 /dev/zero; exit 0"});
    PrTerminal* writer = a.tabs()[0];
    a.app.program = "sh";
    a.app.arguments.clear();
    a.app.arguments.push(String("-c"));
    a.app.arguments.push(String("sleep 10"));
    a.run("tab.new");
    REQUIRE(a.tabs().count() == 2);
    a.frameUntil([&] { return a.tabs().count() == 1; });
    CHECK(a.tabs()[0] != writer);
}

TEST_CASE("terminal app: a new tab starts in the working directory of the terminal that shows") {
    // A directory with no links in its path, and a command that prints the working directory in the same form. On
    // Windows, the sh of Git for Windows prints a Windows path with pwd -W.
#if defined(_WIN32)
    PrRawString temporary = prFileUserDirectory(PrFileUserDirectory_Temporary);
    String directory = String::adopt(prFilePathAbsolute(prRawStringView(&temporary), ""));
    prRawStringReset(&temporary);
    PrStringView print = "pwd -W; sleep 10";
#else
    String directory("/usr/lib");
    PrStringView print = "pwd -P; sleep 10";
#endif
    Window a("sh", {"-c", "sleep 10"});
    PrRawString url = prUrlFromFilePath(directory);
    String report = format("\x1b]7;{}\x07", prRawStringView(&url));
    prRawStringReset(&url);
    prVtermWrite(prTerminalVtermLock(a.tabs()[0]), PrByteView{prStringBytes(report), report.count()});
    prTerminalVtermUnlock(a.tabs()[0]);
    a.app.arguments.clear();
    a.app.arguments.push(String("-c"));
    a.app.arguments.push(String(print));
    a.run("tab.new");
    REQUIRE(a.tabs().count() == 2);
    PrTerminal* terminal = a.tabs()[1];
    a.frameUntil([&] {
        PrRawString line = prRawStringNull();
        PrVterm* vterm = prTerminalVtermLock(terminal);
        prVtermLineAppendText(vterm, prVtermScrollbackCount(vterm), 0, prVtermExtent(vterm).x, &line);
        prTerminalVtermUnlock(terminal);
        bool found = prStringStartsWith(prRawStringView(&line), format("{} ", directory));
        prRawStringReset(&line);
        return found;
    });
}

TEST_CASE("terminal app: zoom sets every tab's font, and Actual Size restores it") {
    Window a("sh", {"-c", "sleep 10"});
    a.run("tab.new");
    REQUIRE(a.tabs().count() == 2);
    PrVec2 cell = prTerminalCellExtent(a.tabs()[1]);
    a.run("view.zoomIn");
    a.run("view.zoomIn");
    PrVec2 larger = prTerminalCellExtent(a.tabs()[1]);
    CHECK(larger.y > cell.y);
    a.run("tab.next");
    CHECK(prTerminalCellExtent(a.tabs()[0]).y == doctest::Approx(double(larger.y)));
    a.run("view.zoomReset");
    CHECK(prTerminalCellExtent(a.tabs()[0]).y == doctest::Approx(double(cell.y)));
    a.run("view.zoomOut");
    CHECK(prTerminalCellExtent(a.tabs()[0]).y < cell.y);
}

TEST_CASE("terminal app: a tab moves within its window and to another window, where it shows and takes focus") {
    Window a("sh", {"-c", "sleep 10"});
    Window b(a);
    a.run("tab.new");
    REQUIRE(a.tabs().count() == 2);
    PrTerminal* first = a.tabs()[0];
    PrTerminal* second = a.tabs()[1];

    moveTab(a.app, first, a.t, 1);
    a.settle();
    CHECK(a.tabs()[0] == second);
    CHECK(a.tabs()[1] == first);
    CHECK(a.shown() == 1);

    PrTerminal* staying = b.tabs()[0];
    moveTab(a.app, first, b.t, 0);
    a.settle();
    b.settle();
    REQUIRE(a.tabs().count() == 1);
    CHECK(a.tabs()[0] == second);
    CHECK(a.stripHeight() == 0);
    REQUIRE(b.tabs().count() == 2);
    CHECK(b.tabs()[0] == first);
    CHECK(b.tabs()[1] == staying);
    CHECK(b.shown() == 0);
    CHECK(prTerminalHasFocus(first));
    CHECK(prUiFocus(b.f.ui) == prTerminalNode(first));
    CHECK(b.stripHeight() > 0);
}

TEST_CASE("terminal app: a window of one tab shows its strip under a dragged tab, and takes the tab dropped there") {
    Window a("sh", {"-c", "sleep 10"});
    Window b(a);
    a.run("tab.new");
    REQUIRE(a.tabs().count() == 2);
    PrTerminal* moving = a.tabs()[1];
    CHECK(b.stripHeight() == 0);

    // The system carries the tab from the first window over the second, as a drag of the application's payload, whose
    // value is the tab's id.
    PrAppDragEvent drag{
        .position = PrVec2{400, 10},
        .forms = PrAppDataFormFlag_Payload,
        .operations = PrAppDragOperationFlag_Move,
        .payload = {.kind = kTabGroup, .value = prTerminalNotificationTag(moving)},
    };
    b.f.push(PrAppEvent{.kind = PrAppEventKind_DragEnter, .drag = drag});
    b.settle();
    REQUIRE(b.stripHeight() > 0);
    drag.position = PrVec2{400, b.stripHeight() / 2};
    b.f.push(PrAppEvent{.kind = PrAppEventKind_DragOver, .drag = drag});
    b.settle();
    b.f.push(PrAppEvent{.kind = PrAppEventKind_Drop, .drag = drag});
    b.settle();
    a.settle();
    REQUIRE(b.tabs().count() == 2);
    CHECK(b.tabs()[b.shown()] == moving);
    REQUIRE(a.tabs().count() == 1);
    CHECK(a.tabs()[0] != moving);
}

TEST_CASE("terminal app: each window has its own tabs, zoom applies to every window, and a notification finds its tab") {
    Window a("sh", {"-c", "sleep 10"});
    Window b(a);
    REQUIRE(a.app.windows.count() == 2);
    b.run("tab.new");
    CHECK(a.tabs().count() == 1);
    REQUIRE(b.tabs().count() == 2);
    CHECK(b.shown() == 1);

    // Zoom in one window applies to the terminals of every window.
    PrVec2 cell = prTerminalCellExtent(a.tabs()[0]);
    b.run("view.zoomIn");
    a.settle();
    CHECK(prTerminalCellExtent(b.tabs()[1]).y > cell.y);
    CHECK(prTerminalCellExtent(a.tabs()[0]).y > cell.y);

    // A click on the notification of the second window's first tab shows that tab in that window.
    PrAppEvent click{.kind = PrAppEventKind_Notification, .tag = prTerminalNotificationTag(b.tabs()[0])};
    CHECK(handleEvent(a.app, click));
    b.settle();
    CHECK(b.shown() == 0);
    CHECK(prTerminalHasFocus(b.tabs()[0]));
    CHECK(a.shown() == 0);
}
