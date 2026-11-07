// Tests the terminal frame by frame, driven as an application drives it, with programs that the tests run.

#include "pr/array.h"
#include "pr/atomic.h"
#include "pr/base64.h"
#include "pr/count_of.h"
#include "pr/draw_commands.h"
#include "pr/draw_list.h"
#include "pr/environment.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/function.h"
#include "pr/kit_theme.h"
#include "pr/layout_frame.h"
#include "pr/min_max.h"
#include "pr/process.h"
#include "pr/process_backend.h"
#include "pr/raw_array.h"
#include "pr/span.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/terminal.h"
#include "pr/text_font.h"
#include "pr/thread.h"
#include "pr/time.h"
#include "pr/ui.h"
#include "pr/url_file.h"
#include "pr/vterm.h"
#include "pr/vterm_graphics.h"

#include "studio/terminal/src/terminal_draw.h"
#include "studio/terminal/src/terminal_link.h"
#include "studio/terminal/src/terminal_shell.h"
#include "tools/harness/kit_fixture.h"

#include "tools/harness/test.h"

extern "C" int prTestTerminalCApi(void);

using namespace harness;
using namespace pr;

namespace pr_impl {

namespace {

// How long a test waits for its program, in milliseconds.
constexpr double kPatience = 10000;

// An application that records what the terminal asks of it: the URLs to open, the clipboard text, the notifications to
// post (as title|body|tag), and the requests for attention. It counts its wakes, which come from the threads of the
// terminal's process. Its window is active until a test clears `active`. Each post returns `postStatus`.
struct FakeApp {
    PrApp app;
    Atomic<uint32_t> wakes{};
    Array<String> urls;
    String clipboard = "";
    bool clipboardRequested = false;
    bool active = true;
    Array<String> posted;
    PrStatus postStatus = PrStatus_Ok;
    uint32_t attention = 0;

    FakeApp() {
        app = PrApp{};
        app.wake = [](PrApp* a) { reinterpret_cast<FakeApp*>(a)->wakes.fetchAdd(1); };
        app.isActive = [](PrApp* a) { return reinterpret_cast<FakeApp*>(a)->active; };
        app.requestAttention = [](PrApp* a) { ++reinterpret_cast<FakeApp*>(a)->attention; };
        app.notificationPost = [](PrApp* a, const PrAppNotificationDesc* desc) -> PrStatus {
            FakeApp* fake = reinterpret_cast<FakeApp*>(a);
            fake->posted.push(format("{}|{}|{}", desc->title, desc->body, desc->tag));
            return fake->postStatus;
        };
        app.openUrl = [](PrApp* a, PrStringView url) -> PrStatus {
            reinterpret_cast<FakeApp*>(a)->urls.push(String(url));
            return PrStatus_Ok;
        };
        app.setClipboardText = [](PrApp* a, PrStringView text) -> PrStatus {
            reinterpret_cast<FakeApp*>(a)->clipboard.assign(text.isNull() ? PrStringView("") : text);
            return PrStatus_Ok;
        };
        app.requestClipboardText = [](PrApp* a) { reinterpret_cast<FakeApp*>(a)->clipboardRequested = true; };
    }

    FakeApp(const FakeApp&) = delete;
    FakeApp& operator=(const FakeApp&) = delete;

    // Takes the wakes since the last call, and returns true if there were any.
    bool woke() { return wakes.exchange(0) != 0; }
};

// A launcher whose process is the test: the program's output is what the test passes to `output`, at the speed of
// memory, with no pseudoterminal or program between. The process runs until the terminal destroys it, and keeps what
// the terminal writes to it, for `input`. A test that calls `output` from a thread of its own joins the thread before
// the terminal is destroyed, as a backend stops its thread when its process is destroyed.
struct FakeLauncher {
    // The process, and the launcher that it belongs to.
    struct FakeProcess {
        PrProcess process;
        FakeLauncher* launcher;
    };

    PrProcessLauncher launcher{};
    FakeProcess process{{}, this};
    PrProcessReceiveFn PR_NULLABLE receive = nullptr;
    void* PR_NULLABLE receiveContext = nullptr;
    PrMutex mutex;
    String written = "";

    FakeLauncher() {
        prMutexInit(&mutex);
        launcher.destroyFn = [](PrProcessLauncher*) {};
        launcher.launchFn = [](PrProcessLauncher* l, const PrProcessDesc* desc, PrProcess* PR_NULLABLE* out) -> PrStatus {
            FakeLauncher* fake = reinterpret_cast<FakeLauncher*>(l);
            fake->receive = desc->receive;
            fake->receiveContext = desc->receiveContext;
            *out = &fake->process.process;
            return PrStatus_Ok;
        };
        PrProcess& p = process.process;
        p.destroyFn = [](PrProcess*) {};
        // The terminal writes from the UI's thread and from the parser's.
        p.writeFn = [](PrProcess* written, PrByteView bytes) {
            FakeLauncher* fake = reinterpret_cast<FakeProcess*>(written)->launcher;
            prMutexLock(&fake->mutex);
            fake->written.append(prStringFromBytes(bytes));
            prMutexUnlock(&fake->mutex);
        };
        p.closeInputFn = [](PrProcess*) {};
        p.readFn = [](PrProcess*, PrProcessStream, uint8_t* PR_NULLABLE, size_t) { return size_t(0); };
        p.infoFn = [](const PrProcess*) { return PrProcessInfo{}; };
        p.killFn = [](PrProcess*) {};
        p.terminalResizeFn = [](PrProcess*, PrUVec2, PrUVec2) {};
        p.terminalForegroundDirectoryFn = [](const PrProcess*) { return prRawStringNull(); };
    }

    ~FakeLauncher() { prMutexReset(&mutex); }

    FakeLauncher(const FakeLauncher&) = delete;
    FakeLauncher& operator=(const FakeLauncher&) = delete;

    // Gives `text` to the terminal as the program's output, on the calling thread, as a backend's thread gives it. The
    // terminal must have launched its program through this launcher.
    void output(PrStringView text) {
        receive(receiveContext, &process.process, PrProcessStream_Output, prStringBytes(text));
    }

    // What the terminal has written to the program.
    String input() {
        prMutexLock(&mutex);
        String text = written;
        prMutexUnlock(&mutex);
        return text;
    }

    // Sets `desc` to launch through this launcher (Term's setup).
    Function<void(PrTerminalDesc*)> setup() {
        return [this](PrTerminalDesc* desc) { desc->launcher = &launcher; };
    }
};

// Gives `text` to a terminal from a thread of its own, as a program's output, until stopped. Stopping joins the thread.
struct Flood {
    FakeLauncher* fake;
    String text;
    Atomic<uint32_t> stopping{};
    PrThread thread{};

    Flood(FakeLauncher* launcher, PrStringView flooded) : fake(launcher), text(flooded) {
        prThreadStart(&thread, [](void* context) {
            Flood* flood = static_cast<Flood*>(context);
            while (flood->stopping.load() == 0)
                flood->fake->output(flood->text);
        }, this);
    }

    ~Flood() { stop(); }

    Flood(const Flood&) = delete;
    Flood& operator=(const Flood&) = delete;

    void stop() {
        if (stopping.exchange(1) == 0)
            prThreadJoin(&thread);
    }
};

// The modifier of a click that opens a link.
PrAppModifierFlags primaryModifier() {
#if defined(__APPLE__)
    return PrAppModifierFlag_Super;
#else
    return PrAppModifierFlag_Control;
#endif
}

// The modifiers of the keys that copy and paste.
PrAppModifierFlags clipboardModifiers() {
#if defined(__APPLE__)
    return PrAppModifierFlag_Super;
#else
    return PrAppModifierFlag_Control | PrAppModifierFlag_Shift;
#endif
}

// The monospaced family that every test terminal uses.
const PrFontFamily kMono{.generic = PrFontGeneric_Monospace};

// Holds a terminal's screen locked (prTerminalVtermLock) for the lifetime of the value. No other call on the terminal,
// such as a frame, may be made while it lives.
struct Locked {
    PrTerminal* terminal;
    PrVterm* vterm;

    explicit Locked(PrTerminal* locked) : terminal(locked), vterm(prTerminalVtermLock(locked)) {}
    ~Locked() { prTerminalVtermUnlock(terminal); }

    Locked(const Locked&) = delete;
    Locked& operator=(const Locked&) = delete;

    operator PrVterm*() const { return vterm; }
};

// A terminal in its own kit, with a viewport of 640 by 480 px. It runs `program` with `arguments` in `directory`, or in
// the launcher's directory if `directory` is the null view. `app` may be null.
struct Term {
    KitFixture f;
    PrProcessLauncher* launcher = prProcessLauncherCreate(PrProcessBackend_Default);
    PrTerminal* terminal = nullptr;
    PrVec2 viewport{640, 480};

    // If `setup` is set, it edits the terminal's description before the terminal is created.
    Term(PrStringView program, Span<const PrStringView> arguments, PrApp* PR_NULLABLE app = nullptr,
            PrStringView directory = {}, const Function<void(PrTerminalDesc*)>& setup = {},
            const KitFonts& fonts = KitFonts{}) : f(fonts) {
        PrTerminalDesc desc{
            .kit = f.kit,
            .font = font(13),
            .app = app,
            .launcher = launcher,
            .program = program,
            .arguments = arguments.data(),
            .argumentCount = arguments.count(),
            .directory = directory,
            .scrollback = 100,
            .notificationTag = 9,
        };
        if (setup)
            setup(&desc);
        REQUIRE(prTerminalCreate(&desc, &terminal) == PrStatus_Ok);
        // As in the terminal app, the terminal gives its background to the root, and the canvas shows it.
        f.build = [this](PrLayoutNode root) { prTerminalFrame(terminal, root, viewport, PrTerminalBackground_Parent); };
        f.frame();
    }

    ~Term() {
        prTerminalDestroy(terminal);
        prProcessLauncherDestroy(launcher);
    }

    Term(const Term&) = delete;
    Term& operator=(const Term&) = delete;

    // The monospaced font from the fixture's font set, at `size`.
    PrTextFontDesc font(float size) const {
        PrFontStyle style = prFontStyleDefault();
        style.size = size;
        return PrTextFontDesc{.set = f.fonts, .families = &kMono, .familyCount = 1, .style = style};
    }

    // The working directory that the terminal reports.
    String directory() const { return String::adopt(prTerminalWorkingDirectory(terminal)); }

    // The text of the screen's rows, each followed by a line feed, without trailing spaces.
    String screen() const {
        Locked vterm(terminal);
        PrUVec2 extent = prVtermExtent(vterm);
        String out = "";
        for (uint32_t row = 0; row < extent.y; ++row) {
            PrRawString text = prRawStringNull();
            uint32_t index = prVtermScrollbackCount(vterm) + row;
            prVtermLineAppendText(vterm, index, 0, extent.x, &text);
            out.append(prStringTrimEndByte(prRawStringView(&text), ' '));
            out.append('\n');
            prRawStringReset(&text);
        }
        return out;
    }

    // Runs frames until `done` returns true. Fails the test if that takes longer than kPatience.
    void frameUntil(const Function<bool()>& done) {
        double end = prTimeMonotonicMilliseconds() + kPatience;
        while (!done()) {
            if (prTimeMonotonicMilliseconds() >= end)
                FAIL("the screen shows:\n" << screen());
            prThreadSleep(2);
            f.frame();
        }
    }

    // Focuses the view and runs frames until it has focus, so that the keys pushed after it reach the view. The view
    // takes focus only in a frame after the request, and a frame routes each key press in a round of its own.
    void focus() {
        prTerminalFocus(terminal);
        frameUntil([&] { return prTerminalHasFocus(terminal); });
    }

    // Waits until the screen has only what the test writes to it (write). A Windows pseudoconsole clears the screen and
    // sets the title shortly after the launch, which would erase anything written before it, so this runs frames until
    // the title changes from the program's name. Elsewhere the screen is ready at once. The program must write nothing,
    // as sleep does.
    void settle() {
#if defined(_WIN32)
        String name(prTerminalTitle(terminal));
        frameUntil([&] { return prTerminalTitle(terminal) != name.view(); });
#endif
    }

