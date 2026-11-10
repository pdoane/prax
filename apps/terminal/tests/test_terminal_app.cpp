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

// The app in a kit of its own, set up as its window sets it up. Its tabs run `program` with `arguments`.
struct Window {
    KitFixture f;
    TerminalApp t;

    explicit Window(PrStringView program, std::initializer_list<PrStringView> arguments) {
        t.kit = f.kit;
        t.ui = f.ui;
        t.fonts = f.fonts;
        t.program = program;
        for (PrStringView argument : arguments)
            t.arguments.push(String(argument));
        openApp(t);
        REQUIRE(openTab(t) == PrStatus_Ok);
        f.build = [this](PrLayoutNode root) { buildApp(t, root, PrVec2{800, 600}); };
        settle();
    }

    ~Window() { closeApp(t); }

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;

    // Runs frames until the app's layout settles.
    void settle() {
        f.frame();
        for (uint32_t i = 0; i < 8 && afterLayout(t); ++i)
            f.frame();
        // A terminal that asked for focus receives it in the next frame.
        f.frame();
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

    PrRect shownBounds() { return f.bounds(prTerminalNode(t.terminals[t.shown])); }

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
    REQUIRE(a.t.terminals.count() == 1);
    CHECK(a.t.strip == kPrLayoutNodeNone);
    PrRect bounds = a.shownBounds();
    CHECK(bounds.top == 0);
    CHECK(bounds.bottom == doctest::Approx(600.0));
    CHECK(prTerminalHasFocus(a.t.terminals[0]));
}

TEST_CASE("terminal app: a new tab shows the strip and takes focus, and the commands move between the tabs") {
    Window a("sh", {"-c", "sleep 5"});
    a.run("tab.new");
    REQUIRE(a.t.terminals.count() == 2);
    CHECK(a.t.shown == 1);
    CHECK(a.t.strip != kPrLayoutNodeNone);
    CHECK(a.t.stripHeight > 0);
    CHECK(a.shownBounds().top == doctest::Approx(double(a.t.stripHeight)));
    CHECK(prTerminalHasFocus(a.t.terminals[1]));

    a.run("tab.next");
    CHECK(a.t.shown == 0);
    CHECK(prTerminalHasFocus(a.t.terminals[0]));
    a.run("tab.previous");
    CHECK(a.t.shown == 1);

    a.run("tab.close");
    REQUIRE(a.t.terminals.count() == 1);
    CHECK(a.t.shown == 0);
    CHECK(a.t.strip == kPrLayoutNodeNone);
    CHECK(prTerminalHasFocus(a.t.terminals[0]));
}

TEST_CASE("terminal app: the window clears to the shown terminal's background, which its view paints none of") {
    Window a("sh", {"-c", "sleep 10"});
    a.run("tab.new");
    REQUIRE(a.t.terminals.count() == 2);
    // The first tab's program sets its own background color.
    PrStringView magenta = "\x1b]11;#ff00ff\x07";
    prVtermWrite(prTerminalVtermLock(a.t.terminals[0]), PrByteView{prStringBytes(magenta), magenta.size});
    prTerminalVtermUnlock(a.t.terminals[0]);
    for (uint32_t shown : {1u, 0u}) {
        if (a.t.shown != shown)
            a.run("tab.next");
        REQUIRE(a.t.shown == shown);
        PrTerminal* terminal = a.t.terminals[shown];
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
        PrVec2 strip{799, a.t.stripHeight / 2};
        CHECK(a.pixel(strip, KitCanvas_Unfilled).a == 255);
        CHECK(near(a.pixel(strip), a.f.kit->theme.palette.bg1));
    }
    CHECK(near(a.pixel(PrVec2{400, 300}), PrColor{1, 0, 1, 1}));
}

TEST_CASE("terminal app: a click on a tab's notification shows the tab and gives it focus") {
    Window a("sh", {"-c", "sleep 5"});
    a.run("tab.new");
    a.run("tab.new");
    REQUIRE(a.t.terminals.count() == 3);
    uint64_t first = prTerminalNotificationTag(a.t.terminals[0]);
    CHECK(first != prTerminalNotificationTag(a.t.terminals[1]));
    CHECK(prTerminalNotificationTag(a.t.terminals[1]) != prTerminalNotificationTag(a.t.terminals[2]));
    CHECK(a.t.shown == 2);

    PrAppEvent click{.kind = PrAppEventKind_Notification, .tag = first};
    CHECK(handleEvent(a.t, click));
    a.settle();
    CHECK(a.t.shown == 0);
    CHECK(prTerminalHasFocus(a.t.terminals[0]));

    // A click on the notification of a tab that has since closed leaves the shown tab as it is.
    PrAppEvent stale{.kind = PrAppEventKind_Notification, .tag = 1000};
    CHECK(handleEvent(a.t, stale));
    CHECK(a.t.shown == 0);
    PrAppEvent key{.kind = PrAppEventKind_KeyDown, .keyboard = {.key = 'A'}};
    CHECK_FALSE(handleEvent(a.t, key));
}

TEST_CASE("terminal app: a tab closes once its program exits") {
    Window a("sh", {"-c", "sleep 5"});
    a.t.program = "sh";
    a.t.arguments.clear();
    a.t.arguments.push(String("-c"));
    a.t.arguments.push(String("exit 0"));
    a.run("tab.new");
    a.frameUntil([&] { return a.t.terminals.count() == 1; });
    CHECK(a.t.shown == 0);
}

TEST_CASE("terminal app: a tab that does not show reads all that its program writes, and closes once it exits") {
    // The program writes more than the process and the pseudoterminal can buffer, after the second tab shows.
    Window a("sh", {"-c", "sleep 0.3; head -c 8000000 /dev/zero; exit 0"});
    PrTerminal* writer = a.t.terminals[0];
    a.t.program = "sh";
    a.t.arguments.clear();
    a.t.arguments.push(String("-c"));
    a.t.arguments.push(String("sleep 10"));
    a.run("tab.new");
    REQUIRE(a.t.terminals.count() == 2);
    a.frameUntil([&] { return a.t.terminals.count() == 1; });
    CHECK(a.t.terminals[0] != writer);
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
    prVtermWrite(prTerminalVtermLock(a.t.terminals[0]), PrByteView{prStringBytes(report), report.count()});
    prTerminalVtermUnlock(a.t.terminals[0]);
    a.t.arguments.clear();
    a.t.arguments.push(String("-c"));
    a.t.arguments.push(String(print));
    a.run("tab.new");
    REQUIRE(a.t.terminals.count() == 2);
    PrTerminal* terminal = a.t.terminals[1];
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
    REQUIRE(a.t.terminals.count() == 2);
    PrVec2 cell = prTerminalCellExtent(a.t.terminals[1]);
    a.run("view.zoomIn");
    a.run("view.zoomIn");
    PrVec2 larger = prTerminalCellExtent(a.t.terminals[1]);
    CHECK(larger.y > cell.y);
    a.run("tab.next");
    CHECK(prTerminalCellExtent(a.t.terminals[0]).y == doctest::Approx(double(larger.y)));
    a.run("view.zoomReset");
    CHECK(prTerminalCellExtent(a.t.terminals[0]).y == doctest::Approx(double(cell.y)));
    a.run("view.zoomOut");
    CHECK(prTerminalCellExtent(a.t.terminals[0]).y < cell.y);
}
