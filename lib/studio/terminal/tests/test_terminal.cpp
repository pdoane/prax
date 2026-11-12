// Tests the terminal frame by frame, driven as an application drives it, with programs that the tests run.

#include "pr/atomic.h"
#include "pr/base64.h"
#include "pr/count_of.h"
#include "pr/draw_commands.h"
#include "pr/draw_list.h"
#include "pr/environment.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/kit_theme.h"
#include "pr/layout_frame.h"
#include "pr/min_max.h"
#include "pr/process.h"
#include "pr/raw_array.h"
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

#include <functional>
#include <initializer_list>
#include <string>
#include <vector>

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
    std::vector<std::string> urls;
    std::string clipboard;
    bool clipboardRequested = false;
    bool active = true;
    std::vector<std::string> posted;
    PrStatus postStatus = PrStatus_Ok;
    uint32_t attention = 0;

    FakeApp() {
        app = PrApp{};
        app.wake = [](PrApp* a) { reinterpret_cast<FakeApp*>(a)->wakes.fetchAdd(1); };
        app.isActive = [](PrApp* a) { return reinterpret_cast<FakeApp*>(a)->active; };
        app.requestAttention = [](PrApp* a) { ++reinterpret_cast<FakeApp*>(a)->attention; };
        app.notificationPost = [](PrApp* a, const PrAppNotificationDesc* desc) -> PrStatus {
            FakeApp* fake = reinterpret_cast<FakeApp*>(a);
            fake->posted.push_back(std::string(desc->title.data, desc->title.size) + "|" +
                std::string(desc->body.data, desc->body.size) + "|" + std::to_string(desc->tag));
            return fake->postStatus;
        };
        app.openUrl = [](PrApp* a, PrStringView url) -> PrStatus {
            reinterpret_cast<FakeApp*>(a)->urls.emplace_back(url.data, url.size);
            return PrStatus_Ok;
        };
        app.setClipboardText = [](PrApp* a, PrStringView text) -> PrStatus {
            reinterpret_cast<FakeApp*>(a)->clipboard.assign(text.data, text.size);
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
// memory, with no pseudoterminal or program between. The process runs until the terminal destroys it, and takes and
// drops what the terminal writes to it. A test that calls `output` from a thread of its own joins the thread before the
// terminal is destroyed, as a backend stops its thread when its process is destroyed.
struct FakeLauncher {
    PrProcessLauncher launcher{};
    PrProcess process{};
    PrProcessReceiveFn PR_NULLABLE receive = nullptr;
    void* PR_NULLABLE receiveContext = nullptr;

    FakeLauncher() {
        launcher.destroy = [](PrProcessLauncher*) {};
        launcher.launch = [](PrProcessLauncher* l, const PrProcessDesc* desc, PrProcess* PR_NULLABLE* out) -> PrStatus {
            FakeLauncher* fake = reinterpret_cast<FakeLauncher*>(l);
            fake->receive = desc->receive;
            fake->receiveContext = desc->receiveContext;
            *out = &fake->process;
            return PrStatus_Ok;
        };
        process.destroy = [](PrProcess*) {};
        process.write = [](PrProcess*, PrByteView) {};
        process.closeInput = [](PrProcess*) {};
        process.read = [](PrProcess*, PrProcessStream, uint8_t* PR_NULLABLE, size_t) { return size_t(0); };
        process.info = [](const PrProcess*) { return PrProcessInfo{}; };
        process.kill = [](PrProcess*) {};
        process.terminalResize = [](PrProcess*, PrUVec2, PrUVec2) {};
        process.terminalForegroundDirectory = [](const PrProcess*) { return prRawStringNull(); };
    }

    FakeLauncher(const FakeLauncher&) = delete;
    FakeLauncher& operator=(const FakeLauncher&) = delete;

    // Gives `text` to the terminal as the program's output, on the calling thread, as a backend's thread gives it. The
    // terminal must have launched its program through this launcher.
    void output(const std::string& text) {
        receive(receiveContext, &process, PrProcessStream_Output,
            PrByteView{reinterpret_cast<const uint8_t*>(text.data()), text.size()});
    }

    // Sets `desc` to launch through this launcher (Term's setup).
    std::function<void(PrTerminalDesc*)> setup() {
        return [this](PrTerminalDesc* desc) { desc->launcher = &launcher; };
    }
};

// Gives `text` to a terminal from a thread of its own, as a program's output, until stopped. Stopping joins the thread.
struct Flood {
    FakeLauncher* fake;
    std::string text;
    Atomic<uint32_t> stopping{};
    PrThread thread{};

    Flood(FakeLauncher* launcher, std::string flooded) : fake(launcher), text(std::move(flooded)) {
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
    Term(PrStringView program, std::initializer_list<PrStringView> arguments, PrApp* PR_NULLABLE app = nullptr,
            PrStringView directory = {}, const std::function<void(PrTerminalDesc*)>& setup = nullptr,
            const KitFonts& fonts = KitFonts{}) : f(fonts) {
        PrTerminalDesc desc{
            .kit = f.kit,
            .font = font(13),
            .app = app,
            .launcher = launcher,
            .program = program,
            .arguments = arguments.begin(),
            .argumentCount = uint32_t(arguments.size()),
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
    std::string directory() const {
        PrRawString text = prTerminalWorkingDirectory(terminal);
        std::string out(prRawStringView(&text).data, prRawStringView(&text).size);
        prRawStringReset(&text);
        return out;
    }

    // The text of the screen's rows, each followed by a line feed, without trailing spaces.
    std::string screen() const {
        Locked vterm(terminal);
        PrUVec2 extent = prVtermExtent(vterm);
        std::string out;
        for (uint32_t row = 0; row < extent.y; ++row) {
            PrRawString text = prRawStringNull();
            uint32_t index = prVtermScrollbackCount(vterm) + row;
            prVtermLineAppendText(vterm, index, 0, extent.x, &text);
            std::string line(prRawStringView(&text).data, prRawStringView(&text).size);
            prRawStringReset(&text);
            while (!line.empty() && line.back() == ' ')
                line.pop_back();
            out += line + "\n";
        }
        return out;
    }

    // Runs frames until `done` returns true. Fails the test if that takes longer than kPatience.
    void frameUntil(const std::function<bool()>& done) {
        double end = prTimeMonotonicMilliseconds() + kPatience;
        while (!done()) {
            if (prTimeMonotonicMilliseconds() >= end)
                FAIL("the screen shows:\n" << screen());
            prThreadSleep(2);
            f.frame();
        }
    }

    // Waits until the screen has only what the test writes to it (write). A Windows pseudoconsole clears the screen and
    // sets the title shortly after the launch, which would erase anything written before it, so this runs frames until
    // the title changes from the program's name. Elsewhere the screen is ready at once. The program must write nothing,
    // as sleep does.
    void settle() {
#if defined(_WIN32)
        std::string name(prTerminalTitle(terminal).data, prTerminalTitle(terminal).size);
        frameUntil([&] { return prTerminalTitle(terminal) != PrStringView(name); });
#endif
    }

    bool shows(const std::string& text) const { return screen().find(text) != std::string::npos; }

    // The center of the screen cell at `column` and `row`, in viewport coordinates.
    PrVec2 cellCenter(uint32_t column, uint32_t row) {
        PrRect bounds = f.bounds(prTerminalNode(terminal));
        PrVec2 origin = prTerminalScreenOrigin(terminal);
        PrVec2 cell = prTerminalCellExtent(terminal);
        float x = bounds.left + origin.x + (float(column) + 0.5f) * cell.x;
        return PrVec2{x, bounds.top + origin.y + (float(row) + 0.5f) * cell.y};
    }

    std::string selection() const {
        PrRawString text = prTerminalSelection(terminal);
        std::string out(prRawStringView(&text).data, prRawStringView(&text).size);
        prRawStringReset(&text);
        return out;
    }

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
    void write(const std::string& text) {
        prVtermWrite(Locked(terminal), PrByteView{reinterpret_cast<const uint8_t*>(text.data()), text.size()});
    }

    // Renders the tree that the last frame laid out, and returns the color of the pixel at `at` in the viewport.
    // `canvas` sets the pixels before the tree paints.
    PrColor8 pixel(PrVec2 at, KitCanvas canvas = KitCanvas_Cleared) {
        std::vector<uint8_t> rgba;
        REQUIRE(f.render(&rgba, canvas));
        size_t i = (size_t(at.y) * KitFixture::kRenderWidth + size_t(at.x)) * 4;
        return PrColor8{rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
    }
};

// Returns the tests' folder under the user's cache directory, after writing a file of three lines named `name` in it.
// The path is normalized, as the terminal's paths are.
std::string folderWithFile(const std::string& name) {
    PrRawString cache = prFileUserDirectory(PrFileUserDirectory_Cache);
    PrRawString absolute = prFilePathAbsolute(prRawStringView(&cache), "");
    std::string folder(prRawStringView(&absolute).data, prRawStringView(&absolute).size);
    prRawStringReset(&absolute);
    prRawStringReset(&cache);
    folder += "/prax-tests/terminal";
    REQUIRE(prFileCreateDirectory(folder));
    std::string text = "one\ntwo\nthree\n";
    REQUIRE(prFileWrite(folder + "/" + name, PrByteView{reinterpret_cast<const uint8_t*>(text.data()), text.size()}));
    return folder;
}

// A file link that the terminal passed to its handler for opening files.
struct OpenedFile {
    std::string path;
    uint32_t line;
    uint32_t column;

    bool operator==(const OpenedFile& other) const {
        return path == other.path && line == other.line && column == other.column;
    }
};

// Describes the links found in `text`. A URL is "url|<url>", and a file reference is "<text>|<path>|<line>|<column>".
std::vector<std::string> textLinks(const std::string& text) {
    RawArray<TerminalTextLink> links;
    terminalTextLinks(PrStringView(text), &links);
    std::vector<std::string> out;
    for (const TerminalTextLink& link : links.span()) {
        std::string target = text.substr(link.targetStart, link.targetEnd - link.targetStart);
        if (link.kind == TerminalTextLinkKind_Url) {
            out.push_back("url|" + target);
            continue;
        }
        out.push_back(text.substr(link.start, link.end - link.start) + "|" + target + "|" + std::to_string(link.line) +
            "|" + std::to_string(link.column));
    }
    links.reset();
    return out;
}

// Returns true if each of the red, green and blue channels of `a` is within two of `b`. The tolerance covers the
// rounding of rendering's conversions.
bool near(PrColor8 a, PrColor8 b) {
    auto close = [](uint8_t x, uint8_t y) { return (x > y ? x - y : y - x) <= 2; };
    return close(a.r, b.r) && close(a.g, b.g) && close(a.b, b.b);
}

// Creates and returns the tests' folder `name` under the user's cache directory.
std::string testFolder(const std::string& name) {
    PrRawString cache = prFileUserDirectory(PrFileUserDirectory_Cache);
    std::string folder(prRawStringView(&cache).data, prRawStringView(&cache).size);
    prRawStringReset(&cache);
    folder += "/prax-tests/terminal/" + name;
    REQUIRE(prFileCreateDirectory(folder));
    return folder;
}

void writeFile(const std::string& path, const std::string& text) {
    REQUIRE(prFileWrite(path, PrByteView{reinterpret_cast<const uint8_t*>(text.data()), text.size()}));
}

std::string readFile(const std::string& path) {
    PrByteView bytes{};
    REQUIRE(prFileMap(path, &bytes));
    std::string text(reinterpret_cast<const char*>(bytes.data), bytes.size);
    prFileUnmap(&bytes);
    return text;
}

// Returns the text of `string`, and releases `string`.
std::string taken(PrRawString string) {
    std::string text(prRawStringView(&string).data, prRawStringView(&string).size);
    prRawStringReset(&string);
    return text;
}

std::vector<std::string> strings(const RawArray<PrRawString>& array) {
    std::vector<std::string> out;
    for (const PrRawString& string : array.span())
        out.emplace_back(prRawStringView(&string).data, prRawStringView(&string).size);
    return out;
}

std::string nameOf(const std::string& path) {
    PrStringView name = prFilePathName(path);
    return std::string(name.data, name.size);
}

// The installed shells that the terminal has an integration for, with one path for each shell.
std::vector<std::string> integratedShells() {
    std::vector<std::string> found;
    for (const char* path : {"/bin/zsh", "/usr/bin/zsh", "/bin/bash", "/usr/bin/bash"}) {
        bool seen = false;
        for (const std::string& shell : found)
            seen = seen || nameOf(shell) == nameOf(path);
        PrFileInfo info{};
        if (!seen && prFileInfo(path, &info) && info.kind == PrFileKind_Regular)
            found.emplace_back(path);
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
const std::string kPrompt = "\x1b]133;A\x07$ \x1b]133;B\x07";

// What a shell with the terminal's integration writes for one command: a prompt, the typed `input`, each line of
// `output`, and the command's end with the exit code `exit`.
std::string command(const std::string& input, const std::vector<std::string>& output, const std::string& exit) {
    std::string text = kPrompt + input + "\r\n\x1b]133;C\x07";
    for (const std::string& line : output)
        text += line + "\r\n";
    return text + "\x1b]133;D;" + exit + "\x07";
}

// `count` lines of output, each `prefix` followed by its index.
std::vector<std::string> numbered(const std::string& prefix, uint32_t count) {
    std::vector<std::string> lines;
    for (uint32_t i = 0; i < count; ++i)
        lines.push_back(prefix + std::to_string(i));
    return lines;
}

std::string joined(const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& line : lines)
        text += (text.empty() ? "" : "\n") + line;
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
std::string hyperlink(const std::string& uri, const std::string& params = "") {
    return "\x1b]8;" + params + ";" + uri + "\x1b\\";
}
const std::string kHyperlinkEnd = "\x1b]8;;\x1b\\";

// Renders the tree that the last frame laid out, and returns true if any pixel in the lower half of the cell at
// `column` and `row` is near `color`.
bool cellHasColor(Term& t, uint32_t column, uint32_t row, PrColor8 color) {
    std::vector<uint8_t> rgba;
    REQUIRE(t.f.render(&rgba));
    PrVec2 center = t.cellCenter(column, row);
    PrVec2 cell = prTerminalCellExtent(t.terminal);
    for (uint32_t y = uint32_t(center.y); y < uint32_t(center.y + cell.y / 2); ++y) {
        for (uint32_t x = uint32_t(center.x - cell.x / 2) + 1; x + 1 < uint32_t(center.x + cell.x / 2); ++x) {
            size_t i = (size_t(y) * KitFixture::kRenderWidth + x) * 4;
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
    std::vector<bool> inked;
    PrRect bounds{1e9f, 1e9f, -1e9f, -1e9f};
};

// Renders the tree that the last frame laid out, and returns the ink of the cell at `column` and `row`.
CellInk cellInk(Term& t, uint32_t column, uint32_t row) {
    std::vector<uint8_t> rgba;
    REQUIRE(t.f.render(&rgba));
    PrColor8 background = prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Background);
    PrVec2 center = t.cellCenter(column, row);
    PrVec2 cell = prTerminalCellExtent(t.terminal);
    CellInk ink;
    for (uint32_t y = uint32_t(center.y - cell.y / 2); y < uint32_t(center.y + cell.y / 2); ++y) {
        for (uint32_t x = uint32_t(center.x - cell.x / 2); x < uint32_t(center.x + cell.x / 2); ++x) {
            size_t i = (size_t(y) * KitFixture::kRenderWidth + x) * 4;
            bool inked = !near(PrColor8{rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]}, background);
            ink.inked.push_back(inked);
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
std::string graphics(const std::string& control, const std::string& pixels = "") {
    std::string payload(prBase64EncodedSize(pixels.size()), '\0');
    prBase64Encode(pixels, payload.data(), payload.size());
    return "\x1b_G" + control + (pixels.empty() ? "" : ";" + payload) + "\x1b\\";
}

// The pixel extent of a cell on the terminal's screen.
PrUVec2 cellPixelExtent(PrTerminal* terminal) {
    return prVtermCellPixelExtent(Locked(terminal));
}

// RGB pixels of one color, enough to fill `columns` by `rows` cells of the terminal's screen.
std::string cellPixels(PrTerminal* terminal, uint32_t columns, uint32_t rows, PrColor8 color) {
    PrUVec2 cell = cellPixelExtent(terminal);
    std::string out;
    for (uint32_t i = 0; i < cell.x * columns * cell.y * rows; ++i)
        out += std::string{char(color.r), char(color.g), char(color.b)};
    return out;
}

// The control keys that transmit and put an RGB image of `columns` by `rows` cells without moving the cursor, followed
// by `keys`.
std::string cellImage(PrTerminal* terminal, uint32_t columns, uint32_t rows, const std::string& keys) {
    PrUVec2 cell = cellPixelExtent(terminal);
    return "a=T,f=24,C=1,s=" + std::to_string(cell.x * columns) + ",v=" + std::to_string(cell.y * rows) + keys;
}

constexpr PrColor8 kImageRed = {0xff, 0x00, 0x00, 0xff};

// Returns an upper bound on the wait at the `share` quantile of `info`: the upper edge of the histogram bucket that
// holds that quantile.
double waitAtShare(const PrTerminalLockInfo& info, double share) {
    uint64_t seen = 0;
    double bound = 0.001;
    for (uint32_t i = 0; i < kPrTerminalLockBuckets; ++i, bound *= 2) {
        seen += info.buckets[i];
        if (double(seen) >= share * double(info.count))
            return bound;
    }
    return info.worstMilliseconds;
}

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

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("terminal: the public header compiles as C") {
    CHECK(prTestTerminalCApi() == 0);
}

TEST_CASE("terminal: the screen fills its extent with whole cells, and the program's terminal has its size") {
    Term t("sh", {"-c", "stty size; sleep 5"});
    PrUVec2 extent = prVtermExtent(Locked(t.terminal));
    CHECK(extent.x >= 60);
    CHECK(extent.x <= 100);
    CHECK(extent.y >= 20);
    CHECK(extent.y <= 40);
    std::string size = std::to_string(extent.y) + " " + std::to_string(extent.x);
    t.frameUntil([&] { return t.shows(size); });
    CHECK(t.f.paintedCommands() > 0);
}

TEST_CASE("terminal: typed text reaches the program, whose terminal echoes it") {
    Term t("cat", {});
    prTerminalFocus(t.terminal);
    t.f.frame();
    REQUIRE(prUiFocus(t.f.ui) == prTerminalNode(t.terminal));
    t.f.type("hi");
    t.f.key(PrAppKey_Enter);
    // The terminal echoes the line, and then cat writes it.
    t.frameUntil([&] { return t.shows("hi\nhi\n"); });
}

TEST_CASE("terminal: the program sets the title, and its exit shows on the screen") {
    Term t("sh", {"-c", "printf '\\033]2;Greeting\\007'; exit 3"});
    t.frameUntil([&] { return t.shows("[exited with code 3]"); });
    CHECK(prTerminalTitle(t.terminal) == "Greeting");
    PrProcessInfo info = prTerminalProcessInfo(t.terminal);
    CHECK(info.exited);
    CHECK(info.code == 3);
}

TEST_CASE("terminal: a double click selects a word, a triple click a line, and a drag with Alt a block") {
    Term t("sh", {"-c", "printf 'alpha beta-gamma delta\\nbravo charlie\\n'; sleep 5"});
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
    CHECK(t.selection().empty());
}

TEST_CASE("terminal: the pointer over a URL with the primary modifier shows that a click opens it") {
    Term t("sh", {"-c", "printf 'see (https://example.com/a), or not\\n'; sleep 5"});
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
    Term t("sh", {"-c", "printf 'see (https://example.com/a) and www.example.org file:///tmp\\n'; sleep 5"}, &app.app);
    t.frameUntil([&] { return t.shows("file:///tmp"); });
    t.f.click(t.cellCenter(10, 0));
    t.f.frame();
    CHECK(app.urls.empty());
    t.f.click(t.cellCenter(10, 0), primaryModifier());
    t.f.frame();
    // Only http and https URLs are found in the text.
    t.f.click(t.cellCenter(36, 0), primaryModifier());
    t.f.frame();
    t.f.click(t.cellCenter(50, 0), primaryModifier());
    t.f.frame();
    CHECK(app.urls == std::vector<std::string>{"https://example.com/a"});
    CHECK(t.selection().empty());
}

TEST_CASE("terminal: a hyperlink under the pointer with the primary modifier underlines all of its cells") {
    FakeApp app;
    Term t("sleep", {"10"}, &app.app);
    t.settle();
    // One link in two parts, whose text differs from its URI, followed by a URL in plain text.
    t.write(hyperlink("https://example.com/real", "id=a") + "one" + kHyperlinkEnd + " gap " +
        hyperlink("https://example.com/real", "id=a") + "three" + kHyperlinkEnd + " https://example.com/text");
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
    CHECK(app.urls == std::vector<std::string>{"https://example.com/real", "https://example.com/text"});
}

TEST_CASE("terminal: a hyperlink at rest has a faint dotted underline, and text without a link has none") {
    Term t("sleep", {"10"});
    t.settle();
    t.write("____\r\n" + hyperlink("https://example.com/") + "____" + kHyperlinkEnd);
    t.f.frame();
    std::vector<uint8_t> rgba;
    REQUIRE(t.f.render(&rgba));
    // The lower halves of the cells in the two rows differ only by the link's underline.
    auto lowerHalf = [&](uint32_t row) {
        PrVec2 center = t.cellCenter(1, row);
        PrVec2 cell = prTerminalCellExtent(t.terminal);
        std::vector<uint8_t> pixels;
        for (uint32_t y = uint32_t(center.y); y < uint32_t(center.y + cell.y / 2); ++y) {
            size_t start = (size_t(y) * KitFixture::kRenderWidth + size_t(center.x - cell.x / 2)) * 4;
            pixels.insert(pixels.end(), rgba.begin() + long(start), rgba.begin() + long(start + size_t(cell.x) * 4));
        }
        return pixels;
    };
    CHECK(lowerHalf(0) != lowerHalf(1));
    CHECK_FALSE(cellHasColor(t, 1, 1, color8Of(t.f.kit->theme.palette.accent)));
}

TEST_CASE("terminal: a hyperlink to a file that exists opens through the handler, and any other through the app") {
    std::string folder = folderWithFile("linked.txt");
    FakeApp app;
    std::vector<OpenedFile> opened;
    auto setup = [&](PrTerminalDesc* desc) {
        desc->openFile = [](void* context, const PrTerminalFileLink* link) {
            static_cast<std::vector<OpenedFile>*>(context)->push_back(
                OpenedFile{std::string(link->path.data, link->path.size), link->line, link->column});
        };
        desc->openFileContext = &opened;
    };
    Term t("sleep", {"10"}, &app.app, {}, setup);
    t.settle();
    std::string uri = taken(prUrlFromFilePath(folder + "/linked.txt"));
    t.write(hyperlink(uri) + "here" + kHyperlinkEnd + " " + hyperlink("file:///nowhere/missing.txt") + "gone" +
        kHyperlinkEnd + " " + hyperlink("file://elsewhere.invalid/x") + "far" + kHyperlinkEnd);
    t.f.frame();
    t.f.click(t.cellCenter(1, 0), primaryModifier());
    t.f.frame();
    t.f.click(t.cellCenter(6, 0), primaryModifier());
    t.f.frame();
    t.f.click(t.cellCenter(11, 0), primaryModifier());
    t.f.frame();
    CHECK(opened == std::vector<OpenedFile>{OpenedFile{folder + "/linked.txt", 0, 0}});
    CHECK(app.urls == std::vector<std::string>{"file:///nowhere/missing.txt", "file://elsewhere.invalid/x"});
}

TEST_CASE("terminal: a change of the palette, the roles or reverse video redraws the rows that it colors") {
    Term t("sleep", {"10"});
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
    Term t("sleep", {"10"});
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
    Term t("sleep", {"10"});
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
    for (PrVec2 at : {blank, gutter, corner}) {
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
    Term t("sleep", {"10"});
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
    Term t("sleep", {"10"}, &app.app);
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
    Term t("sleep", {"10"}, &app.app);
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
    Term t("sleep", {"10"}, &app.app);
    // The terminal shows in the active window without focus. The default and unfocused conditions post, and the
    // invisible condition does not.
    t.write("\x1b]9;one\x07\x1b]99;o=unfocused;two\x1b\\\x1b]99;o=invisible;three\x1b\\");
    t.f.frame();
    CHECK(app.posted == std::vector<std::string>{"sleep|one|9", "two||9"});
    CHECK(prTerminalAlerts(t.terminal) == 0);

    app.posted.clear();
    prTerminalFocus(t.terminal);
    t.f.frame();
    t.f.frame();
    REQUIRE(prTerminalHasFocus(t.terminal));
    t.write("\x1b]9;four\x07\x1b]99;o=always;five\x1b\\\x1b]777;notify;Six;six\x07");
    t.f.frame();
    CHECK(app.posted == std::vector<std::string>{"five||9"});

    // In an inactive window, every notification posts and raises an alert until the window becomes active.
    app.posted.clear();
    app.active = false;
    t.write("\x1b]777;notify;Seven;seven\x07\x1b]99;o=invisible;eight\x1b\\");
    t.f.frame();
    CHECK(app.posted == std::vector<std::string>{"Seven|seven|9", "eight||9"});
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
        std::string chunk(4 * kPrTerminalParseSliceBytes, 'a');
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

// The wait's duration is reported, and not checked: the scheduler can preempt the parser while it holds the lock, for
// as long as the machine's load keeps it off the processor. What the parser took during a wait is the bound that load
// cannot move.
TEST_CASE("terminal: while the program floods the screen, the UI waits for it no longer than a slice of parsing") {
    FakeLauncher fake;
    Term t("program", {}, nullptr, {}, fake.setup());
    REQUIRE(fake.receive != nullptr);
    // Colored text, wide characters and scrolling lines, which parse slower than plain text, in pieces of several
    // slices.
    std::string line = "\033[31mred \033[1;4mbold\033[0m \xe6\xbc\xa2\xe5\xad\x97 caf\xc3\xa9 plain text\r\n";
    std::string text;
    while (text.size() < 4 * kPrTerminalParseSliceBytes)
        text += line;
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
    MESSAGE("waits " << info.count << ", median under " << waitAtShare(info, 0.5) * 1000 << " us, 99.9% under " <<
        waitAtShare(info, 0.999) * 1000 << " us, mean " << info.totalMilliseconds / double(info.count) * 1000 <<
        " us, worst " << info.worstMilliseconds * 1000 << " us, most parsed during a wait " << info.worstBytes << " bytes");
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
        fake.output("line " + std::to_string(i) + "\r\n");
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
    prTerminalFocus(t.terminal);
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
    // The keys are pressed once the program reads input.
    Term t("sh", {"-c", "echo ready; cat"}, &app.app);
    prTerminalFocus(t.terminal);
    t.frameUntil([&] { return t.shows("ready"); });
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
    // Control with C copies and clears the selection. Pressed again, it interrupts the program.
    t.f.key('C', PrAppModifierFlag_Control);
    t.f.frame();
    CHECK(app.clipboard == "alpha");
    CHECK(t.selection().empty());
    t.f.key('C', PrAppModifierFlag_Control);
    t.f.frame();
    t.frameUntil([&] { return prTerminalProcessInfo(t.terminal).exited; });
#endif
}

TEST_CASE("terminal: a program that asks for key releases has them, but for the keys that the view takes") {
    FakeApp app;
    Term t("sh", {"-c", "stty raw -echo; printf '\\033[>2u'; echo ready; cat -v"}, &app.app);
    prTerminalFocus(t.terminal);
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

TEST_CASE("terminal: a terminal that has no title is named for its program, without its directory") {
#if defined(_WIN32)
    // A pseudoconsole sets the title shortly after the launch, so the check comes first.
    PrRawString shell = prEnvironmentVariable("COMSPEC");
    std::string program(prRawStringView(&shell).data, prRawStringView(&shell).size);
    prRawStringReset(&shell);
    REQUIRE(program.find('\\') != std::string::npos);
    Term t(program, {"/c", "set /p line="});
    CHECK(prTerminalTitle(t.terminal) == program.substr(program.rfind('\\') + 1));
#else
    Term t("/bin/cat", {});
    CHECK(prTerminalTitle(t.terminal) == "cat");
#endif
}

TEST_CASE("terminal: a terminal of no program runs the user's shell") {
    Term t("", {});
    prTerminalFocus(t.terminal);
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
    Term t("sh", {"-c", "stty raw -echo; echo ready; cat -v"});
    prTerminalFocus(t.terminal);
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
    Term t("sh", {"-c", "stty raw -echo; echo ready; cat -v"});
    prTerminalFocus(t.terminal);
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
    std::string chunk(4 * kPrTerminalParseSliceBytes, 'a');
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
    Term t("sh", {"-c", "stty raw -echo; printf '\\033[?1004h'; echo ready; cat -v"});
    prTerminalFocus(t.terminal);
    t.frameUntil([&] { return t.shows("ready") && prTerminalHasFocus(t.terminal); });
    t.f.build = [&](PrLayoutNode) { prTerminalUpdate(t.terminal); };
    t.f.frame();
    CHECK_FALSE(prTerminalHasFocus(t.terminal));
    t.frameUntil([&] { return t.shows("^[[O"); });
}

TEST_CASE("terminal: the working directory is the file URL of this machine that the program reports, decoded") {
    Term t("sleep", {"10"}, nullptr, "/");
    t.settle();
    // Without a report, the directory is the one that the program started in.
    CHECK(t.directory() == "/");
    t.write("\x1b]7;file:///tmp/a%20b\x07");
    CHECK(t.directory() == "/tmp/a b");
    t.write("\x1b]7;file://localhost/usr/lib\x1b\\");
    CHECK(t.directory() == "/usr/lib");
    PrRawString host = prEnvironmentHostName();
    t.write("\x1b]7;file://" + std::string(prRawStringView(&host).data, prRawStringView(&host).size) + "/var\x07");
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
    std::string folder = folderWithFile("started.txt");
    Term t("cmd", {"/c", "cd .. & set /p line="}, nullptr, folder);
    prThreadSleep(200);
    t.f.frame();
    CHECK(t.directory() == folder);
}
#else
TEST_CASE("terminal: without a report, the working directory is the foreground program's") {
    Term t("sh", {"-c", "cd /tmp && pwd -P && exec sleep 10"}, nullptr, "/");
    t.frameUntil([&] { return t.screen().find('\n') > 1; });
    std::string screen = t.screen();
    std::string expected = screen.substr(0, screen.find('\n'));
    t.frameUntil([&] { return t.directory() == expected; });
}
#endif

TEST_CASE("terminal: a new font remakes the cells, and the program hears of the new columns and rows") {
    // The program reports its size when it reads a line. The frame that takes the new font resizes the program's
    // terminal before the line is typed, so the report has the new size. A trap on SIGWINCH would depend on when the
    // signal arrives, which under a Windows pseudoconsole is late or never.
    Term t("sh", {"-c", "echo ready; read line; stty size; sleep 10"});
    prTerminalFocus(t.terminal);
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
    t.frameUntil([&] { return t.shows(std::to_string(fewer.y) + " " + std::to_string(fewer.x)); });
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
    Term t("sleep", {"10"});
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
        Term t("sleep", {"10"}, nullptr, {}, nullptr,
            KitFonts{.bundles = {PrFontBundle_NotoMono, PrFontBundle_NerdFontSymbols}, .system = system});
        t.settle();
        // A Han ideograph and an emoji, each two columns wide, and a Nerd Fonts file icon. Each is followed by a blank
        // cell.
        t.write("\xe6\xbc\xa2 \xf0\x9f\x98\x80 \xef\x85\x9b ");
        t.f.frame();
        PrVec2 cell = prTerminalCellExtent(t.terminal);
        for (uint32_t column : {0u, 3u, 6u}) {
            CAPTURE(column);
            CHECK(float(cellInk(t, column, 0).count) > cell.x * cell.y / 8);
        }
        // The ideograph and the emoji fall back to families that the system provides.
        CHECK(prFontSetFallbackCount(t.f.fonts) >= 3);
    }
    prFontSystemDestroy(system);
}

TEST_CASE("terminal: blinking text hides and shows on a steady period over its background") {
    Term t("sleep", {"10"});
    t.settle();
    // A full block that blinks over a red background, and one that does not.
    t.write("\x1b[5;41m\xe2\x96\x88\x1b[0m \xe2\x96\x88");
    t.f.frame();
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
    Term t("sleep", {"10"});
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
    prTerminalFocus(t.terminal);
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
    CHECK(textLinks("see (https://example.com/a), or not") == std::vector<std::string>{"url|https://example.com/a"});
    CHECK(textLinks("<http://x.org/p?q=1>.") == std::vector<std::string>{"url|http://x.org/p?q=1"});
    // A URL's path is not a file reference. Only http and https start URLs, so a bare name with a dot may name a file.
    CHECK(textLinks("https://x.com/a.txt:3") == std::vector<std::string>{"url|https://x.com/a.txt:3"});
    CHECK(textLinks("www.example.org") == std::vector<std::string>{"www.example.org|www.example.org|0|0"});
    CHECK(textLinks("a.cpp:1 https://b/ c.h:2") ==
        std::vector<std::string>{"a.cpp:1|a.cpp|1|0", "url|https://b/", "c.h:2|c.h|2|0"});
}

TEST_CASE("terminal: references to files are found with the line and the column that tools print after them") {
    CHECK(textLinks("src/a.cpp:12:5: error: no") == std::vector<std::string>{"src/a.cpp:12:5|src/a.cpp|12|5"});
    CHECK(textLinks("a.cpp:7 b") == std::vector<std::string>{"a.cpp:7|a.cpp|7|0"});
    CHECK(textLinks("Makefile:3: *** missing") == std::vector<std::string>{"Makefile:3|Makefile|3|0"});
    CHECK(textLinks("C:\\x\\a.cpp(12,5): error C2065") ==
        std::vector<std::string>{"C:\\x\\a.cpp(12,5)|C:\\x\\a.cpp|12|5"});
    CHECK(textLinks("foo.h(3): warning") == std::vector<std::string>{"foo.h(3)|foo.h|3|0"});
    CHECK(textLinks("  File \"/tmp/x y.py\", line 42, in <module>") ==
        std::vector<std::string>{"/tmp/x y.py\", line 42|/tmp/x y.py|42|0"});
    CHECK(textLinks("    at f (/a/b.js:10:15)") == std::vector<std::string>{"/a/b.js:10:15|/a/b.js|10|15"});
    CHECK(textLinks("see notes.txt, then ./run") ==
        std::vector<std::string>{"notes.txt|notes.txt|0|0", "./run|./run|0|0"});
    CHECK(textLinks("plain words: here").empty());
}

TEST_CASE("terminal: a click with the primary modifier opens a file that a reference names, from the directory") {
    std::string folder = folderWithFile("hello.txt");
    std::vector<OpenedFile> opened;
    auto setup = [&](PrTerminalDesc* desc) {
        desc->directory = PrStringView(folder);
        desc->openFile = [](void* context, const PrTerminalFileLink* link) {
            static_cast<std::vector<OpenedFile>*>(context)->push_back(
                OpenedFile{std::string(link->path.data, link->path.size), link->line, link->column});
        };
        desc->openFileContext = &opened;
    };
    Term t("sh", {"-c", "printf 'hello.txt:3:2: note\\nmissing.txt:1:1: note\\n'; sleep 10"}, nullptr, {}, setup);
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
    REQUIRE(opened.size() == 1);
    CHECK(opened[0] == OpenedFile{folder + "/hello.txt", 3, 2});

    // A directory that the program reports replaces the one that it started in.
    t.write("\x1b]7;file:///nowhere/at/all\x07");
    t.f.frame();
    t.move(t.cellCenter(2, 0), primaryModifier());
    t.f.frame();
    CHECK(prUiOutput(t.f.ui).cursor == PrAppCursor_Text);
}

TEST_CASE("terminal: a reference to a file opens as a file URL without a handler") {
    std::string folder = folderWithFile("hello.txt");
    FakeApp app;
    auto setup = [&](PrTerminalDesc* desc) { desc->directory = PrStringView(folder); };
    Term t("sh", {"-c", "printf 'hello.txt:3\\n'; sleep 10"}, &app.app, {}, setup);
    t.frameUntil([&] { return t.shows("hello.txt"); });
    t.f.click(t.cellCenter(2, 0), primaryModifier());
    t.f.frame();
    CHECK(app.urls == std::vector<std::string>{taken(prUrlFromFilePath(folder + "/hello.txt"))});
}

TEST_CASE("terminal: the shell integration writes its scripts, and starts each shell that it knows to read them") {
    std::string directory = testFolder("integration");
    TerminalShellLaunch launch{};
    CHECK_FALSE(terminalShellIntegrate("/bin/sh", directory, nullptr, 0, &launch));
    CHECK(launch.arguments.count == 0);

    const PrStringView zdotdir[] = {"ZDOTDIR=/home/me/zsh"};
    REQUIRE(terminalShellIntegrate("/usr/local/bin/zsh", directory, zdotdir, 1, &launch));
    CHECK(strings(launch.arguments) == std::vector<std::string>{"-l"});
    CHECK(strings(launch.environment) ==
        std::vector<std::string>{"ZDOTDIR=" + directory + "/zsh", "PRAX_ZSH_ZDOTDIR=/home/me/zsh"});
    CHECK(readFile(directory + "/zsh/.zshenv").find("PRAX_ZSH_ZDOTDIR") != std::string::npos);
    terminalShellLaunchReset(&launch);

    REQUIRE(terminalShellIntegrate("bash", directory, nullptr, 0, &launch));
    CHECK(strings(launch.arguments) == std::vector<std::string>{"--rcfile", directory + "/bash/prax.bash"});
    CHECK(strings(launch.environment) == std::vector<std::string>{"PRAX_BASH_LOGIN=1"});
    terminalShellLaunchReset(&launch);
    const PrStringView posix[] = {"POSIXLY_CORRECT=1"};
    CHECK_FALSE(terminalShellIntegrate("bash", directory, posix, 1, &launch));

    // fish reads the integration's data directory and then the default data directories. If the environment sets
    // XDG_DATA_DIRS, its directories replace the defaults.
    const PrStringView unset[] = {"XDG_DATA_DIRS="};
    REQUIRE(terminalShellIntegrate("/opt/bin/fish", directory, unset, 1, &launch));
    CHECK(strings(launch.environment) ==
        std::vector<std::string>{"XDG_DATA_DIRS=" + directory + ":/usr/local/share:/usr/share"});
    terminalShellLaunchReset(&launch);
    const PrStringView data[] = {"XDG_DATA_DIRS=/opt/share"};
    REQUIRE(terminalShellIntegrate("fish", directory, data, 1, &launch));
    CHECK(strings(launch.environment) ==
        std::vector<std::string>{"XDG_DATA_DIRS=" + directory + ":/opt/share", "PRAX_FISH_XDG_DATA_DIRS=/opt/share"});
    terminalShellLaunchReset(&launch);
    PrFileInfo info{};
    CHECK(prFileInfo(directory + "/fish/vendor_conf.d/prax-shell-integration.fish", &info));

    // A script that changed is written again.
    writeFile(directory + "/zsh/prax.zsh", "changed");
    REQUIRE(terminalShellIntegrate("zsh", directory, nullptr, 0, &launch));
    terminalShellLaunchReset(&launch);
    CHECK(readFile(directory + "/zsh/prax.zsh") != "changed");
}

TEST_CASE("terminal: the user's shell starts with the integration, after its own startup files, and marks commands") {
    std::vector<std::string> shells = integratedShells();
    if (shells.empty())
        MESSAGE("no shell that the terminal has an integration for");
    for (const std::string& shell : shells) {
        CAPTURE(shell);
        std::string home = testFolder("home-" + nameOf(shell));
        writeFile(home + "/.zshrc", "export PRAX_TEST_RC=read\nPS1='ready$ '\n");
        writeFile(home + "/.bash_profile", "export PRAX_TEST_RC=read\nPS1='ready$ '\n");
        // The shell reads its startup files from the test home. For zsh, ZDOTDIR names that home, and it still does
        // inside the shell.
        std::string shellVariable = "SHELL=" + shell;
        std::string homeVariable = "HOME=" + home;
        std::string zdotdirVariable = "ZDOTDIR=" + home;
        PrStringView environment[] = {shellVariable, homeVariable, zdotdirVariable, "BASH_SILENCE_DEPRECATION_WARNING=1"};
        auto setup = [&](PrTerminalDesc* desc) {
            desc->environment = environment;
            desc->environmentCount = uint32_t(countOf(environment));
        };
        Term t("", {}, nullptr, {}, setup);
        prTerminalFocus(t.terminal);
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
        std::vector<PrVtermCommandStatus> statuses;
        for (uint32_t i = 0; i < prVtermScrollbackCount(vterm) + prVtermExtent(vterm).y; ++i) {
            PrVtermLine line = prVtermLine(vterm, i);
            if ((line.marks & PrVtermLineMarkFlag_Prompt) != 0)
                statuses.push_back(line.commandStatus);
        }
        CHECK(statuses == std::vector<PrVtermCommandStatus>{
            PrVtermCommandStatus_Succeeded, PrVtermCommandStatus_Failed, PrVtermCommandStatus_None,
        });
    }
}

TEST_CASE("terminal: a terminal whose caller turns the integration off runs the user's shell without it") {
    std::vector<std::string> shells = integratedShells();
    if (shells.empty())
        MESSAGE("no shell that the terminal has an integration for");
    for (const std::string& shell : shells) {
        CAPTURE(shell);
        std::string home = testFolder("home-" + nameOf(shell));
        std::string shellVariable = "SHELL=" + shell;
        std::string homeVariable = "HOME=" + home;
        std::string zdotdirVariable = "ZDOTDIR=" + home;
        PrStringView environment[] = {shellVariable, homeVariable, zdotdirVariable, "BASH_SILENCE_DEPRECATION_WARNING=1"};
        auto setup = [&](PrTerminalDesc* desc) {
            desc->environment = environment;
            desc->environmentCount = uint32_t(countOf(environment));
            desc->shellIntegration = PrTerminalShellIntegration_Off;
        };
        Term t("", {}, nullptr, {}, setup);
        prTerminalFocus(t.terminal);
        t.f.frame();
        t.f.type("echo off-$((1 + 2))");
        t.f.key(PrAppKey_Enter);
        t.frameUntil([&] { return t.shows("off-3"); });
        CHECK(prTerminalShellInfo(t.terminal).state == PrVtermShellState_None);
    }
}

TEST_CASE("terminal: the last command's output and exit code are the application's to read and copy") {
    FakeApp app;
    Term t("sleep", {"10"}, &app.app);
    t.settle();
    CHECK(taken(prTerminalCommandOutput(t.terminal)).empty());
    t.write(command("one", {"a"}, "0") + command("two", {"b", "c  ", ""}, "4") + kPrompt);
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
    Term t("sleep", {"10"});
    t.settle();
    uint32_t rows = prVtermExtent(Locked(t.terminal)).y;
    std::vector<std::string> first = numbered("f", rows + 4);
    std::vector<std::string> second = numbered("s", rows + 4);
    t.write(command("first", first, "0") + command("second", second, "1") + kPrompt);
    prTerminalFocus(t.terminal);
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
    Term t("sleep", {"10"});
    t.settle();
    t.write(command("ok", {}, "0") + command("bad", {}, "2") + kPrompt);
    t.f.frame();
    const PrKitPalette& palette = t.f.kit->theme.palette;
    CHECK(prTerminalScreenOrigin(t.terminal).x > 0);
    CHECK(near(t.pixel(gutterAt(t, 0)), color8Of(palette.success)));
    CHECK(near(t.pixel(gutterAt(t, 1)), color8Of(palette.danger)));
    CHECK(near(t.pixel(gutterAt(t, 2)), prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Background)));
}

TEST_CASE("terminal: the cell's pixel extent is the cell's in target pixels, which the screen reports to programs") {
    Term t("sleep", {"10"});
    t.settle();
    PrVec2 cell = prTerminalCellExtent(t.terminal);
    PrUVec2 pixels = cellPixelExtent(t.terminal);
    CHECK(pixels.x == uint32_t(prF32Round(cell.x)));
    CHECK(pixels.y == uint32_t(prF32Round(cell.y)));
}

TEST_CASE("terminal: images draw over the text, between the backgrounds and the text, or under the backgrounds") {
    Term t("sleep", {"10"});
    t.settle();
    PrColor8 blue = prVtermPaletteColor(Locked(t.terminal), 4);
    PrColor8 foreground = prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Foreground);
    // Row 0: three cells with a blue background, each with an image in a different z-index band. Row 1: two full blocks
    // in the foreground color, with an image under the first and over the second.
    t.write("\x1b[44m   \x1b[0m\r\n\xe2\x96\x88\xe2\x96\x88\x1b[H");
    std::string red = cellPixels(t.terminal, 1, 1, kImageRed);
    t.write(graphics(cellImage(t.terminal, 1, 1, ",z=1"), red) + "\x1b[C");
    t.write(graphics(cellImage(t.terminal, 1, 1, ",z=-1"), red) + "\x1b[C");
    t.write(graphics(cellImage(t.terminal, 1, 1, ",z=-1073741825"), red));
    t.write("\x1b[2;1H" + graphics(cellImage(t.terminal, 1, 1, ",z=-1"), red) + "\x1b[C");
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
    Term t("sleep", {"10"});
    t.settle();
    PrColor8 background = prVtermRoleColor(Locked(t.terminal), PrVtermColorRole_Background);
    t.write("\x1b[3;5H" + graphics(cellImage(t.terminal, 2, 2, ""), cellPixels(t.terminal, 2, 2, kImageRed)));
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
    Term t("sleep", {"10"});
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
    Term t("sh", {"-c", "printf '\\033_Ga=T,f=24,s=1,v=1,c=2,r=1,i=5,q=2;/wAA\\033\\\\done'; sleep 5"});
    t.frameUntil([&] { return t.shows("done"); });
    t.f.frame();
    CHECK(near(t.pixel(t.cellCenter(0, 0)), kImageRed));
}
#endif