    bool shows(PrStringView text) const { return prStringContains(screen(), text); }

    // The center of the screen cell at `column` and `row`, in viewport coordinates.
    PrVec2 cellCenter(uint32_t column, uint32_t row) {
        PrRect bounds = f.bounds(prTerminalNode(terminal));
        PrVec2 origin = prTerminalScreenOrigin(terminal);
        PrVec2 cell = prTerminalCellExtent(terminal);
        float x = bounds.left + origin.x + (float(column) + 0.5f) * cell.x;
        return PrVec2{x, bounds.top + origin.y + (float(row) + 0.5f) * cell.y};
    }

    String selection() const { return String::adopt(prTerminalSelection(terminal)); }

    void press(PrVec2 at, uint32_t clicks, PrAppModifierFlags modifiers = 0) {
        f.push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .modifiers = modifiers, .clicks = clicks, .position = at});
    }

    void release(PrVec2 at, PrAppModifierFlags modifiers = 0) {
        f.push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .modifiers = modifiers, .position = at});
    }

    void move(PrVec2 at, PrAppModifierFlags modifiers) {
        f.push(PrAppEvent{.kind = PrAppEventKind_PointerMove, .modifiers = modifiers, .position = at});
    }

    // Writes `text` to the screen as if the program wrote it.
    void write(PrStringView text) {
        prVtermWrite(Locked(terminal), prStringBytes(text));
    }

    // Renders the tree that the last frame laid out, and returns the color of the pixel at `at` in the viewport.
    // `canvas` sets the pixels before the tree paints.
    PrColor8 pixel(PrVec2 at, KitCanvas canvas = KitCanvas_Cleared) {
        Array<uint8_t> rgba;
        REQUIRE(f.render(&rgba, canvas));
        uint32_t i = (uint32_t(at.y) * KitFixture::kRenderWidth + uint32_t(at.x)) * 4;
        return PrColor8{rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
    }
};

// Returns the binary's folder (harness::testFolder), after writing a file of three lines named `name` in it. The path
// is normalized, as the terminal's paths are.
String folderWithFile(PrStringView name) {
    String folder = String::adopt(prFilePathAbsolute(harness::testFolder(), ""));
    PrStringView text = "one\ntwo\nthree\n";
    REQUIRE(prFileWrite(concat(folder, "/", name), prStringBytes(text)));
    return folder;
}

// A file link that the terminal passed to its handler for opening files.
struct OpenedFile {
    String path;
    uint32_t line;
    uint32_t column;

    bool operator==(const OpenedFile& other) const {
        return path == other.path && line == other.line && column == other.column;
    }
};

// Describes the links found in `text`. A URL is "url|<url>", and a file reference is "<text>|<path>|<line>|<column>".
Array<String> textLinks(PrStringView text) {
    RawArray<TerminalTextLink> links;
    terminalTextLinks(text, &links);
    Array<String> out;
    for (const TerminalTextLink& link : links.span()) {
        PrStringView target = prStringSlice(text, link.targetStart, link.targetEnd - link.targetStart);
        if (link.kind == TerminalTextLinkKind_Url) {
            out.push(concat("url|", target));
            continue;
        }
        out.push(format("{}|{}|{}|{}", prStringSlice(text, link.start, link.end - link.start), target, link.line,
            link.column));
    }
    links.reset();
    return out;
}

// Returns true if each of the red, green and blue channels of `a` is within two of `b`. The tolerance covers the
// rounding of rendering's conversions.
bool near(PrColor8 a, PrColor8 b) {
    auto close = [](uint8_t x, uint8_t y) { return absDifference(x, y) <= 2; };
    return close(a.r, b.r) && close(a.g, b.g) && close(a.b, b.b);
}

// Creates and returns the folder `name` in the binary's folder (harness::testFolder).
String subfolder(PrStringView name) {
    String folder = concat(harness::testFolder(), "/", name);
    REQUIRE(prFileCreateDirectory(folder));
    return folder;
}

void writeFile(PrStringView path, PrStringView text) {
    REQUIRE(prFileWrite(path, prStringBytes(text)));
}

// Returns the text of `string`, and releases `string`.
String taken(PrRawString string) {
    String text = String::adopt(string);
    if (text.isNull())
        text = "";
    return text;
}

Array<String> strings(const RawArray<PrRawString>& array) {
    Array<String> out;
    for (const PrRawString& string : array.span())
        out.push(String(prRawStringView(&string)));
    return out;
}

PrStringView nameOf(PrStringView path) {
    return prFilePathName(path);
}

// The installed shells that the terminal has an integration for, with one path for each shell.
Array<String> integratedShells() {
    Array<String> found;
    const char* const paths[] = {"/bin/zsh", "/usr/bin/zsh", "/bin/bash", "/usr/bin/bash"};
    for (const char* path : paths) {
        bool seen = false;
        for (const String& shell : found)
            seen = seen || nameOf(shell) == nameOf(path);
        PrFileInfo info{};
        if (!seen && prFileInfo(path, &info) && info.kind == PrFileKind_Regular)
            found.push(String(path));
    }
    return found;
}

// The modifiers of the keys that scroll to a prompt.
PrAppModifierFlags promptModifiers() {
#if defined(__APPLE__)
    return PrAppModifierFlag_Super;
#else
    return PrAppModifierFlag_Control | PrAppModifierFlag_Shift;
#endif
}

// What a shell with the terminal's integration writes for a prompt.
constexpr PrStringView kPrompt = "\x1b]133;A\x07$ \x1b]133;B\x07";

// What a shell with the terminal's integration writes for one command: a prompt, the typed `input`, each line of
// `output`, and the command's end with the exit code `exit`.
String command(PrStringView input, Span<const PrStringView> output, PrStringView exit) {
    String text = concat(kPrompt, input, "\r\n\x1b]133;C\x07");
    for (PrStringView line : output) {
        text.append(line);
        text.append("\r\n");
    }
    text.append("\x1b]133;D;");
    text.append(exit);
    text.append("\x07");
    return text;
}

// Views of `strings`, valid while they are.
Array<PrStringView> viewsOf(const Array<String>& strings) {
    Array<PrStringView> views;
    for (const String& text : strings)
        views.push(text);
    return views;
}

// `count` lines of output, each `prefix` followed by its index.
Array<String> numbered(PrStringView prefix, uint32_t count) {
    Array<String> lines;
    for (uint32_t i = 0; i < count; ++i)
        lines.push(format("{}{}", prefix, i));
    return lines;
}

String joined(const Array<String>& lines) {
    String text = "";
    for (const String& line : lines) {
        if (!text.isEmpty())
            text.append('\n');
        text.append(line);
    }
    return text;
}

// The middle of the gutter beside view row `row`, in viewport coordinates.
PrVec2 gutterAt(Term& t, uint32_t row) {
    PrRect bounds = t.f.bounds(prTerminalNode(t.terminal));
    PrVec2 cell = prTerminalCellExtent(t.terminal);
    float middle = prTerminalScreenOrigin(t.terminal).x / 2;
    return PrVec2{bounds.left + middle, bounds.top + (float(row) + 0.5f) * cell.y};
}

PrColor8 color8Of(PrColor color) {
    auto channel = [](float value) { return uint8_t(value * 255 + 0.5f); };
    return PrColor8{channel(color.r), channel(color.g), channel(color.b), 0xff};
}

// The sequence that opens a hyperlink to `uri` with `params` (OSC 8), and the one that closes it.
String hyperlink(PrStringView uri, PrStringView params = "") {
    return concat("\x1b]8;", params, ";", uri, "\x1b\\");
}
constexpr PrStringView kHyperlinkEnd = "\x1b]8;;\x1b\\";

// Renders the tree that the last frame laid out, and returns true if any pixel in the lower half of the cell at
// `column` and `row` is near `color`.
bool cellHasColor(Term& t, uint32_t column, uint32_t row, PrColor8 color) {
    Array<uint8_t> rgba;
    REQUIRE(t.f.render(&rgba));
    PrVec2 center = t.cellCenter(column, row);
    PrVec2 cell = prTerminalCellExtent(t.terminal);
    for (uint32_t y = uint32_t(center.y); y < uint32_t(center.y + cell.y / 2); ++y) {
        for (uint32_t x = uint32_t(center.x - cell.x / 2) + 1; x + 1 < uint32_t(center.x + cell.x / 2); ++x) {
            uint32_t i = (y * KitFixture::kRenderWidth + x) * 4;
            if (near(PrColor8{rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]}, color))
                return true;
        }
    }
    return false;
}

// The pixels of a cell that are not near the screen's background. `inked` holds a flag for each pixel of the cell, row
// by row. `bounds` is in viewport coordinates.
struct CellInk {
    uint32_t count = 0;
    Array<bool> inked;
    PrRect bounds{1e9f, 1e9f, -1e9f, -1e9f};
};

// Renders the tree that the last frame laid out, and returns the ink of the cell at `column` and `row`.
CellInk cellInk(Term& t, uint32_t column, uint32_t row) {
    Array<uint8_t> rgba;
    REQUIRE(t.f.render(&rgba));
    PrColor8 background = prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Background);
    PrVec2 center = t.cellCenter(column, row);
    PrVec2 cell = prTerminalCellExtent(t.terminal);
    CellInk ink;
    for (uint32_t y = uint32_t(center.y - cell.y / 2); y < uint32_t(center.y + cell.y / 2); ++y) {
        for (uint32_t x = uint32_t(center.x - cell.x / 2); x < uint32_t(center.x + cell.x / 2); ++x) {
            uint32_t i = (y * KitFixture::kRenderWidth + x) * 4;
            bool inked = !near(PrColor8{rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]}, background);
            ink.inked.push(inked);
            if (!inked)
                continue;
            ++ink.count;
            ink.bounds.left = min(ink.bounds.left, float(x));
            ink.bounds.top = min(ink.bounds.top, float(y));
            ink.bounds.right = max(ink.bounds.right, float(x + 1));
            ink.bounds.bottom = max(ink.bounds.bottom, float(y + 1));
        }
    }
    return ink;
}

// A graphics command with the `control` keys, which transmits `pixels` in base64.
String graphics(PrStringView control, PrStringView pixels = "") {
    Array<char> payload(prBase64EncodedSize(pixels.size));
    prBase64Encode(pixels, payload.data(), payload.count());
    String out = concat("\x1b_G", control);
    if (!pixels.isEmpty())
        out.append(concat(";", PrStringView(payload.data(), payload.count())));
    out.append("\x1b\\");
    return out;
}

// The pixel extent of a cell on the terminal's screen.
PrUVec2 cellPixelExtent(PrTerminal* terminal) {
    return prVtermCellPixelExtent(Locked(terminal));
}

// RGB pixels of one color, enough to fill `columns` by `rows` cells of the terminal's screen.
String cellPixels(PrTerminal* terminal, uint32_t columns, uint32_t rows, PrColor8 color) {
    PrUVec2 cell = cellPixelExtent(terminal);
    String out = "";
    for (uint32_t i = 0; i < cell.x * columns * cell.y * rows; ++i) {
        out.append(char(color.r));
        out.append(char(color.g));
        out.append(char(color.b));
    }
    return out;
}

// The control keys that transmit and put an RGB image of `columns` by `rows` cells without moving the cursor, followed
// by `keys`.
String cellImage(PrTerminal* terminal, uint32_t columns, uint32_t rows, PrStringView keys) {
    PrUVec2 cell = cellPixelExtent(terminal);
    return format("a=T,f=24,C=1,s={},v={}{}", cell.x * columns, cell.y * rows, keys);
}

constexpr PrColor8 kImageRed = {0xff, 0x00, 0x00, 0xff};

// Runs the frames that an application would run now: one for each wake of the program, and those that the terminal asks
// for at once. Returns the number of wakes.
uint32_t runWoken(Term& t, FakeApp& app) {
    uint32_t wakes = 0;
    for (uint32_t i = 0; i < 100; ++i) {
        bool woke = app.woke();
        wakes += woke ? 1 : 0;
        if (!woke && !prUiOutput(t.f.ui).wantsFrame)
            return wakes;
        t.f.frame();
    }
    FAIL("the terminal asks for frames without end");
    return wakes;
}

// The column of the left edge of the input method's caret that the last frame reported, counted in cells from the
// screen's first column, or a negative number if the frame reported none.
float textInputColumn(Term& t) {
    PrAppOutput output = prUiOutput(t.f.ui);
    if (!output.hasTextInput)
        return -1;
    PrRect bounds = t.f.bounds(prTerminalNode(t.terminal));
    float left = output.textInput.left - bounds.left - prTerminalScreenOrigin(t.terminal).x;
    return left / prTerminalCellExtent(t.terminal).x;
}

// Queues an update of the input method's composition to `text`, with the input method's caret at its end.
void compose(Term& t, PrStringView text) {
    uint32_t size = uint32_t(text.size);
    t.f.push(PrAppEvent{.kind = PrAppEventKind_CompositionUpdate, .text = text, .selection = {size, size}});
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("terminal: the public header compiles as C") {
    CHECK(prTestTerminalCApi() == 0);
}

TEST_CASE("terminal: the screen fills its extent with whole cells, and the program's terminal has its size") {
    const PrStringView arguments[] = {"-c", "stty size; sleep 5"};
    Term t("sh", arguments);
    PrUVec2 extent = prVtermExtent(Locked(t.terminal));
    CHECK(extent.x >= 60);
    CHECK(extent.x <= 100);
    CHECK(extent.y >= 20);
    CHECK(extent.y <= 40);
    String size = format("{} {}", extent.y, extent.x);
    t.frameUntil([&] { return t.shows(size); });
    CHECK(t.f.paintedCommands() > 0);
}

TEST_CASE("terminal: typed text reaches the program, whose terminal echoes it") {
    Term t("cat", {});
    t.focus();
    t.f.frame();
    REQUIRE(prUiFocus(t.f.ui) == prTerminalNode(t.terminal));
    t.f.type("hi");
    t.f.key(PrAppKey_Enter);
    // The terminal echoes the line, and then cat writes it.
    t.frameUntil([&] { return t.shows("hi\nhi\n"); });
}

TEST_CASE("terminal: the program sets the title, and its exit shows on the screen") {
    const PrStringView arguments[] = {"-c", "printf '\\033]2;Greeting\\007'; exit 3"};
    Term t("sh", arguments);
    t.frameUntil([&] { return t.shows("[exited with code 3]"); });
    CHECK(prTerminalTitle(t.terminal) == "Greeting");
    PrProcessInfo info = prTerminalProcessInfo(t.terminal);
    CHECK(info.exited);
    CHECK(info.code == 3);
}

TEST_CASE("terminal: a double click selects a word, a triple click a line, and a drag with Alt a block") {
    const PrStringView arguments[] = {"-c", "printf 'alpha beta-gamma delta\\nbravo charlie\\n'; sleep 5"};
    Term t("sh", arguments);
    t.frameUntil([&] { return t.shows("bravo charlie"); });
    PrVec2 beta = t.cellCenter(8, 0);
    t.f.move(beta);
    t.f.frame();
    t.press(beta, 2);
    t.release(beta);
    t.f.frame();
    CHECK(t.selection() == "beta-gamma");

    t.press(beta, 3);
    t.release(beta);
    t.f.frame();
    CHECK(t.selection() == "alpha beta-gamma delta");

    // A drag by words, from the second word of the first line to the first word of the second line.
    t.press(beta, 2);
    t.f.frame();
    t.f.move(t.cellCenter(1, 1));
    t.f.frame();
    t.release(t.cellCenter(1, 1));
    t.f.frame();
    CHECK(t.selection() == "beta-gamma delta\nbravo");

    t.press(t.cellCenter(0, 0), 1, PrAppModifierFlag_Alt);
    t.f.frame();
    t.f.move(t.cellCenter(2, 1));
    t.f.frame();
    t.release(t.cellCenter(2, 1), PrAppModifierFlag_Alt);
    t.f.frame();
    CHECK(t.selection() == "alp\nbra");

    // A click that does not move selects nothing.
    t.f.click(beta);
    t.f.frame();
    CHECK(t.selection().isEmpty());
}

TEST_CASE("terminal: the pointer over a URL with the primary modifier shows that a click opens it") {
    const PrStringView arguments[] = {"-c", "printf 'see (https://example.com/a), or not\\n'; sleep 5"};
    Term t("sh", arguments);
    t.frameUntil([&] { return t.shows("https://example.com/a"); });
    t.move(t.cellCenter(10, 0), primaryModifier());
    t.f.frame();
    CHECK(prUiOutput(t.f.ui).cursor == PrAppCursor_Hand);
    // Without the modifier, a click selects as it does anywhere.
    t.move(t.cellCenter(10, 0), 0);
    t.f.frame();
    CHECK(prUiOutput(t.f.ui).cursor == PrAppCursor_Text);
    // The closing parenthesis and the comma are not part of the URL.
    t.move(t.cellCenter(27, 0), primaryModifier());
    t.f.frame();
    CHECK(prUiOutput(t.f.ui).cursor == PrAppCursor_Text);
    t.move(t.cellCenter(32, 0), primaryModifier());
    t.f.frame();
    CHECK(prUiOutput(t.f.ui).cursor == PrAppCursor_Text);
}

TEST_CASE("terminal: a click with the primary modifier opens the URL under it, and a plain click does not") {
    FakeApp app;
    const PrStringView arguments[] = {
        "-c", "printf 'see (https://example.com/a) and www.example.org file:///tmp\\n'; sleep 5",
    };
    Term t("sh", arguments, &app.app);
    t.frameUntil([&] { return t.shows("file:///tmp"); });
    t.f.click(t.cellCenter(10, 0));
    t.f.frame();
    CHECK(app.urls.isEmpty());
    t.f.click(t.cellCenter(10, 0), primaryModifier());
    t.f.frame();
    // Only http and https URLs are found in the text.
    t.f.click(t.cellCenter(36, 0), primaryModifier());
    t.f.frame();
    t.f.click(t.cellCenter(50, 0), primaryModifier());
    t.f.frame();
    CHECK(app.urls == Array<String>({"https://example.com/a"}));
    CHECK(t.selection().isEmpty());
}

TEST_CASE("terminal: a hyperlink under the pointer with the primary modifier underlines all of its cells") {
    FakeApp app;
    const PrStringView arguments[] = {"10"};
    Term t("sleep", arguments, &app.app);
    t.settle();
    // One link in two parts, whose text differs from its URI, followed by a URL in plain text.
    t.write(concat(hyperlink("https://example.com/real", "id=a"), "one", kHyperlinkEnd, " gap ",
        hyperlink("https://example.com/real", "id=a"), "three", kHyperlinkEnd, " https://example.com/text"));
    t.f.frame();
    PrColor8 accent = color8Of(t.f.kit->theme.palette.accent);
    CHECK_FALSE(cellHasColor(t, 10, 0, accent));
    t.move(t.cellCenter(1, 0), primaryModifier());
    t.f.frame();
    CHECK(prUiOutput(t.f.ui).cursor == PrAppCursor_Hand);
    CHECK(cellHasColor(t, 1, 0, accent));
    CHECK(cellHasColor(t, 10, 0, accent));
    CHECK_FALSE(cellHasColor(t, 5, 0, accent));
    t.move(t.cellCenter(5, 0), primaryModifier());
    t.f.frame();
    CHECK(prUiOutput(t.f.ui).cursor == PrAppCursor_Text);
    CHECK_FALSE(cellHasColor(t, 10, 0, accent));

    t.f.click(t.cellCenter(10, 0), primaryModifier());
    t.f.frame();
    t.f.click(t.cellCenter(20, 0), primaryModifier());
    t.f.frame();
    CHECK(app.urls == Array<String>({"https://example.com/real", "https://example.com/text"}));
}

TEST_CASE("terminal: a hyperlink at rest has a faint dotted underline, and text without a link has none") {
    const PrStringView arguments[] = {"10"};
    Term t("sleep", arguments);
    t.settle();
    t.write(concat("____\r\n", hyperlink("https://example.com/"), "____", kHyperlinkEnd));
    t.f.frame();
    Array<uint8_t> rgba;
    REQUIRE(t.f.render(&rgba));
    // The lower halves of the cells in the two rows differ only by the link's underline.
    auto lowerHalf = [&](uint32_t row) {
        PrVec2 center = t.cellCenter(1, row);
        PrVec2 cell = prTerminalCellExtent(t.terminal);
        Array<uint8_t> pixels;
        for (uint32_t y = uint32_t(center.y); y < uint32_t(center.y + cell.y / 2); ++y) {
            uint32_t start = (y * KitFixture::kRenderWidth + uint32_t(center.x - cell.x / 2)) * 4;
            pixels.append(rgba.span().slice(start, uint32_t(cell.x) * 4));
        }
        return pixels;
    };
    CHECK(lowerHalf(0) != lowerHalf(1));
    CHECK_FALSE(cellHasColor(t, 1, 1, color8Of(t.f.kit->theme.palette.accent)));
}

TEST_CASE("terminal: a hyperlink to a file that exists opens through the handler, and any other through the app") {
    String folder = folderWithFile("linked.txt");
    FakeApp app;
    Array<OpenedFile> opened;
    auto setup = [&](PrTerminalDesc* desc) {
        desc->openFile = [](void* context, const PrTerminalFileLink* link) {
            static_cast<Array<OpenedFile>*>(context)->push(OpenedFile{String(link->path), link->line, link->column});
        };
        desc->openFileContext = &opened;
    };
    const PrStringView arguments[] = {"10"};
    Term t("sleep", arguments, &app.app, {}, setup);
    t.settle();
    String uri = taken(prUrlFromFilePath(concat(folder, "/linked.txt")));
    t.write(concat(hyperlink(uri), "here", kHyperlinkEnd, " ", hyperlink("file:///nowhere/missing.txt"), "gone",
        kHyperlinkEnd, " ", hyperlink("file://elsewhere.invalid/x"), "far", kHyperlinkEnd));
    t.f.frame();
    t.f.click(t.cellCenter(1, 0), primaryModifier());
    t.f.frame();
    t.f.click(t.cellCenter(6, 0), primaryModifier());
    t.f.frame();
    t.f.click(t.cellCenter(11, 0), primaryModifier());
    t.f.frame();
    CHECK(opened == Array<OpenedFile>({OpenedFile{concat(folder, "/linked.txt"), 0, 0}}));
    CHECK(app.urls == Array<String>({"file:///nowhere/missing.txt", "file://elsewhere.invalid/x"}));
}

TEST_CASE("terminal: a change of the palette, the roles or reverse video redraws the rows that it colors") {
    const PrStringView arguments[] = {"10"};
    Term t("sleep", arguments);
    t.settle();
    // Two full blocks, which the terminal draws as shapes that fill their cells. One is in the default foreground, and
    // one is in the palette's red.
    t.write("\xe2\x96\x88\x1b[31m\xe2\x96\x88\x1b[0m");
    t.f.frame();
    PrVec2 plain = t.cellCenter(0, 0);
    PrVec2 red = t.cellCenter(1, 0);
    CHECK(near(t.pixel(plain), prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Foreground)));
    CHECK(near(t.pixel(red), prVtermPaletteColor(Locked(t.terminal), 1)));

    // Reverse video draws the default foreground in the background's color.
    t.write("\x1b[?5h");
    t.f.frame();
    CHECK(near(t.pixel(plain), prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Background)));
    t.write("\x1b]11;#ff00ff\x07");
    t.f.frame();
    CHECK(near(t.pixel(plain), PrColor8{0xff, 0x00, 0xff, 0xff}));
    t.write("\x1b]4;1;#00ff00\x07");
    t.f.frame();
    CHECK(near(t.pixel(red), PrColor8{0x00, 0xff, 0x00, 0xff}));
}

TEST_CASE("terminal: the screen takes the colors of the kit's theme, and follows its changes") {
    const PrStringView arguments[] = {"10"};
    Term t("sleep", arguments);
    t.settle();
    auto color8 = [](PrColor color) {
        return PrColor8{uint8_t(color.r * 255 + 0.5f), uint8_t(color.g * 255 + 0.5f), uint8_t(color.b * 255 + 0.5f), 255};
    };
    CHECK(near(prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Background), color8(t.f.kit->theme.palette.bg1)));

    // A program's color lasts until the theme changes.
    t.write("\x1b]10;#ff00ff\x07");
    t.f.frame();
    CHECK(near(prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Foreground), PrColor8{0xff, 0x00, 0xff, 0xff}));

    PrKitTheme light = prKitThemeStudio(PrKitThemeVariant_Light, t.f.fonts);
    prKitSetTheme(t.f.kit, &light);
    t.f.frame();
    CHECK(near(prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Background), color8(light.palette.bg1)));
    CHECK(near(prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Foreground), color8(light.palette.text1)));
    CHECK(near(prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Cursor), color8(light.palette.accent)));
}

TEST_CASE("terminal: the view paints no background, and its owner's shows under the cells, the gutter and the edges") {
    const PrStringView arguments[] = {"10"};
    Term t("sleep", arguments);
    t.settle();
    t.viewport = PrVec2{float(KitFixture::kRenderWidth), float(KitFixture::kRenderHeight)};
    t.f.frame();
    PrColor8 background = prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Background);
    CHECK(near(color8Of(prTerminalBackground(t.terminal)), background));
    // The view paints nothing over a blank cell, the gutter, or the partial cell past the last whole cell. The window's
    // clear shows there.
    PrVec2 blank = t.cellCenter(10, 5);
    PrVec2 gutter = gutterAt(t, 5);
    PrVec2 corner{float(KitFixture::kRenderWidth) - 1, float(KitFixture::kRenderHeight) - 1};
    const PrVec2 points[] = {blank, gutter, corner};
    for (PrVec2 at : points) {
        CHECK(t.pixel(at, KitCanvas_Unfilled).a == 0);
        CHECK(near(t.pixel(at), background));
    }

    // Cells with another background, and an image below the cell backgrounds, draw over the owner's background.
    t.write("\x1b[44m \x1b[0m");
    t.write(graphics(cellImage(t.terminal, 1, 1, ",z=-1073741825"), cellPixels(t.terminal, 1, 1, kImageRed)));
    t.f.frame();
    CHECK(near(t.pixel(t.cellCenter(0, 0)), prVtermPaletteColor(Locked(t.terminal), 4)));
    CHECK(near(t.pixel(t.cellCenter(1, 0)), kImageRed));

    // The bell's flash draws over the owner's background.
    t.write("\a");
    t.f.frame();
    CHECK(t.pixel(blank, KitCanvas_Unfilled).a != 0);
    CHECK_FALSE(near(t.pixel(blank), background));
    t.f.frame(1);
    CHECK(near(t.pixel(blank), background));

    // Reverse video, the program's background and the theme each change the background that the owner paints.
    t.write("\x1b[?5h");
    t.f.frame();
    PrColor8 foreground = prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Foreground);
    CHECK(near(color8Of(prTerminalBackground(t.terminal)), foreground));
    CHECK(near(t.pixel(corner), foreground));
    t.write("\x1b[?5l\x1b]11;#ff00ff\x07");
    t.f.frame();
    PrColor8 magenta{0xff, 0x00, 0xff, 0xff};
    CHECK(near(color8Of(prTerminalBackground(t.terminal)), magenta));
    CHECK(near(t.pixel(corner), magenta));
    CHECK(near(t.pixel(blank), magenta));
    t.write("\x1b]111\x07");
    PrKitTheme light = prKitThemeStudio(PrKitThemeVariant_Light, t.f.fonts);
    prKitSetTheme(t.f.kit, &light);
    t.f.frame();
    CHECK(near(color8Of(prTerminalBackground(t.terminal)), color8Of(light.palette.bg1)));
    CHECK(near(t.pixel(gutter), color8Of(light.palette.bg1)));
    CHECK(kitVerified(t.f.tree));
}

TEST_CASE("terminal: a terminal that paints its background in its view shows it within the view alone") {
    const PrStringView arguments[] = {"10"};
    Term t("sleep", arguments);
    t.settle();
    // A panel in the window holds the terminal, which paints its own background, as in Scribe.
    t.f.build = [&t](PrLayoutNode root) {
        PrLayoutTree* tree = t.f.tree;
        PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
        box.display = PrLayoutDisplay_Block;
        box.width = prLayoutLengthPx(t.viewport.x);
        box.height = prLayoutLengthPx(t.viewport.y);
        PrLayoutStyle style = prLayoutStyleDefault();
        style.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
        PrLayoutNode panel = prLayoutTreeFrameElement(tree, root, 0, prLayoutStyleIntern(tree, &style));
        prTerminalFrame(t.terminal, panel, t.viewport, PrTerminalBackground_View);
    };
    t.write("\x1b]11;#ff00ff\x07");
    t.f.frame();
    t.f.frame();
    PrColor8 magenta{0xff, 0x00, 0xff, 0xff};
    CHECK(near(t.pixel(t.cellCenter(10, 5)), magenta));
    CHECK(near(t.pixel(gutterAt(t, 5)), magenta));
    CHECK(near(t.pixel(PrVec2{t.viewport.x - 1, t.viewport.y - 1}), magenta));
    // Past the panel, the window's background shows.
    CHECK(near(t.pixel(PrVec2{700, 500}), color8Of(t.f.kit->theme.palette.bg0)));
    CHECK(kitVerified(t.f.tree));
}

TEST_CASE("terminal: a bell flashes the screen and fades, and asks for attention once while the window is inactive") {
    FakeApp app;
    const PrStringView arguments[] = {"10"};
    Term t("sleep", arguments, &app.app);
    t.settle();
    PrVec2 blank = t.cellCenter(10, 5);
    PrColor8 background = prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Background);
    REQUIRE(near(t.pixel(blank), background));
    t.write("\a");
    t.f.frame();
    PrColor8 flashed = t.pixel(blank);
    CHECK_FALSE(near(flashed, background));
    CHECK(prUiOutput(t.f.ui).wantsFrame);
    t.f.frame();
    PrColor8 faded = t.pixel(blank);
    CHECK(faded.r < flashed.r);
    CHECK(faded.r > background.r);
    t.f.frame(1);
    CHECK(near(t.pixel(blank), background));
    // The window is active, so the user saw the flash.
    CHECK(app.attention == 0);
    CHECK(prTerminalAlerts(t.terminal) == 0);

    app.active = false;
    t.write("\a");
    t.f.frame();
    t.write("\a");
    t.f.frame();
    CHECK(app.attention == 1);
    CHECK(prTerminalAlerts(t.terminal) == PrTerminalAlertFlag_Bell);
    app.active = true;
    t.f.frame();
    CHECK(prTerminalAlerts(t.terminal) == 0);
    app.active = false;
    t.write("\a");
    t.f.frame();
    CHECK(app.attention == 2);
}

TEST_CASE("terminal: with reduced motion, the bell's flash holds still and ends at its time") {
    FakeApp app;
    const PrStringView arguments[] = {"10"};
    Term t("sleep", arguments, &app.app);
    t.settle();
    t.f.preferences.reducedMotion = true;
    t.f.frame();
    PrVec2 blank = t.cellCenter(10, 5);
    PrColor8 background = prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Background);
    t.write("\a");
    t.f.frame();
    PrColor8 flashed = t.pixel(blank);
    CHECK_FALSE(near(flashed, background));
    PrAppOutput output = prUiOutput(t.f.ui);
    CHECK_FALSE(output.wantsFrame);
    CHECK(output.frameTime > t.f.time);
    t.f.frame(0.01);
    CHECK(near(t.pixel(blank), flashed));
    t.f.frame(1);
    CHECK(near(t.pixel(blank), background));
}

TEST_CASE("terminal: a notification posts unless the terminal has focus in the active window, or as its condition says") {
    FakeApp app;
    // The notifications take the program's name as their title, so the test does not settle (Term::settle), which waits
    // for a Windows pseudoconsole's title. A notification acts on the screen's events, not its contents.
    const PrStringView arguments[] = {"10"};
    Term t("sleep", arguments, &app.app);
    // The terminal shows in the active window without focus. The default and unfocused conditions post, and the
    // invisible condition does not.
    t.write("\x1b]9;one\x07\x1b]99;o=unfocused;two\x1b\\\x1b]99;o=invisible;three\x1b\\");
    t.f.frame();
    CHECK(app.posted == Array<String>({"sleep|one|9", "two||9"}));
    CHECK(prTerminalAlerts(t.terminal) == 0);

    app.posted.clear();
    t.focus();
    t.write("\x1b]9;four\x07\x1b]99;o=always;five\x1b\\\x1b]777;notify;Six;six\x07");
    t.f.frame();
    CHECK(app.posted == Array<String>({"five||9"}));

    // In an inactive window, every notification posts and raises an alert until the window becomes active.
    app.posted.clear();
    app.active = false;
    t.write("\x1b]777;notify;Seven;seven\x07\x1b]99;o=invisible;eight\x1b\\");
    t.f.frame();
    CHECK(app.posted == Array<String>({"Seven|seven|9", "eight||9"}));
    CHECK(prTerminalAlerts(t.terminal) == PrTerminalAlertFlag_Notification);
    CHECK(app.attention == 0);
    app.active = true;
    t.f.frame();
    CHECK(prTerminalAlerts(t.terminal) == 0);

    // If the system cannot post notifications, the inactive window asks for attention instead.
    app.postStatus = PrStatus_NotSupported;
    app.active = false;
    t.write("\x1b]9;nine\x07");
    t.f.frame();
    CHECK(app.attention == 1);
}

TEST_CASE("terminal: a flood of output goes on showing in the frames that the terminal and its program ask for") {
    FakeApp app;
    FakeLauncher fake;
    Term t("program", {}, &app.app, {}, fake.setup());
    REQUIRE(fake.receive != nullptr);
    // Far more output than one frame parses, from the program's thread, followed by a marker.
    struct Output {
        FakeLauncher* fake;
        PrThread thread;
    } output{&fake, {}};
    prThreadStart(&output.thread, [](void* context) {
        FakeLauncher* fake = static_cast<Output*>(context)->fake;
        String chunk = String().append(4 * kPrTerminalParseSliceBytes, 'a');
        for (uint32_t i = 0; i < 64; ++i)
            fake->output(chunk);
        fake->output("\r\nflood-done\r\n");
    }, &output);
    // Frames run as an application runs them: only if the program woke it or the terminal asks for a frame.
    double end = prTimeMonotonicMilliseconds() + kPatience;
    while (!t.shows("flood-done")) {
        if (prTimeMonotonicMilliseconds() >= end)
            break;
        if (app.woke() || prUiOutput(t.f.ui).wantsFrame)
            t.f.frame();
        else
            prThreadSleep(1);
    }
    prThreadJoin(&output.thread);
    REQUIRE(t.shows("flood-done"));
    // Once the output stops, the terminal asks for no more frames. So the frames above ran because of the flood.
    t.f.frame();
    CHECK_FALSE(prUiOutput(t.f.ui).wantsFrame);
}

// The wait's duration is not checked: the scheduler can preempt the parser while it holds the lock, for as long as the
// machine's load keeps it off the processor. What the parser took during a wait is the bound that load cannot move.
TEST_CASE("terminal: while the program floods the screen, the UI waits for it no longer than a slice of parsing") {
    FakeLauncher fake;
    Term t("program", {}, nullptr, {}, fake.setup());
    REQUIRE(fake.receive != nullptr);
    // Colored text, wide characters and scrolling lines, which parse slower than plain text, in pieces of several
    // slices.
    PrStringView line = "\033[31mred \033[1;4mbold\033[0m \xe6\xbc\xa2\xe5\xad\x97 caf\xc3\xa9 plain text\r\n";
    String text = "";
    while (text.count() < 4 * kPrTerminalParseSliceBytes)
        text.append(line);
    // Frames until the UI has waited for the parser in the middle of its output, and many times over.
    PrTerminalLockInfo info{};
    {
        Flood flood(&fake, text);
        double end = prTimeMonotonicMilliseconds() + kPatience;
        while ((info.worstBytes == 0 || info.count < 20) && prTimeMonotonicMilliseconds() < end) {
            t.f.frame();
            info = prTerminalLockInfo(t.terminal);
        }
    }
    REQUIRE(info.worstBytes != 0);
    CHECK(info.count >= 20);
    CHECK(info.worstBytes <= kPrTerminalParseSliceBytes);
}

TEST_CASE("terminal: a synchronized update wakes the application as it begins and as it ends, and not between") {
    FakeApp app;
    FakeLauncher fake;
    Term t("program", {}, &app.app, {}, fake.setup());
    REQUIRE(fake.receive != nullptr);
    fake.output("before");
    runWoken(t, app);
    REQUIRE(t.shows("before"));
    // Outside a synchronized update, each of the 20 writes would wake the application.
    uint32_t wakes = 0;
    fake.output("\x1b[?2026hduring");
    wakes += runWoken(t, app);
    for (uint32_t i = 0; i < 20; ++i) {
        fake.output(".");
        wakes += runWoken(t, app);
    }
    fake.output("\x1b[?2026l after");
    wakes += runWoken(t, app);
    CHECK(t.shows("before"));
    CHECK(t.shows("during.................... after"));
    CHECK(wakes == 2);
}

TEST_CASE("terminal: a terminal that does not show wakes the application for the program's events, not its output") {
    FakeApp app;
    FakeLauncher fake;
    Term t("program", {}, &app.app, {}, fake.setup());
    REQUIRE(fake.receive != nullptr);
    t.f.build = [&](PrLayoutNode) { prTerminalUpdate(t.terminal); };
    t.f.frame();
    runWoken(t, app);
    uint32_t wakes = 0;
    for (uint32_t i = 0; i < 20; ++i) {
        fake.output(format("line {}\r\n", i));
        wakes += runWoken(t, app);
    }
    CHECK(wakes == 0);
    fake.output("\a");
    wakes += runWoken(t, app);
    CHECK(prTerminalAlerts(t.terminal) == PrTerminalAlertFlag_Bell);
    CHECK(wakes == 1);
    CHECK(t.shows("line 19"));
}

TEST_CASE("terminal: the key that pastes drops only its own text, and text typed after it reaches the program") {
    FakeApp app;
    Term t("cat", {}, &app.app);
    t.focus();
    t.f.frame();
    // The platform delivers no text for the chord.
    t.f.push(PrAppEvent{.kind = PrAppEventKind_KeyDown, .modifiers = clipboardModifiers(), .key = 'V', .text = "v"});
    t.f.frame();
    CHECK(app.clipboardRequested);
    t.f.type("x");
    t.f.key(PrAppKey_Enter);
    t.frameUntil([&] { return t.shows("x\nx\n"); });
}

TEST_CASE("terminal: the clipboard's keys copy the selection and paste, in each platform's chords") {
    FakeApp app;
    FakeLauncher fake;
    Term t("program", {}, &app.app, {}, fake.setup());
    REQUIRE(fake.receive != nullptr);
    t.focus();
    t.f.frame();
    t.write("\x1b[5;1Halpha beta");
    t.f.frame();
    PrVec2 alpha = t.cellCenter(1, 4);
    t.f.move(alpha);
    t.f.frame();
    t.press(alpha, 2);
    t.release(alpha);
    t.f.frame();
    REQUIRE(t.selection() == "alpha");
    t.f.key('C', clipboardModifiers());
    t.f.frame();
    CHECK(app.clipboard == "alpha");
    t.f.key('V', clipboardModifiers());
    t.f.frame();
    CHECK(app.clipboardRequested);
#if !defined(__APPLE__)
    app.clipboard.clear();
    app.clipboardRequested = false;
    t.f.key(PrAppKey_Insert, PrAppModifierFlag_Control);
    t.f.frame();
    CHECK(app.clipboard == "alpha");
    t.f.key(PrAppKey_Insert, PrAppModifierFlag_Shift);
    t.f.frame();
    CHECK(app.clipboardRequested);
#endif
#if defined(_WIN32)
    app.clipboard.clear();
    app.clipboardRequested = false;
    t.f.key('V', PrAppModifierFlag_Control);
    t.f.frame();
    CHECK(app.clipboardRequested);
    // Control with C copies and clears the selection. Pressed again, it goes to the program, as the interrupt.
    t.f.key('C', PrAppModifierFlag_Control);
    t.f.frame();
    CHECK(app.clipboard == "alpha");
    CHECK(t.selection().isEmpty());
    CHECK(fake.input().isEmpty());
    t.f.key('C', PrAppModifierFlag_Control);
    t.f.frame();
    CHECK(fake.input() == "\x03");
#else
    // The clipboard's keys write nothing to the program.
    CHECK(fake.input().isEmpty());
#endif
}

TEST_CASE("terminal: a program that asks for key releases has them, but for the keys that the view takes") {
    FakeApp app;
    const PrStringView arguments[] = {"-c", "stty raw -echo; printf '\\033[>2u'; echo ready; cat -v"};
    Term t("sh", arguments, &app.app);
    t.focus();
    t.frameUntil([&] { return t.shows("ready"); });
    auto stroke = [&](PrAppKey key, PrAppModifierFlags modifiers, const char* text, PrAppKeyCode code) {
        t.f.push(PrAppEvent{
            .kind = PrAppEventKind_KeyDown,
            .modifiers = modifiers,
            .key = key,
            .code = code,
            .text = text,
        });
        if (text[0] != 0)
            t.f.type(text);
        t.f.push(PrAppEvent{.kind = PrAppEventKind_KeyUp, .modifiers = modifiers, .key = key, .code = code});
    };
    stroke('A', 0, "a", PrAppKeyCode_KeyA);
    stroke('C', clipboardModifiers(), "c", PrAppKeyCode_KeyC);
    stroke('B', 0, "b", PrAppKeyCode_KeyB);
    t.frameUntil([&] { return t.shows("a^[[97;1:3ub^[[98;1:3u"); });
}

TEST_CASE("terminal: an input method composes at the cursor, and only the text that it commits reaches the program") {
    FakeApp app;
    Term t("cat", {}, &app.app);
    prTerminalFocus(t.terminal);
    t.f.frame();
    t.f.type("ab");
    t.frameUntil([&] { return t.shows("ab"); });
    // The frame that saw the echo may have drawn before it arrived.
    t.f.frame();
    CHECK(textInputColumn(t) == doctest::Approx(2));
    PrColor8 foreground = prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Foreground);
    CHECK_FALSE(cellHasColor(t, 5, 0, foreground));

    // Three wide characters cover six cells from the cursor, underlined, and the screen holds none of them. The keys
    // that the input method takes, Enter among them, never reach the program.
    t.f.push(PrAppEvent{.kind = PrAppEventKind_KeyDown, .composing = true, .key = 'N', .text = "n"});
    compose(t, "\xe3\x81\xab\xe3\x81\xbb\xe3\x82\x93");
    t.f.frame();
    CHECK(cellHasColor(t, 5, 0, foreground));
    CHECK_FALSE(cellHasColor(t, 9, 0, foreground));
    CHECK_FALSE(t.shows("\xe3"));
    t.f.push(PrAppEvent{.kind = PrAppEventKind_KeyDown, .composing = true, .key = PrAppKey_Enter});
    t.f.push(PrAppEvent{.kind = PrAppEventKind_CompositionEnd, .text = "\xe6\x97\xa5\xe6\x9c\xac"});
    t.f.type("\xe6\x97\xa5\xe6\x9c\xac");
    t.f.push(PrAppEvent{.kind = PrAppEventKind_KeyUp, .key = PrAppKey_Enter});
    t.f.key(PrAppKey_Enter);
    t.frameUntil([&] { return t.shows("ab\xe6\x97\xa5\xe6\x9c\xac\nab\xe6\x97\xa5\xe6\x9c\xac\n"); });

    // A canceled composition leaves nothing behind.
    compose(t, "xyz");
    t.f.frame();
    CHECK(cellHasColor(t, 1, 2, foreground));
    t.f.push(PrAppEvent{.kind = PrAppEventKind_CompositionEnd, .text = ""});
    t.f.frame();
    CHECK_FALSE(cellHasColor(t, 1, 2, foreground));
}

TEST_CASE("terminal: a composition that would pass the end of the row moves left to fit, and ends with focus") {
    const PrStringView arguments[] = {"10"};
    Term t("sleep", arguments);
    prTerminalFocus(t.terminal);
    t.f.frame();
    uint32_t columns = prVtermExtent(Locked(t.terminal)).x;
    t.write(format("\x1b[1;{}H", columns - 1));
    t.f.frame();
    CHECK(textInputColumn(t) == doctest::Approx(float(columns - 2)));
    compose(t, "abcd");
    t.f.frame();
    CHECK(textInputColumn(t) == doctest::Approx(float(columns - 4)));
    PrColor8 foreground = prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Foreground);
    CHECK(cellHasColor(t, columns - 4, 0, foreground));

    // Without focus, nothing would commit the composition, so it ends.
    prUiSetFocus(t.f.ui, kPrLayoutNodeNone);
    t.f.frame();
    t.f.frame();
    CHECK_FALSE(prUiOutput(t.f.ui).hasTextInput);
    CHECK_FALSE(cellHasColor(t, columns - 4, 0, foreground));
}

TEST_CASE("terminal: a terminal that has no title is named for its program, without its directory") {
#if defined(_WIN32)
    // A pseudoconsole sets the title shortly after the launch, so the check comes first.
    PrRawString shell = prEnvironmentVariable("COMSPEC");
    String program = String::adopt(shell);
    REQUIRE(prStringFindByte(program, '\\') != program.count());
    const PrStringView arguments[] = {"/c", "set /p line="};
    Term t(program, arguments);
    size_t name = prStringFindLastByte(program, '\\') + 1;
    CHECK(prTerminalTitle(t.terminal) == prStringFrom(program, name));
#else
    Term t("/bin/cat", {});
    CHECK(prTerminalTitle(t.terminal) == "cat");
#endif
}

TEST_CASE("terminal: a terminal of no program runs the user's shell") {
    Term t("", {});
    t.focus();
    t.f.frame();
    // The echoed command does not contain the number that it computes. On Windows, the shell is cmd.
#if defined(_WIN32)
    t.f.type("set /a 6*7");
    t.f.key(PrAppKey_Enter);
    t.frameUntil([&] { return t.shows("\n42\n"); });
#else
    t.f.type("echo shell-$((6 * 7))");
    t.f.key(PrAppKey_Enter);
    t.frameUntil([&] { return t.shows("shell-42"); });
#endif
}

#if defined(__APPLE__)
TEST_CASE("terminal: Option with an arrow moves by a word, and Command with an arrow to the line's ends, as on a Mac") {
    // cat -v shows each control character that it reads as ^ and a letter, and an escape as ^[. The keys are sent only
    // once the terminal is raw, because a terminal that edits lines handles ^U and ^? itself.
    const PrStringView arguments[] = {"-c", "stty raw -echo; echo ready; cat -v"};
    Term t("sh", arguments);
    t.focus();
    t.frameUntil([&] { return t.shows("ready"); });
    t.f.key(PrAppKey_Left, PrAppModifierFlag_Alt);
    t.f.key(PrAppKey_Right, PrAppModifierFlag_Alt);
    t.f.key(PrAppKey_Left, PrAppModifierFlag_Super);
    t.f.key(PrAppKey_Right, PrAppModifierFlag_Super);
    t.f.key(PrAppKey_Backspace, PrAppModifierFlag_Super);
    t.f.key(PrAppKey_Backspace, PrAppModifierFlag_Alt);
    t.frameUntil([&] { return t.shows("^[b^[f^A^E^U^[^?"); });
}
#endif

TEST_CASE("terminal: Control with an arrow, Backspace or Delete, and Home and End, go as xterm encodes them") {
    // The keys are sent only once the terminal is raw, because a terminal that edits lines handles ^U and ^? itself.
    const PrStringView arguments[] = {"-c", "stty raw -echo; echo ready; cat -v"};
    Term t("sh", arguments);
    t.focus();
    t.frameUntil([&] { return t.shows("ready"); });
    t.f.key(PrAppKey_Left, PrAppModifierFlag_Control);
    t.f.key(PrAppKey_Right, PrAppModifierFlag_Control);
    t.f.key(PrAppKey_Backspace, PrAppModifierFlag_Control);
    t.f.key(PrAppKey_Delete, PrAppModifierFlag_Control);
    t.f.key(PrAppKey_Home);
    t.f.key(PrAppKey_End);
    t.frameUntil([&] { return t.shows("^[[1;5D^[[1;5C^H^[[3;5~^[[H^[[F"); });
}

TEST_CASE("terminal: a terminal that does not show parses all of its program's output, and keeps its extent") {
    FakeApp app;
    FakeLauncher fake;
    Term t("program", {}, &app.app, {}, fake.setup());
    REQUIRE(fake.receive != nullptr);
    PrUVec2 extent = prVtermExtent(Locked(t.terminal));
    t.f.build = [&](PrLayoutNode) { prTerminalUpdate(t.terminal); };
    t.f.frame();
    CHECK(prTerminalNode(t.terminal) == kPrLayoutNodeNone);
    // Many slices of output while the terminal does not show, followed by a marker.
    String chunk = String().append(4 * kPrTerminalParseSliceBytes, 'a');
    for (uint32_t i = 0; i < 16; ++i) {
        fake.output(chunk);
        runWoken(t, app);
    }
    fake.output("\r\nhidden-done\r\n");
    runWoken(t, app);
    CHECK(t.shows("hidden-done"));
    PrUVec2 kept = prVtermExtent(Locked(t.terminal));
    CHECK(kept.x == extent.x);
    CHECK(kept.y == extent.y);
}

TEST_CASE("terminal: a terminal that stops showing loses focus, which the program hears if it asked to") {
    const PrStringView arguments[] = {"-c", "stty raw -echo; printf '\\033[?1004h'; echo ready; cat -v"};
    Term t("sh", arguments);
    t.focus();
    t.frameUntil([&] { return t.shows("ready"); });
    t.f.build = [&](PrLayoutNode) { prTerminalUpdate(t.terminal); };
    t.f.frame();
    CHECK_FALSE(prTerminalHasFocus(t.terminal));
    t.frameUntil([&] { return t.shows("^[[O"); });
}

TEST_CASE("terminal: the working directory is the file URL of this machine that the program reports, decoded") {
    const PrStringView arguments[] = {"10"};
    Term t("sleep", arguments, nullptr, "/");
    t.settle();
    // Without a report, the directory is the one that the program started in.
    CHECK(t.directory() == "/");
    t.write("\x1b]7;file:///tmp/a%20b\x07");
    CHECK(t.directory() == "/tmp/a b");
    t.write("\x1b]7;file://localhost/usr/lib\x1b\\");
    CHECK(t.directory() == "/usr/lib");
    PrRawString host = prEnvironmentHostName();
    t.write(concat("\x1b]7;file://", prRawStringView(&host), "/var\x07"));
    prRawStringReset(&host);
    CHECK(t.directory() == "/var");
    // A report with another host, another scheme, or a path that contains a NUL is ignored.
    t.write("\x1b]7;file://elsewhere.invalid/srv\x07");
    CHECK(t.directory() == "/");
    t.write("\x1b]7;https://example.com/www\x07");
    CHECK(t.directory() == "/");
    t.write("\x1b]7;file:///tmp/a%00b\x07");
    CHECK(t.directory() == "/");
}

#if defined(_WIN32)
TEST_CASE("terminal: without a report, the working directory is the one that the program started in") {
    // A pseudoconsole does not report its foreground program.
    String folder = folderWithFile("started.txt");
    const PrStringView arguments[] = {"/c", "cd .. & set /p line="};
    Term t("cmd", arguments, nullptr, folder);
    prThreadSleep(200);
    t.f.frame();
    CHECK(t.directory() == folder);
}
#else
TEST_CASE("terminal: without a report, the working directory is the foreground program's") {
    const PrStringView arguments[] = {"-c", "cd /tmp && pwd -P && exec sleep 10"};
    Term t("sh", arguments, nullptr, "/");
    t.frameUntil([&] { return prStringFindByte(t.screen(), '\n') > 1; });
    String screen = t.screen();
    String expected(prStringHead(screen, prStringFindByte(screen, '\n')));
    t.frameUntil([&] { return t.directory() == expected; });
}
#endif

TEST_CASE("terminal: a new font remakes the cells, and the program hears of the new columns and rows") {
    // The program reports its size when it reads a line. The frame that takes the new font resizes the program's
    // terminal before the line is typed, so the report has the new size. A trap on SIGWINCH would depend on when the
    // signal arrives, which under a Windows pseudoconsole is late or never.
    const PrStringView arguments[] = {"-c", "echo ready; read line; stty size; sleep 10"};
    Term t("sh", arguments);
    t.focus();
    t.frameUntil([&] { return t.shows("ready"); });
    PrVec2 cell = prTerminalCellExtent(t.terminal);
    PrUVec2 extent = prVtermExtent(Locked(t.terminal));
    PrTextFontDesc larger = t.font(26);
    prTerminalSetFont(t.terminal, &larger);
    t.f.frame();
    PrVec2 grown = prTerminalCellExtent(t.terminal);
    CHECK(grown.x > cell.x * 1.5f);
    CHECK(grown.y > cell.y * 1.5f);
    PrUVec2 fewer = prVtermExtent(Locked(t.terminal));
    CHECK(fewer.x < extent.x);
    CHECK(fewer.y < extent.y);
    t.f.key(PrAppKey_Enter);
    t.frameUntil([&] { return t.shows(format("{} {}", fewer.y, fewer.x)); });
}

TEST_CASE("terminal: arcs, braille patterns and Powerline separators are drawn as shapes within their cells") {
    TerminalDrawer drawer{};
    drawer.pixelRatio = 2;
    drawer.metrics.underlineThickness = 1;
    PrRect rect{10, 20, 18, 36};
    PrColor color{1, 1, 1, 1};
    PrDrawList* list = prDrawListCreate();
    // Returns true if the list draws within the cell. The check allows one pixel past each edge, because the list's
    // bounds include the pixel that antialiasing may touch past a shape's edge.
    auto inCell = [&]() {
        PrRect bounds = prDrawListBounds(list);
        return bounds.left >= rect.left - 1 && bounds.top >= rect.top - 1 && bounds.right <= rect.right + 1 &&
            bounds.bottom <= rect.bottom + 1;
    };
    // The blank braille pattern draws nothing, and the full pattern draws its eight dots.
    CHECK(terminalDrawShape(&drawer, list, 0x2800, rect, color));
    CHECK(prDrawListCommandCount(list) == 0);
    CHECK(terminalDrawShape(&drawer, list, 0x28ff, rect, color));
    CHECK(prDrawListCommandCount(list) == 8);
    CHECK(inCell());
    // Each arc and separator stays in its cell. The cell clips the joins and caps of a separator's strokes.
    const uint32_t shapes[] = {
        0x256d, 0x256e, 0x256f, 0x2570, 0xe0b0, 0xe0b1, 0xe0b2, 0xe0b3, 0xe0b4, 0xe0b5, 0xe0b6,
        0xe0b7, 0xe0b8, 0xe0b9, 0xe0ba, 0xe0bb, 0xe0bc, 0xe0bd, 0xe0be, 0xe0bf,
    };
    for (uint32_t codePoint : shapes) {
        CAPTURE(codePoint);
        prDrawListClear(list);
        CHECK(terminalDrawShape(&drawer, list, codePoint, rect, color));
        CHECK(prDrawListCommandCount(list) >= 2);
        CHECK(inCell());
    }
    // Other private-use characters, and letters, are left to the font.
    CHECK_FALSE(terminalDrawShape(&drawer, list, 0xe0c0, rect, color));
    CHECK_FALSE(terminalDrawShape(&drawer, list, 'A', rect, color));
    prDrawListRelease(list);
}

TEST_CASE("terminal: the icons of Nerd Fonts draw within their cells, centered in the line") {
    const PrStringView arguments[] = {"10"};
    Term t("sleep", arguments);
    t.settle();
    // A file icon from the Private Use Area and a Material Design icon from the Supplementary Private Use Area, each
    // followed by a blank cell.
    t.write("\xef\x85\x9b \xf3\xb0\x80\x81 ");
    t.f.frame();
    PrVec2 cell = prTerminalCellExtent(t.terminal);
    CellInk icons[2];
    for (uint32_t i = 0; i < 2; ++i) {
        uint32_t column = i * 2;
        CAPTURE(column);
        icons[i] = cellInk(t, column, 0);
        CHECK(float(icons[i].count) > cell.x * cell.y / 4);
        float middle = (icons[i].bounds.top + icons[i].bounds.bottom) / 2;
        CHECK(prF32Abs(middle - t.cellCenter(column, 0).y) < cell.y / 8);
        CHECK(cellInk(t, column + 1, 0).count == 0);
    }
    // The two icons differ, so neither is the missing glyph that a face without them would draw for both.
    CHECK(icons[0].inked != icons[1].inked);
}

TEST_CASE("terminal: with the terminal app's fonts, what Noto Sans Mono lacks draws in the system's fonts") {
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    if (system == nullptr) {
        MESSAGE("the build has no system's fonts");
        return;
    }
    {
        const PrStringView arguments[] = {"10"};
        Term t("sleep", arguments, nullptr, {}, {},
            KitFonts{.bundles = {PrFontBundle_NotoMono, PrFontBundle_NerdFontSymbols}, .bundleCount = 2, .system = system});
        t.settle();
        // A Han ideograph and an emoji, each two columns wide, and a Nerd Fonts file icon. Each is followed by a blank
        // cell.
        t.write("\xe6\xbc\xa2 \xf0\x9f\x98\x80 \xef\x85\x9b ");
        t.f.frame();
        // Each draws across its cells. How much ink it leaves depends on the system's face, which differs between
        // machines, so the bound is only that it is not blank.
        PrVec2 cell = prTerminalCellExtent(t.terminal);
        struct Glyph {
            uint32_t column;
            uint32_t width;
        };
        const Glyph glyphs[] = {{0, 2}, {3, 2}, {6, 1}};
        for (Glyph glyph : glyphs) {
            CAPTURE(glyph.column);
            uint32_t ink = 0;
            for (uint32_t i = 0; i < glyph.width; ++i)
                ink += cellInk(t, glyph.column + i, 0).count;
            CHECK(float(ink) > float(glyph.width) * cell.x * cell.y / 16);
        }
        // The ideograph and the emoji fall back to families that the system provides.
        CHECK(prFontSetFallbackCount(t.f.fonts) >= 3);
    }
    prFontSystemDestroy(system);
}

TEST_CASE("terminal: blinking text hides and shows on a steady period over its background") {
    const PrStringView arguments[] = {"10"};
    Term t("sleep", arguments);
    t.settle();
    // A full block that blinks over a red background, and one that does not.
    t.write("\x1b[5;41m\xe2\x96\x88\x1b[0m \xe2\x96\x88");
    // Phases of half a second count from time zero, and settling runs as many frames as the launch takes, so the first
    // frame moves to the middle of a phase that shows blinking text.
    t.f.frame(prF64Ceil(t.f.time) + 0.25 - t.f.time);
    PrVec2 blinking = t.cellCenter(0, 0);
    PrVec2 steady = t.cellCenter(2, 0);
    PrColor8 foreground = prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Foreground);
    PrColor8 red = prVtermPaletteColor(Locked(t.terminal), 1);
    CHECK(near(t.pixel(blinking), foreground));
    // The terminal asks for a frame at the time that the blink next changes.
    CHECK(prUiOutput(t.f.ui).frameTime > t.f.time);
    t.f.frame(0.5);
    CHECK(near(t.pixel(blinking), red));
    CHECK(near(t.pixel(steady), foreground));
    t.f.frame(0.5);
    CHECK(near(t.pixel(blinking), foreground));

    // With no blinking text on the screen, the terminal asks for no frames.
    t.write("\x1b[2J");
    t.f.frame();
    CHECK(prUiOutput(t.f.ui).frameTime == 0);
    CHECK_FALSE(prUiOutput(t.f.ui).wantsFrame);
}

TEST_CASE("terminal: blinking text stays shown for a user who asks for less motion") {
    const PrStringView arguments[] = {"10"};
    Term t("sleep", arguments);
    t.settle();
    t.f.preferences.reducedMotion = true;
    t.write("\x1b[5m\xe2\x96\x88\x1b[0m");
    PrColor8 foreground = prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Foreground);
    for (uint32_t i = 0; i < 3; ++i) {
        t.f.frame(0.5);
        CHECK(near(t.pixel(t.cellCenter(0, 0)), foreground));
        CHECK(prUiOutput(t.f.ui).frameTime == 0);
    }
}

TEST_CASE("terminal: a paste of lines asks first, and Enter pastes them while Escape drops them") {
    FakeApp app;
    Term t("cat", {}, &app.app);
    t.focus();
    t.f.frame();
    t.f.push(PrAppEvent{.kind = PrAppEventKind_Clipboard, .text = "one\ntwo\n"});
    for (uint32_t i = 0; i < 4; ++i)
        t.f.frame();
    CHECK(prUiFocus(t.f.ui) != prTerminalNode(t.terminal));
    CHECK_FALSE(t.shows("one"));
    t.f.key(PrAppKey_Enter);
#if defined(_WIN32)
    // A console echoes each line as cat reads it.
    t.frameUntil([&] { return t.shows("one\none\ntwo\ntwo\n"); });
#else
    // The terminal echoes the lines, which arrive together, and then cat writes them.
    t.frameUntil([&] { return t.shows("one\ntwo\none\ntwo\n"); });
#endif

    t.f.push(PrAppEvent{.kind = PrAppEventKind_Clipboard, .text = "three\nfour"});
    for (uint32_t i = 0; i < 4; ++i)
        t.f.frame();
    t.f.key(PrAppKey_Escape);
    // Focus returns to the screen, which receives the next typed text.
    t.frameUntil([&] { return prUiFocus(t.f.ui) == prTerminalNode(t.terminal); });
    t.f.type("x");
    t.f.key(PrAppKey_Enter);
    t.frameUntil([&] { return t.shows("x\nx\n"); });
    CHECK_FALSE(t.shows("three"));

    // A single line pastes at once, and so do several lines if the program asked for bracketed paste.
    t.f.push(PrAppEvent{.kind = PrAppEventKind_Clipboard, .text = "solo"});
    t.frameUntil([&] { return t.shows("solo"); });
    t.write("\x1b[?2004h");
    t.f.push(PrAppEvent{.kind = PrAppEventKind_Clipboard, .text = "five\nsix"});
    t.frameUntil([&] { return t.shows("five"); });
}

TEST_CASE("terminal: URLs are found in the text without the brackets and punctuation around them") {
    CHECK(textLinks("see (https://example.com/a), or not") == Array<String>({"url|https://example.com/a"}));
    CHECK(textLinks("<http://x.org/p?q=1>.") == Array<String>({"url|http://x.org/p?q=1"}));
    // A URL's path is not a file reference. Only http and https start URLs, so a bare name with a dot may name a file.
    CHECK(textLinks("https://x.com/a.txt:3") == Array<String>({"url|https://x.com/a.txt:3"}));
    CHECK(textLinks("www.example.org") == Array<String>({"www.example.org|www.example.org|0|0"}));
    CHECK(textLinks("a.cpp:1 https://b/ c.h:2") ==
        Array<String>({"a.cpp:1|a.cpp|1|0", "url|https://b/", "c.h:2|c.h|2|0"}));
}

TEST_CASE("terminal: references to files are found with the line and the column that tools print after them") {
    CHECK(textLinks("src/a.cpp:12:5: error: no") == Array<String>({"src/a.cpp:12:5|src/a.cpp|12|5"}));
    CHECK(textLinks("a.cpp:7 b") == Array<String>({"a.cpp:7|a.cpp|7|0"}));
    CHECK(textLinks("Makefile:3: *** missing") == Array<String>({"Makefile:3|Makefile|3|0"}));
    CHECK(textLinks("C:\\x\\a.cpp(12,5): error C2065") == Array<String>({"C:\\x\\a.cpp(12,5)|C:\\x\\a.cpp|12|5"}));
    CHECK(textLinks("foo.h(3): warning") == Array<String>({"foo.h(3)|foo.h|3|0"}));
    CHECK(textLinks("  File \"/tmp/x y.py\", line 42, in <module>") ==
        Array<String>({"/tmp/x y.py\", line 42|/tmp/x y.py|42|0"}));
    CHECK(textLinks("    at f (/a/b.js:10:15)") == Array<String>({"/a/b.js:10:15|/a/b.js|10|15"}));
    CHECK(textLinks("see notes.txt, then ./run") == Array<String>({"notes.txt|notes.txt|0|0", "./run|./run|0|0"}));
    CHECK(textLinks("plain words: here").isEmpty());
}

TEST_CASE("terminal: a click with the primary modifier opens a file that a reference names, from the directory") {
    String folder = folderWithFile("hello.txt");
    Array<OpenedFile> opened;
    auto setup = [&](PrTerminalDesc* desc) {
        desc->directory = PrStringView(folder);
        desc->openFile = [](void* context, const PrTerminalFileLink* link) {
            static_cast<Array<OpenedFile>*>(context)->push(OpenedFile{String(link->path), link->line, link->column});
        };
        desc->openFileContext = &opened;
    };
    const PrStringView arguments[] = {"-c", "printf 'hello.txt:3:2: note\\nmissing.txt:1:1: note\\n'; sleep 10"};
    Term t("sh", arguments, nullptr, {}, setup);
    t.frameUntil([&] { return t.shows("missing.txt"); });
    t.move(t.cellCenter(2, 0), primaryModifier());
    t.f.frame();
    CHECK(prUiOutput(t.f.ui).cursor == PrAppCursor_Hand);
    t.move(t.cellCenter(2, 1), primaryModifier());
    t.f.frame();
    CHECK(prUiOutput(t.f.ui).cursor == PrAppCursor_Text);
    // The note after the reference is not part of the link.
    t.move(t.cellCenter(17, 0), primaryModifier());
    t.f.frame();
    CHECK(prUiOutput(t.f.ui).cursor == PrAppCursor_Text);

    t.f.click(t.cellCenter(2, 0), primaryModifier());
    t.f.frame();
    REQUIRE(opened.count() == 1);
    CHECK(opened[0] == OpenedFile{concat(folder, "/hello.txt"), 3, 2});

    // A directory that the program reports replaces the one that it started in.
    t.write("\x1b]7;file:///nowhere/at/all\x07");
    t.f.frame();
    t.move(t.cellCenter(2, 0), primaryModifier());
    t.f.frame();
    CHECK(prUiOutput(t.f.ui).cursor == PrAppCursor_Text);
}

TEST_CASE("terminal: a reference to a file opens as a file URL without a handler") {
    String folder = folderWithFile("hello.txt");
    FakeApp app;
    auto setup = [&](PrTerminalDesc* desc) { desc->directory = PrStringView(folder); };
    const PrStringView arguments[] = {"-c", "printf 'hello.txt:3\\n'; sleep 10"};
    Term t("sh", arguments, &app.app, {}, setup);
    t.frameUntil([&] { return t.shows("hello.txt"); });
    t.f.click(t.cellCenter(2, 0), primaryModifier());
    t.f.frame();
    CHECK(app.urls == Array<String>({taken(prUrlFromFilePath(concat(folder, "/hello.txt")))}));
}

TEST_CASE("terminal: the shell integration writes its scripts, and starts each shell that it knows to read them") {
    String directory = subfolder("integration");
    TerminalShellLaunch launch{};
    CHECK_FALSE(terminalShellIntegrate("/bin/sh", directory, nullptr, 0, &launch));
    CHECK(launch.arguments.count == 0);

    const PrStringView zdotdir[] = {"ZDOTDIR=/home/me/zsh"};
    REQUIRE(terminalShellIntegrate("/usr/local/bin/zsh", directory, zdotdir, 1, &launch));
    CHECK(strings(launch.arguments) == Array<String>({"-l"}));
    CHECK(strings(launch.environment) ==
        Array<String>({concat("ZDOTDIR=", directory, "/zsh"), "PRAX_ZSH_ZDOTDIR=/home/me/zsh"}));
    CHECK(prStringContains(readText(concat(directory, "/zsh/.zshenv")), "PRAX_ZSH_ZDOTDIR"));
    terminalShellLaunchReset(&launch);

    REQUIRE(terminalShellIntegrate("bash", directory, nullptr, 0, &launch));
    CHECK(strings(launch.arguments) == Array<String>({"--rcfile", concat(directory, "/bash/prax.bash")}));
    CHECK(strings(launch.environment) == Array<String>({"PRAX_BASH_LOGIN=1"}));
    terminalShellLaunchReset(&launch);
    const PrStringView posix[] = {"POSIXLY_CORRECT=1"};
    CHECK_FALSE(terminalShellIntegrate("bash", directory, posix, 1, &launch));

    // fish reads the integration's data directory and then the default data directories. If the environment sets
    // XDG_DATA_DIRS, its directories replace the defaults.
    const PrStringView unset[] = {"XDG_DATA_DIRS="};
    REQUIRE(terminalShellIntegrate("/opt/bin/fish", directory, unset, 1, &launch));
    CHECK(strings(launch.environment) ==
        Array<String>({concat("XDG_DATA_DIRS=", directory, ":/usr/local/share:/usr/share")}));
    terminalShellLaunchReset(&launch);
    const PrStringView data[] = {"XDG_DATA_DIRS=/opt/share"};
    REQUIRE(terminalShellIntegrate("fish", directory, data, 1, &launch));
    CHECK(strings(launch.environment) ==
        Array<String>({concat("XDG_DATA_DIRS=", directory, ":/opt/share"), "PRAX_FISH_XDG_DATA_DIRS=/opt/share"}));
    terminalShellLaunchReset(&launch);
    PrFileInfo info{};
    CHECK(prFileInfo(concat(directory, "/fish/vendor_conf.d/prax-shell-integration.fish"), &info));

    // A script that changed is written again.
    writeFile(concat(directory, "/zsh/prax.zsh"), "changed");
    REQUIRE(terminalShellIntegrate("zsh", directory, nullptr, 0, &launch));
    terminalShellLaunchReset(&launch);
    CHECK(readText(concat(directory, "/zsh/prax.zsh")) != "changed");
}

TEST_CASE("terminal: the user's shell starts with the integration, after its own startup files, and marks commands") {
    Array<String> shells = integratedShells();
    if (shells.isEmpty())
        MESSAGE("no shell that the terminal has an integration for");
    for (const String& shell : shells) {
        CAPTURE(shell);
        String home = subfolder(concat("home-", nameOf(shell)));
        writeFile(concat(home, "/.zshrc"), "export PRAX_TEST_RC=read\nPS1='ready$ '\n");
        writeFile(concat(home, "/.bash_profile"), "export PRAX_TEST_RC=read\nPS1='ready$ '\n");
        // The shell reads its startup files from the test home. For zsh, ZDOTDIR names that home, and it still does
        // inside the shell.
        String shellVariable = concat("SHELL=", shell);
        String homeVariable = concat("HOME=", home);
        String zdotdirVariable = concat("ZDOTDIR=", home);
        PrStringView environment[] = {shellVariable, homeVariable, zdotdirVariable, "BASH_SILENCE_DEPRECATION_WARNING=1"};
        auto setup = [&](PrTerminalDesc* desc) {
            desc->environment = environment;
            desc->environmentCount = uint32_t(countOf(environment));
        };
        Term t("", {}, nullptr, {}, setup);
        t.focus();
        auto input = [&] { return prTerminalShellInfo(t.terminal).state == PrVtermShellState_Input; };
        t.frameUntil(input);
        t.f.type("echo rc-$PRAX_TEST_RC zdotdir-${ZDOTDIR#$HOME}");
        t.f.key(PrAppKey_Enter);
        t.frameUntil([&] { return t.shows("rc-read zdotdir-\n") && input(); });
        CHECK(prTerminalShellInfo(t.terminal).lastStatus == PrVtermCommandStatus_Succeeded);
        CHECK(taken(prTerminalCommandOutput(t.terminal)) == "rc-read zdotdir-");

        t.f.type("(exit 3)");
        t.f.key(PrAppKey_Enter);
        t.frameUntil([&] { return prTerminalShellInfo(t.terminal).lastStatus == PrVtermCommandStatus_Failed; });
        CHECK(prTerminalShellInfo(t.terminal).lastExitCode == 3);
        CHECK(prStringStartsWith(prVtermWorkingDirectory(Locked(t.terminal)), "file://"));

        // The prompts of the two commands, and the prompt that follows them.
        t.frameUntil(input);
        Locked vterm(t.terminal);
        Array<PrVtermCommandStatus> statuses;
        for (uint32_t i = 0; i < prVtermScrollbackCount(vterm) + prVtermExtent(vterm).y; ++i) {
            PrVtermLine line = prVtermLine(vterm, i);
            if ((line.marks & PrVtermLineMarkFlag_Prompt) != 0)
                statuses.push(line.commandStatus);
        }
        CHECK(statuses == Array<PrVtermCommandStatus>({
            PrVtermCommandStatus_Succeeded, PrVtermCommandStatus_Failed, PrVtermCommandStatus_None,
        }));
    }
}

TEST_CASE("terminal: a terminal whose caller turns the integration off runs the user's shell without it") {
    Array<String> shells = integratedShells();
    if (shells.isEmpty())
        MESSAGE("no shell that the terminal has an integration for");
    for (const String& shell : shells) {
        CAPTURE(shell);
        String home = subfolder(concat("home-", nameOf(shell)));
        String shellVariable = concat("SHELL=", shell);
        String homeVariable = concat("HOME=", home);
        String zdotdirVariable = concat("ZDOTDIR=", home);
        PrStringView environment[] = {shellVariable, homeVariable, zdotdirVariable, "BASH_SILENCE_DEPRECATION_WARNING=1"};
        auto setup = [&](PrTerminalDesc* desc) {
            desc->environment = environment;
            desc->environmentCount = uint32_t(countOf(environment));
            desc->shellIntegration = PrTerminalShellIntegration_Off;
        };
        Term t("", {}, nullptr, {}, setup);
        t.focus();
        t.f.frame();
        t.f.type("echo off-$((1 + 2))");
        t.f.key(PrAppKey_Enter);
        t.frameUntil([&] { return t.shows("off-3"); });
        CHECK(prTerminalShellInfo(t.terminal).state == PrVtermShellState_None);
    }
}

TEST_CASE("terminal: the last command's output and exit code are the application's to read and copy") {
    FakeApp app;
    const PrStringView arguments[] = {"10"};
    Term t("sleep", arguments, &app.app);
    t.settle();
    CHECK(taken(prTerminalCommandOutput(t.terminal)).isEmpty());
    const PrStringView one[] = {"a"};
    const PrStringView two[] = {"b", "c  ", ""};
    t.write(concat(command("one", one, "0"), command("two", two, "4"), kPrompt));
    t.f.frame();
    CHECK(taken(prTerminalCommandOutput(t.terminal)) == "b\nc");
    PrVtermShellInfo info = prTerminalShellInfo(t.terminal);
    CHECK(info.lastStatus == PrVtermCommandStatus_Failed);
    CHECK(info.lastExitCode == 4);
    prTerminalCopyCommandOutput(t.terminal);
    CHECK(app.clipboard == "b\nc");

    // A command that is still running is not the last command that finished.
    t.write("three\r\n\x1b]133;C\x07" "d\r\n");
    t.f.frame();
    CHECK(taken(prTerminalCommandOutput(t.terminal)) == "b\nc");
}

TEST_CASE("terminal: the prompt keys scroll from prompt to prompt, and the jumped-to command's output selects") {
    const PrStringView arguments[] = {"10"};
    Term t("sleep", arguments);
    t.settle();
    uint32_t rows = prVtermExtent(Locked(t.terminal)).y;
    Array<String> first = numbered("f", rows + 4);
    Array<String> second = numbered("s", rows + 4);
    Array<PrStringView> firstLines = viewsOf(first);
    Array<PrStringView> secondLines = viewsOf(second);
    t.write(concat(command("first", firstLines.span(), "0"), command("second", secondLines.span(), "1"), kPrompt));
    t.focus();
    t.f.frame();

    t.f.key(PrAppKey_Up, promptModifiers());
    t.f.frame();
    REQUIRE(prTerminalSelectCommandOutput(t.terminal));
    CHECK(t.selection() == joined(second));
    // The view's top line is the prompt. A click in the gutter beside a line selects the output of that line's command.
    t.f.click(gutterAt(t, 0));
    t.f.frame();
    CHECK(t.selection() == joined(second));
    t.f.click(gutterAt(t, 3));
    t.f.frame();
    CHECK(t.selection() == joined(second));

    t.f.key(PrAppKey_Up, promptModifiers());
    t.f.frame();
    REQUIRE(prTerminalSelectCommandOutput(t.terminal));
    CHECK(t.selection() == joined(first));
    CHECK_FALSE(prTerminalScrollToPrompt(t.terminal, PrTerminalDirection_Previous));

    t.f.key(PrAppKey_Down, promptModifiers());
    t.f.frame();
    REQUIRE(prTerminalSelectCommandOutput(t.terminal));
    CHECK(t.selection() == joined(second));
    // The current prompt has no output. Past it, the view returns to the screen.
    CHECK(prTerminalScrollToPrompt(t.terminal, PrTerminalDirection_Next));
    CHECK_FALSE(prTerminalSelectCommandOutput(t.terminal));
    CHECK(prTerminalScrollToPrompt(t.terminal, PrTerminalDirection_Next));
    CHECK_FALSE(prTerminalScrollToPrompt(t.terminal, PrTerminalDirection_Next));
    // If the view has not scrolled to a prompt, the output of the last command that finished is selected.
    REQUIRE(prTerminalSelectCommandOutput(t.terminal));
    CHECK(t.selection() == joined(second));
}

TEST_CASE("terminal: the gutter marks a command that succeeded in the theme's success color, and one that failed in "
        "its danger color") {
    const PrStringView arguments[] = {"10"};
    Term t("sleep", arguments);
    t.settle();
    t.write(concat(command("ok", {}, "0"), command("bad", {}, "2"), kPrompt));
    t.f.frame();
    const PrKitPalette& palette = t.f.kit->theme.palette;
    CHECK(prTerminalScreenOrigin(t.terminal).x > 0);
    CHECK(near(t.pixel(gutterAt(t, 0)), color8Of(palette.success)));
    CHECK(near(t.pixel(gutterAt(t, 1)), color8Of(palette.danger)));
    CHECK(near(t.pixel(gutterAt(t, 2)), prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Background)));
}

TEST_CASE("terminal: the cell's pixel extent is the cell's in target pixels, which the screen reports to programs") {
    const PrStringView arguments[] = {"10"};
    Term t("sleep", arguments);
    t.settle();
    PrVec2 cell = prTerminalCellExtent(t.terminal);
    PrUVec2 pixels = cellPixelExtent(t.terminal);
    CHECK(pixels.x == uint32_t(prF32Round(cell.x)));
    CHECK(pixels.y == uint32_t(prF32Round(cell.y)));
}

TEST_CASE("terminal: images draw over the text, between the backgrounds and the text, or under the backgrounds") {
    const PrStringView arguments[] = {"10"};
    Term t("sleep", arguments);
    t.settle();
    PrColor8 blue = prVtermPaletteColor(Locked(t.terminal), 4);
    PrColor8 foreground = prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Foreground);
    // Row 0: three cells with a blue background, each with an image in a different z-index band. Row 1: two full blocks
    // in the foreground color, with an image under the first and over the second.
    t.write("\x1b[44m   \x1b[0m\r\n\xe2\x96\x88\xe2\x96\x88\x1b[H");
    String red = cellPixels(t.terminal, 1, 1, kImageRed);
    t.write(concat(graphics(cellImage(t.terminal, 1, 1, ",z=1"), red), "\x1b[C"));
    t.write(concat(graphics(cellImage(t.terminal, 1, 1, ",z=-1"), red), "\x1b[C"));
    t.write(graphics(cellImage(t.terminal, 1, 1, ",z=-1073741825"), red));
    t.write(concat("\x1b[2;1H", graphics(cellImage(t.terminal, 1, 1, ",z=-1"), red), "\x1b[C"));
    t.write(graphics(cellImage(t.terminal, 1, 1, ",z=0"), red));
    t.f.frame();
    CHECK(near(t.pixel(t.cellCenter(0, 0)), kImageRed));
    CHECK(near(t.pixel(t.cellCenter(1, 0)), kImageRed));
    CHECK(near(t.pixel(t.cellCenter(2, 0)), blue));
    CHECK(near(t.pixel(t.cellCenter(0, 1)), foreground));
    CHECK(near(t.pixel(t.cellCenter(1, 1)), kImageRed));
    // After a delete, the cells show again, although the drawings of their rows did not change.
    t.write(graphics("a=d"));
    t.f.frame();
    CHECK(near(t.pixel(t.cellCenter(0, 0)), blue));
    CHECK(near(t.pixel(t.cellCenter(1, 1)), foreground));
}

TEST_CASE("terminal: an image scrolls with its text, into the scrollback that the view scrolls back to") {
    const PrStringView arguments[] = {"10"};
    Term t("sleep", arguments);
    t.settle();
    PrColor8 background = prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Background);
    t.write(concat("\x1b[3;5H", graphics(cellImage(t.terminal, 2, 2, ""), cellPixels(t.terminal, 2, 2, kImageRed))));
    t.f.frame();
    CHECK(near(t.pixel(t.cellCenter(5, 2)), kImageRed));
    CHECK(near(t.pixel(t.cellCenter(5, 4)), background));
    t.write("\x1b[1S");
    t.f.frame();
    CHECK(near(t.pixel(t.cellCenter(5, 1)), kImageRed));
    CHECK(near(t.pixel(t.cellCenter(5, 3)), background));
    // Three more lines move the image off the screen into the scrollback, where scrolling the view back finds it.
    t.write("\x1b[3S");
    t.f.frame();
    CHECK(near(t.pixel(t.cellCenter(5, 0)), background));
    t.f.move(t.cellCenter(10, 5));
    t.f.frame();
    t.f.push(PrAppEvent{
        .kind = PrAppEventKind_Scroll,
        .scrollUnit = PrAppScrollUnit_Line,
        .position = t.cellCenter(10, 5),
        .delta = PrVec2{0, -3},
    });
    t.f.frame();
    CHECK(near(t.pixel(t.cellCenter(5, 1)), kImageRed));
}

TEST_CASE("terminal: the placeholders of a virtual placement draw its image in their cells") {
    const PrStringView arguments[] = {"10"};
    Term t("sleep", arguments);
    t.settle();
    PrColor8 background = prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Background);
    t.write(graphics(cellImage(t.terminal, 2, 1, ",U=1,i=7,c=2,r=1,q=2"), cellPixels(t.terminal, 2, 1, kImageRed)));
    t.f.frame();
    CHECK(near(t.pixel(t.cellCenter(0, 0)), background));
    // Two placeholders of image 7, at row 0 and columns 0 and 1. The second inherits its row and column.
    t.write("\x1b[38;5;7m\xf4\x8e\xbb\xae\xcc\x85\xcc\x85\xf4\x8e\xbb\xae\x1b[39m");
    t.f.frame();
    CHECK(near(t.pixel(t.cellCenter(0, 0)), kImageRed));
    CHECK(near(t.pixel(t.cellCenter(1, 0)), kImageRed));
    CHECK(near(t.pixel(t.cellCenter(2, 0)), background));
}

// A Windows pseudoconsole drops the graphics protocol's sequences.
#if !defined(_WIN32)
TEST_CASE("terminal: a program shows an image with the graphics protocol") {
    // One red pixel, encoded in base64 in the escape code, shown over two cells.
    const PrStringView arguments[] = {
        "-c", "printf '\\033_Ga=T,f=24,s=1,v=1,c=2,r=1,i=5,q=2;/wAA\\033\\\\done'; sleep 5",
    };
    Term t("sh", arguments);
    t.frameUntil([&] { return t.shows("done"); });
    t.f.frame();
    CHECK(near(t.pixel(t.cellCenter(0, 0)), kImageRed));
}
#endif
