#include "pr/checksum.h"
#include "pr/compress_builtin.h"
#include "pr/css_presentation.h"
#include "pr/css_view.h"
#include "pr/draw_cpu.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/draw_structure.h"
#include "pr/html.h"
#include "pr/html_forms.h"
#include "pr/html_page.h"
#include "pr/html_structure.h"
#include "pr/html_view.h"
#include "pr/image_builtin.h"
#include "pr/layout_compositor.h"
#include "pr/layout_page.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/text_harfbuzz.h"

#include "html/src/html_focus.h"
#include "html/tests/support/html_test_fetch.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

namespace pr_impl {

namespace {

// Sets the headroom of the display that the view is shown on, in sRGB's gamut.
void setHeadroom(PrHtmlView* view, float headroom) {
    PrAppDisplay display = prAppDisplayDefault();
    display.headroom = headroom;
    prHtmlViewSetDisplay(view, &display);
}

// Verifies the presentation's incremental work, and reports the first difference if there is one.
bool verified(PrCssPresentation* presentation) {
    PrRawString report{};
    bool same = prCssPresentationVerify(presentation, &report);
    if (!same) {
        PrStringView what = prRawStringView(&report);
        MESSAGE(std::string(what.data, what.size));
    }
    prRawStringReset(&report);
    return same;
}

// Renders `list` onto 40 by 40 RGBA pixels.
std::vector<uint8_t> presentation(PrDrawList* list) {
    std::vector<uint8_t> drawn(size_t(40) * 40 * 4, 0);
    PrDrawRenderer* cpu = prDrawCpuCreate(nullptr);
    PrPixels into{drawn.data(), 40 * 4, {40, 40}};
    prDrawRendererRenderPixels(cpu, list, into, PrPixelFormat_Rgba8);
    prDrawRendererDestroy(cpu);
    return drawn;
}

// Returns the red channel of a pixel of 40 by 40 RGBA pixels.
uint8_t redAt(const std::vector<uint8_t>& pixels, uint32_t x, uint32_t y) {
    return pixels[(size_t(y) * 40 + x) * 4];
}

// Returns the color of a pixel of 40 by 40 RGBA pixels as 0xRRGGBB.
uint32_t rgbAt(const std::vector<uint8_t>& pixels, uint32_t x, uint32_t y) {
    const uint8_t* p = &pixels[(size_t(y) * 40 + x) * 4];
    return uint32_t(p[0]) << 16 | uint32_t(p[1]) << 8 | p[2];
}

// Returns `png` with an eXIf chunk after its header chunk, which gives the EXIF orientation `orientation`.
std::vector<uint8_t> withOrientation(const std::vector<uint8_t>& png, uint8_t orientation) {
    // The chunk's type and a big-endian TIFF directory of one entry, the orientation tag.
    std::vector<uint8_t> typed = {
        'e', 'X', 'I', 'f', 'M', 'M', 0, 42, 0, 0, 0, 8, 0, 1, 0x01, 0x12, 0, 3,
        0,   0,   0,   1,   0,   orientation, 0, 0, 0, 0, 0, 0,
    };
    uint32_t crc = prCrc32Update(0, typed);
    uint32_t length = uint32_t(typed.size() - 4);
    std::vector<uint8_t> chunk = {uint8_t(length >> 24), uint8_t(length >> 16), uint8_t(length >> 8), uint8_t(length)};
    chunk.insert(chunk.end(), typed.begin(), typed.end());
    chunk.insert(chunk.end(), {uint8_t(crc >> 24), uint8_t(crc >> 16), uint8_t(crc >> 8), uint8_t(crc)});
    // The signature and the header chunk take 33 bytes.
    std::vector<uint8_t> out(png.begin(), png.begin() + 33);
    out.insert(out.end(), chunk.begin(), chunk.end());
    out.insert(out.end(), png.begin() + 33, png.end());
    return out;
}

// A page in a browsing context, presented by a view to a compositor without a thread of its own, over Tinos. The page's
// clock moves only when runNextTask steps it, so that no rendering update comes due while a test sets the page up.
struct Viewed {
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextHarfBuzzCreate();
    PrTextFont* defaultFont = nullptr;
    PrHtmlPage* page = nullptr;
    PrLayoutCompositor* compositor = nullptr;
    PrHtmlView* view = nullptr;
    double now = 0;                 // the page's clock, in milliseconds
    uint32_t presented = 0;
    uint32_t wanted = 0;            // the times the compositor came to want a frame
    std::vector<uint8_t> pixels;    // the last frame presented, 40 by 40

    // The link that the host reports visited, or null for none.
    const PrDomElement* PR_NULLABLE visited = nullptr;

    // The resource that answers fetches, and the fetcher that holds them for answer() to answer, if it holds them.
    const std::vector<uint8_t>* PR_NULLABLE resource = nullptr;
    TestFetcher fetcher;

    // If `images` is given, the host decodes images. If `resource` is given, the host answers every fetch with it,
    // including the fetches of frames. The host answers at once, or when answer() is called if `holds` is set. The page
    // loads at `url`.
    explicit Viewed(const char* html, PrImageCodec* PR_NULLABLE images = nullptr,
            const std::vector<uint8_t>* PR_NULLABLE resource = nullptr, bool holds = false,
            const char* url = "about:blank") : resource(resource) {
        std::string path = std::string(PR_FONTS_DIR) + "/Tinos.ttf";
        const PrFont* font = nullptr;
        REQUIRE(prFontCreateFromFile(path, 0, nullptr, &font) == PrStatus_Ok);
        PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
        prFontSetAdd(fonts, &descriptor, font);
        prFontRelease(font);
        prFontSetSetDefaultFamily(fonts, prFontFamilyFromName("Tinos"));
        PrTextFontDesc defaultFontDesc{.set = fonts, .style = prFontStyleDefault()};
        defaultFont = prTextFontCreate(&defaultFontDesc);
        PrHtmlHost host = prHtmlHostDefault();
        host.context = this;
        host.now = [](void* context) { return static_cast<Viewed*>(context)->now; };
        host.elementState =
                [](void* context, PrHtmlPage*, const PrDomElement* element, PrDomHostState wanted) -> PrDomHostState {
            bool visited = element == static_cast<Viewed*>(context)->visited;
            return visited ? PrDomHostState(wanted & PrDomHostState_Visited) : PrDomHostState(0);
        };
        host.font = defaultFont;
        host.shaper = shaper;
        host.images = images;
        if (resource != nullptr)
            host.fetcher = fetcher.fetcher;
        if (!holds)
            fetcher.answer = [this](PrFetchInterceptRequest* request) { respond(request); };
        page = prHtmlPageCreate(&host);
        REQUIRE(prHtmlPageLoad(page, html, url, nullptr) == PrStatus_Ok);
        if (!holds)
            fetcher.settle(prHtmlPageAgent(page));
        PrLayoutCompositorDesc compositorDesc{
            .context = this,
            .now = [](void* PR_NULLABLE) { return 0.0; },
            .present = [](void* PR_NULLABLE context, PrDrawList* list, PrColor16) {
                Viewed* viewed = static_cast<Viewed*>(context);
                ++viewed->presented;
                viewed->pixels = presentation(list);
            },
            .wantFrame = [](void* PR_NULLABLE context) { ++static_cast<Viewed*>(context)->wanted; },
            .canvasFilled = true,
        };
        compositor = prLayoutCompositorCreate(&compositorDesc);
        PrHtmlViewDesc viewDesc{.page = page, .compositor = compositor, .canvasFilled = true};
        view = prHtmlViewCreate(&viewDesc);
    }

    ~Viewed() {
        prHtmlViewDestroy(view);
        prLayoutCompositorDestroy(compositor);
        prHtmlPageDestroy(page);
        prTextFontRelease(defaultFont);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
    }

    PrDomDocument* document() { return prHtmlWindowDocument(prHtmlPageWindow(page)); }

    // Steps the page's clock to the next task of its event loop, and runs the task. Returns false if there is none.
    bool runNextTask() {
        PrDomAgent* agent = prHtmlPageAgent(page);
        double delay = prDomAgentNextTaskDelay(agent);
        if (delay < 0)
            return false;
        now += delay;
        prDomAgentRunTask(agent);
        return true;
    }

    // Paints the view into 40 by 40 pixels.
    std::vector<uint8_t> paint() {
        PrDrawList* list = prDrawListCreate();
        prHtmlViewPaint(view, list);
        std::vector<uint8_t> drawn = presentation(list);
        prDrawListRelease(list);
        return drawn;
    }

    // Answers `request` with the PNG image that is the resource.
    void respond(PrFetchInterceptRequest* request) {
        fetcher.respondTo(request, std::string(resource->begin(), resource->end()), 200, "image/png");
    }

    // Answers the fetches held so far with the resource, and runs the tasks that the answers queue.
    void answer() {
        prNetNullRun(fetcher.net);
        std::vector<PrFetchInterceptRequest*> answering = fetcher.held;
        for (PrFetchInterceptRequest* request : answering)
            respond(request);
        fetcher.settle(prHtmlPageAgent(page));
    }

    // Answers the held fetch whose URL ends in `name` with `body`, of the content type `type`, and runs the tasks that
    // the answer queues.
    void answer(const std::string& name, const char* body, const char* type) {
        fetcher.waiting();
        if (!fetcher.respond(name, body, 200, type))
            FAIL("no fetch waits for " << name);
        fetcher.settle(prHtmlPageAgent(page));
    }

    PrDomElement* byId(const char* id) {
        PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(document()), id);
        REQUIRE(element != nullptr);
        return element;
    }

    // Waits for the job under way, and returns the update that commits it.
    PrHtmlViewUpdate settle() {
        prHtmlViewWait(view);
        return prHtmlViewUpdate(view);
    }

    bool matches(const char* id, const char* selector) {
        bool out = false;
        REQUIRE(prCssElementMatches(byId(id), selector, &out) == PrStatus_Ok);
        return out;
    }
};

// A page of inline SVG whose changes are checked against a full rebuild after each update.
struct ViewedSvg : Viewed {
    explicit ViewedSvg(const char* body) :
            Viewed((std::string("<style>body { margin: 0 } svg { display: block }</style>") + body).c_str()) {
        update();
    }

    // Brings the view up to date, checks the presentation against a full recomputation, and returns the nodes of SVG
    // trees that the update built.
    uint32_t update() {
        // The stats are those of the last update that started a job.
        bool started = prHtmlViewUpdate(view).started;
        settle();
        uint32_t built = started ? prCssPresentationStats(prHtmlPagePresentation(page)).svgNodesBuilt : 0;
        prHtmlViewUpdate(view);
        settle();
        CHECK(verified(prHtmlPagePresentation(page)));
        return built;
    }

    void set(const char* id, const char* name, const char* value) { prDomElementSetAttribute(byId(id), name, value); }

    PrDomNode* node(const char* id) { return prDomElementAsNode(byId(id)); }
};

// Two svg elements whose text is set in the SVG fonts Used and Other, and a font that no text reads.
constexpr const char* kSvgFonts =
    "<svg width='100' height='100'><font id='used' horiz-adv-x='500'><font-face font-family='Used'/>"
    "<glyph id='glyph' unicode='a' d='M0 0h100v100z'/></font>"
    "<font id='unread' horiz-adv-x='500'><font-face id='unread-face' font-family='Unread'/>"
    "<glyph id='unread-glyph' unicode='a' d='M0 0h100v100z'/></font>"
    "<text id='used-text' y='10' font-family='Used'>a</text></svg>"
    "<svg width='100' height='100'><text y='10' font-family='Other'>a</text><rect width='1' height='1'/>"
    "<rect width='2' height='2'/><rect width='3' height='3'/><rect width='4' height='4'/></svg>";

// The types of the events heard, in order.
struct Heard {
    std::vector<std::string> types;
};

PrDomEventListener recorder(Heard* heard) {
    return PrDomEventListener{
        [](void* context, PrDomEvent* event) {
            PrStringView type = prDomEventType(event);
            static_cast<Heard*>(context)->types.emplace_back(type.data, type.size);
        },
        heard,
        0,
        nullptr,
        nullptr,
    };
}

// The events heard, each as its type, then for a keyboard event its key and legacy key code, and for a composition
// event its data. An event that the engine did not fire is marked untrusted.
struct HeardKeys {
    std::vector<std::string> events;
    bool cancelKeydown = false;
};

PrDomEventListener keyRecorder(HeardKeys* heard) {
    return PrDomEventListener{
        [](void* context, PrDomEvent* event) {
            HeardKeys* heardKeys = static_cast<HeardKeys*>(context);
            PrStringView type = prDomEventType(event);
            std::string line(type.data, type.size);
            if (PrDomKeyboardEvent* key = prDomKeyboardEventQuery(event)) {
                PrDomKeyboardEventInit members = prDomKeyboardEventMembers(key);
                line += " " + std::string(members.key.data, members.key.size) + " " + std::to_string(members.keyCode);
                if (members.isComposing)
                    line += " composing";
            }
            if (PrDomCompositionEvent* composition = prDomCompositionEventQuery(event)) {
                PrStringView data = prDomCompositionEventData(composition);
                line += " " + std::string(data.data, data.size);
            }
            if (!prDomEventIsTrusted(event))
                line += " untrusted";
            heardKeys->events.push_back(line);
            if (heardKeys->cancelKeydown && line.rfind("keydown", 0) == 0)
                prDomEventPreventDefault(event);
        },
        heard,
        0,
        nullptr,
        nullptr,
    };
}

// Listens on `target` for the keyboard's, the composition's and click events.
void listenForKeys(PrDomElement* target, PrDomEventListener* listener) {
    for (const char* type : {"keydown", "keypress", "keyup", "click", "compositionstart", "compositionupdate",
        "compositionend"})
        prDomEventTargetAddEventListener(prDomNodeAsEventTarget(prDomElementAsNode(target)), type, listener, nullptr);
}

// Returns the events of pressing and releasing a key, with the text that it types between them.
std::vector<PrAppEvent> press(PrAppKey key, PrAppKeyCode code, const char* text, PrAppModifierFlags modifiers = 0) {
    PrAppEvent down{.kind = PrAppEventKind_KeyDown, .modifiers = modifiers, .keyboard = {.key = key, .code = code, .text = text}};
    PrAppEvent typed{.kind = PrAppEventKind_Text, .modifiers = modifiers, .input = {.text = text}};
    PrAppEvent up = down;
    up.kind = PrAppEventKind_KeyUp;
    if (text[0] == '\0')
        return {down, up};
    return {down, typed, up};
}

void send(PrHtmlView* view, const std::vector<PrAppEvent>& events) {
    for (const PrAppEvent& event : events)
        prHtmlViewEvent(view, &event, 0);
}

// Returns the ID of the element at a point of the page's viewport, its local name if it has none, or the empty string.
std::string hitAt(Viewed& page, double x, double y) {
    PrDomElement* element = prCssDocumentElementFromPoint(page.document(), x, y);
    if (element == nullptr)
        return "";
    PrStringView id = prDomElementGetAttribute(element, "id");
    PrStringView text = id.size != 0 ? id : prDomElementLocalName(element);
    return std::string(text.data, text.size);
}

// Sends a press and a release of the primary button at `point`.
void clickAt(Viewed& page, PrVec2 point) {
    PrAppEvent event{.kind = PrAppEventKind_PointerMove, .pointer = {.position = point}};
    prHtmlViewEvent(page.view, &event, 0);
    prHtmlViewUpdate(page.view);
    event.kind = PrAppEventKind_PointerDown;
    prHtmlViewEvent(page.view, &event, 0);
    event.kind = PrAppEventKind_PointerUp;
    prHtmlViewEvent(page.view, &event, 0);
}

// The wheel events heard, each as its target's id, its vertical delta, and whether it can be canceled. A listener
// cancels each one while `cancel` is set.
struct HeardWheels {
    std::vector<std::string> events;
    bool cancel = false;
};

PrDomEventListener wheelRecorder(HeardWheels* heard) {
    return PrDomEventListener{
        [](void* context, PrDomEvent* event) {
            HeardWheels* heardWheels = static_cast<HeardWheels*>(context);
            PrDomWheelEvent* wheel = prDomWheelEventQuery(event);
            REQUIRE(wheel != nullptr);
            PrDomElement* target = prDomElementQuery(prDomEventTarget(event));
            PrStringView id = target != nullptr ? prDomElementId(target) : PrStringView{};
            std::string line(id.data != nullptr ? id.data : "", id.size);
            line += " " + std::to_string(int(prDomWheelEventMembers(wheel).deltaY));
            line += prDomEventCancelable(event) ? " cancelable" : "";
            heardWheels->events.push_back(line);
            if (heardWheels->cancel)
                prDomEventPreventDefault(event);
        },
        heard,
        uint64_t(reinterpret_cast<uintptr_t>(heard)),
        nullptr,
        nullptr,
    };
}

// Returns a wheel's scroll by `pixels` down at `at`, at `time` in seconds.
PrAppEvent wheelAt(PrVec2 at, float pixels, double time) {
    return PrAppEvent{.kind = PrAppEventKind_Scroll, .time = time, .scroll = {.position = at, .delta = PrVec2{0, pixels}}};
}

// The edits heard at a textarea, each as its type, input type, data and whether it is composing, and its value after
// it. A listener cancels each beforeinput while `cancel` is set.
struct HeardEdits {
    std::vector<std::string> events;
    bool cancel = false;
};

PrDomEventListener editRecorder(HeardEdits* heard) {
    return PrDomEventListener{
        [](void* context, PrDomEvent* event) {
            HeardEdits* heard = static_cast<HeardEdits*>(context);
            PrStringView type = prDomEventType(event);
            std::string line(type.data, type.size);
            if (PrDomInputEvent* input = prDomInputEventQuery(event)) {
                PrStringView kind = prDomInputEventInputType(input);
                PrStringView data = prDomInputEventData(input);
                line += " " + std::string(kind.data, kind.size) + " [" + std::string(data.data, data.size) + "]";
                if (prDomInputEventIsComposing(input))
                    line += " composing";
                if (heard->cancel && prDomEventCancelable(event))
                    prDomEventPreventDefault(event);
            }
            heard->events.push_back(line);
        },
        heard,
        0,
        nullptr,
        nullptr,
    };
}

std::string valueOf(PrDomElement* element) {
    PrDomEventTarget* target = prDomNodeAsEventTarget(prDomElementAsNode(element));
    PrRawString value = prHtmlTextAreaElementValue(prHtmlTextAreaElementQuery(target));
    std::string out(prRawStringView(&value).data, prRawStringView(&value).size);
    prRawStringReset(&value);
    return out;
}

// A textarea with focus and a recorder on its edits, and the caret at the end of its text.
struct EditedArea {
    Viewed page;
    PrDomElement* area;
    HeardEdits heard;
    PrDomEventListener listener;

    explicit EditedArea(const char* markup) : page(markup) {
        area = page.byId("t");
        listener = editRecorder(&heard);
        for (const char* type : {"beforeinput", "input", "compositionstart", "compositionupdate", "compositionend"})
            prDomEventTargetAddEventListener(prDomNodeAsEventTarget(prDomElementAsNode(area)), type, &listener, nullptr);
        prHtmlFocus(area, nullptr);
        uint32_t end = uint32_t(valueOf(area).size());
        prHtmlTextControlSetSelectionRange(area, end, end, PrDomSelectionDirection_None);
    }
};

// Appends the text of the subtree of `node` in `tree` to `out`.
void appendText(PrLayoutTree* tree, PrLayoutNode node, std::string& out) {
    if (prLayoutNodeKind(tree, node) == PrLayoutNodeKind_Text) {
        PrStringView text = prLayoutNodeText(tree, node);
        out.append(text.data, text.size);
        return;
    }
    for (PrLayoutNode child = prLayoutNodeFirstChild(tree, node); child != kPrLayoutNodeNone;
            child = prLayoutNodeNextSibling(tree, child))
        appendText(tree, child, out);
}

// Returns the text of each margin box of the page at `index` of a paged view, by PrLayoutMarginBox, and "-" for a box
// that the page does not generate.
std::vector<std::string> marginTexts(Viewed& v, uint32_t index) {
    PrLayoutTree* tree = prCssPresentationTree(prHtmlPagePresentation(v.page));
    PrLayoutNode boxes[PrLayoutMarginBox_Count];
    prLayoutTreePageMargins(tree, index, boxes);
    std::vector<std::string> texts;
    for (PrLayoutNode box : boxes) {
        std::string text = box != kPrLayoutNodeNone ? "" : "-";
        if (box != kPrLayoutNodeNone)
            appendText(tree, box, text);
        texts.push_back(text);
    }
    return texts;
}

// Returns the text of the margin box `which` of the page at `index`.
std::string marginText(Viewed& v, uint32_t index, PrLayoutMarginBox which) {
    return marginTexts(v, index)[which];
}

// Lays `v` out as pages over a US letter page, and returns the number of pages.
uint32_t paged(Viewed& v) {
    PrLayoutPageBox initial = prLayoutPageBoxDefault();
    prHtmlViewSetPaged(v.view, &initial, 0);
    return prHtmlViewPageCount(v.view);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a change to an SVG font rebuilds only the text that reads its family") {
    ViewedSvg viewed(kSvgFonts);
    uint32_t nodes = viewed.update();
    CHECK(nodes == 0);

    // No text reads this family, whatever happens to its font.
    viewed.set("unread-glyph", "horiz-adv-x", "900");
    CHECK(viewed.update() == 0);
    viewed.set("unread-face", "unicode-range", "U+0061");
    CHECK(viewed.update() == 0);

    // The glyphs of the font that the first element's text reads, and not the second element, which has more nodes.
    viewed.set("glyph", "horiz-adv-x", "900");
    uint32_t used = viewed.update();
    CHECK(used != 0);
    CHECK(used < 6);

    // A face takes the name of a family that text looked for and did not find.
    viewed.set("unread-face", "font-family", "Other");
    uint32_t other = viewed.update();
    CHECK(other > used);

    // Removing the font takes it from the text that reads it.
    prDomNodeRemoveChild(prDomNodeParentNode(viewed.node("used")), viewed.node("used"));
    CHECK(viewed.update() != 0);
}

TEST_CASE("a view commits each job's scene to its compositor, and starts a job only where something changed") {
    Viewed page("<style>body { margin: 0 } #a { height: 10px }</style><div id=a></div>");
    PrHtmlViewUpdate first = prHtmlViewUpdate(page.view);
    CHECK(first.started);
    CHECK_FALSE(first.committed);
    PrHtmlViewUpdate committed = page.settle();
    CHECK(committed.committed);
    CHECK(committed.layout.blocksLaidOut != 0);
    CHECK_FALSE(committed.started);
    // Nothing changed, so nothing starts.
    PrHtmlViewUpdate idle = prHtmlViewUpdate(page.view);
    CHECK_FALSE(idle.started);
    CHECK_FALSE(idle.committed);
    prLayoutCompositorFrame(page.compositor);
    CHECK(page.presented == 1);

    // A change starts a job, whose scene the next update commits.
    prDomElementSetAttribute(page.byId("a"), "style", "height: 20px");
    PrHtmlViewUpdate changed = prHtmlViewUpdate(page.view);
    CHECK(changed.started);
    CHECK(changed.style.elementsStyled == 1);
    CHECK(page.settle().committed);
}

TEST_CASE("a view's viewport is the window's media environment and the extent the document is laid out in") {
    Viewed page("<style>body { margin: 0 } #a { height: 10px } @media (max-width: 500px) { #a { height: 30px } }"
        "</style><div id=a></div>");
    prHtmlViewUpdate(page.view);
    page.settle();
    CHECK(prCssElementGetBoundingClientRect(page.byId("a")).height == 10.0);
    CHECK(prCssElementGetBoundingClientRect(page.byId("a")).width == 800.0);
    prHtmlViewSetViewport(page.view, PrVec2{400, 300});
    CHECK(prHtmlViewUpdate(page.view).started);
    page.settle();
    CHECK(prCssElementGetBoundingClientRect(page.byId("a")).height == 30.0);
    CHECK(prCssElementGetBoundingClientRect(page.byId("a")).width == 400.0);
}

TEST_CASE("an inline box split by a block has its parts and its row as client rects, and bounds of those with area") {
    Viewed page("<style>body { margin: 0 } .w { width: 500px } b { display: inline-block; height: 1px }</style>"
        "<div class=w><span id=empty><b style='width: 100px'></b><div></div><b style='width: 200px'></b></span></div>"
        "<div class=w><span id=tall><b style='width: 100px'></b><div style='height: 100px'></div>"
        "<b style='width: 200px'></b></span></div><div class=w><span id=split>ab<div>c</div>d</span></div>"
        "<div class=w><span id=alone><div></div></span></div>");
    prHtmlViewUpdate(page.view);
    page.settle();
    // A row with no height widens no bounds; one with height does.
    CHECK(prCssElementGetBoundingClientRect(page.byId("empty")).width == 200.0);
    CHECK(prCssElementGetBoundingClientRect(page.byId("tall")).width == 500.0);
    // The part before the block, its row, and the part after, in content order. offsetTop is the first part's.
    PrDomRect rects[3]{};
    REQUIRE(prCssElementGetClientRects(page.byId("split"), rects, 3) == 3);
    CHECK(rects[1].width == 500.0);
    CHECK(rects[0].y < rects[1].y);
    CHECK(rects[1].y < rects[2].y);
    PrHtmlElement* split = prHtmlElementQuery(prDomNodeAsEventTarget(prDomElementAsNode(page.byId("split"))));
    CHECK(double(prHtmlElementOffsetTop(split)) == rects[0].y);
    // A box that holds only a block has an empty part on a line before its row and after it
    // (CSS2/#anonymous-block-level). If no rect has any area, the bounds are those of the first.
    PrDomRect alone[3]{};
    REQUIRE(prCssElementGetClientRects(page.byId("alone"), alone, 3) == 3);
    CHECK(alone[0].width == 0.0);
    CHECK(alone[1].width == 500.0);
    CHECK(alone[2].width == 0.0);
    PrDomRect bounds = prCssElementGetBoundingClientRect(page.byId("alone"));
    CHECK(bounds.x == alone[0].x);
    CHECK(bounds.width == 0.0);
    CHECK(bounds.height == 0.0);
}

TEST_CASE("client rects and hit tests follow a projected box, cut where it passes behind the eye") {
    Viewed page("<style>body { margin: 0 } #p { perspective: 40px; width: 40px; height: 40px }"
        "#c { width: 40px; height: 40px; transform: rotateY(30deg) }#q { perspective: 10px; width: 40px; height: 40px }"
        "#d { width: 40px; height: 40px; transform: rotateY(45deg) }</style>"
        "<div id=p><div id=c></div></div><div id=q><div id=d></div></div>");
    prHtmlViewUpdate(page.view);
    page.settle();
    // At 30deg, the left edge comes toward the eye at w = 0.75 and the right edge goes away at w = 1.25.
    PrDomRect turned = prCssElementGetBoundingClientRect(page.byId("c"));
    CHECK(turned.x == doctest::Approx(20 - 17.3205 / 0.75).epsilon(0.01));
    CHECK(turned.x + turned.width == doctest::Approx(20 + 17.3205 / 1.25).epsilon(0.01));
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page.page));
    CHECK(prCssDocumentElementFromPoint(document, 30, 20) == page.byId("c"));
    CHECK(prCssDocumentElementFromPoint(document, 36, 20) == page.byId("p"));
    // At a perspective of 10px, the left part of the card is behind the eye. Its rect is cut there, and is finite.
    PrDomRect cut = prCssElementGetBoundingClientRect(page.byId("d"));
    CHECK(cut.x < -1000.0);
    CHECK(cut.x > -1e6);
    CHECK(cut.x + cut.width == doctest::Approx(20 + 14.1421 / 2.4142).epsilon(0.01));
}

TEST_CASE("the offsets a compositor scrolls to reach the document at the view's next update") {
    Viewed page("<div style='height: 5000px'></div>");
    prHtmlViewUpdate(page.view);
    page.settle();
    CHECK(prLayoutCompositorScroll(page.compositor, 0, kPrLayoutNodeNone, prLayoutVecFromPx(PrVec2{0, 100})).y.raw ==
        prLayoutUnitFromPx(100).raw);
    // The scroll applies to the document immediately and needs no job, since no scene holds an offset.
    CHECK_FALSE(prHtmlViewUpdate(page.view).started);
    CHECK(prHtmlWindowScrollY(prHtmlPageWindow(page.page)) == 100.0);
}

TEST_CASE("the offsets a document scrolls itself to reach the compositor at the view's next update, with no job") {
    Viewed page("<div style='height: 5000px'></div>");
    prHtmlViewUpdate(page.view);
    page.settle();
    PrHtmlWindow* window = prHtmlPageWindow(page.page);
    PrCssScrollToOptions to{false, true, 0, 100};
    prHtmlWindowScroll(window, &to);
    CHECK_FALSE(prHtmlViewUpdate(page.view).started);
    // The compositor shows the document at 100: scrolling back to the top moves it all the way.
    CHECK(prLayoutCompositorScroll(page.compositor, 0, kPrLayoutNodeNone, prLayoutVecFromPx(PrVec2{0, -150})).y.raw ==
        prLayoutUnitFromPx(-100).raw);
}

TEST_CASE("a view routes the pointer to the elements under it") {
    Viewed page("<style>body { margin: 0 } div { height: 50px }</style><div id=a></div><div id=b></div>");
    prHtmlViewUpdate(page.view);
    page.settle();
    Heard heard;
    PrDomEventListener listener = recorder(&heard);
    PrDomEventTarget* a = prDomNodeAsEventTarget(prDomElementAsNode(page.byId("a")));
    for (const char* type : {"mousedown", "mouseup", "click"})
        prDomEventTargetAddEventListener(a, type, &listener, nullptr);

    // A move hovers at the view's next update.
    PrAppEvent event{.kind = PrAppEventKind_PointerMove, .pointer = {.position = PrVec2{10, 10}}};
    CHECK(prHtmlViewEvent(page.view, &event, 0) == PrHtmlViewEventResult_Handled);
    CHECK_FALSE(page.matches("a", ":hover"));
    prHtmlViewUpdate(page.view);
    CHECK(page.matches("a", ":hover"));
    event.pointer.position = PrVec2{10, 60};
    prHtmlViewEvent(page.view, &event, 0);
    prHtmlViewUpdate(page.view);
    CHECK_FALSE(page.matches("a", ":hover"));
    CHECK(page.matches("b", ":hover"));

    // A press and a release over one element click it; a release over another does not.
    event.pointer.position = PrVec2{10, 10};
    event.kind = PrAppEventKind_PointerDown;
    prHtmlViewEvent(page.view, &event, 0);
    CHECK(page.matches("a", ":active"));
    event.kind = PrAppEventKind_PointerUp;
    prHtmlViewEvent(page.view, &event, 0);
    CHECK_FALSE(page.matches("a", ":active"));
    event.kind = PrAppEventKind_PointerDown;
    prHtmlViewEvent(page.view, &event, 0);
    event.kind = PrAppEventKind_PointerUp;
    event.pointer.position = PrVec2{10, 60};
    prHtmlViewEvent(page.view, &event, 0);
    CHECK(heard.types == std::vector<std::string>{"mousedown", "mouseup", "click", "mousedown"});
}

TEST_CASE("a key fires keydown, a keypress for its text and keyup at the focused element, all trusted") {
    Viewed page("<div id=a tabindex=0></div>");
    HeardKeys heard;
    PrDomEventListener listener = keyRecorder(&heard);
    listenForKeys(page.byId("a"), &listener);
    prHtmlFocus(page.byId("a"), nullptr);
    send(page.view, press('A', PrAppKeyCode_KeyA, "a"));
    send(page.view, press('A', PrAppKeyCode_KeyA, "A", PrAppModifierFlag_Shift));
    CHECK(heard.events == std::vector<std::string>{
        "keydown a 65", "keypress a 97", "keyup a 65", "keydown A 65",
        "keypress A 65", "keyup A 65",
    });

    // A shortcut types no text, and a canceled keydown drops the text of its key.
    heard.events.clear();
    send(page.view, press('A', PrAppKeyCode_KeyA, "a", PrAppModifierFlag_Control));
    heard.cancelKeydown = true;
    send(page.view, press('1', PrAppKeyCode_Digit1, "1"));
    CHECK(heard.events == std::vector<std::string>{"keydown a 65", "keyup a 65", "keydown 1 49", "keyup 1 49"});
}

TEST_CASE("the first focusable element with autofocus is focused at the next rendering update, once") {
    Viewed page("<svg><rect id=plain autofocus width=10 height=10 /><rect id=rect autofocus tabindex=0 width=10 "
        "height=10 /></svg><input id=input autofocus>");
    PrDomNode* root = prDomDocumentAsNode(page.document());
    // The rendering update flushes the candidates, as the next frame of the window would.
    auto frame = [&] {
        prHtmlViewUpdate(page.view);
        page.settle();
        htmlFocusFlushAutofocus(page.document());
    };
    frame();
    // The rect without a tabindex is not focusable, and the input comes after the rect that is.
    CHECK(prHtmlActiveElement(root) == page.byId("rect"));
    // The candidates are flushed once. A later autofocus attribute focuses nothing.
    prHtmlBlur(page.byId("rect"));
    prDomElementSetAttribute(page.byId("input"), "autofocus", "");
    frame();
    CHECK(prHtmlActiveElement(root) != page.byId("input"));
}

TEST_CASE("typing in a textarea fires beforeinput and input around the edit") {
    EditedArea e("<textarea id=t>ab</textarea>");
    send(e.page.view, press('C', PrAppKeyCode_KeyC, "c"));
    CHECK(valueOf(e.area) == "abc");
    CHECK(e.heard.events == std::vector<std::string>{"beforeinput insertText [c]", "input insertText [c]"});
    CHECK(prHtmlTextControlSelectionStart(e.area) == 3);
}

TEST_CASE("a canceled beforeinput leaves a textarea's value as it was") {
    EditedArea e("<textarea id=t>ab</textarea>");
    e.heard.cancel = true;
    send(e.page.view, press('C', PrAppKeyCode_KeyC, "c"));
    CHECK(valueOf(e.area) == "ab");
    CHECK(e.heard.events == std::vector<std::string>{"beforeinput insertText [c]"});
}

TEST_CASE("Left and Right move as a right-to-left textarea shows its text") {
    for (const char* dir : {"rtl", "auto"}) {
        std::string html = std::string("<meta charset=utf-8><textarea id=t dir=") + dir +
            ">\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d</textarea>";
        EditedArea e(html.c_str());
        CHECK(prHtmlTextControlSelectionStart(e.area) == 4);
        send(e.page.view, press(PrAppKey_Right, PrAppKeyCode_ArrowRight, ""));
        CHECK(prHtmlTextControlSelectionStart(e.area) == 3);
        send(e.page.view, press(PrAppKey_Left, PrAppKeyCode_ArrowLeft, ""));
        CHECK(prHtmlTextControlSelectionStart(e.area) == 4);
        send(e.page.view, press(PrAppKey_Home, PrAppKeyCode_Home, ""));
        CHECK(prHtmlTextControlSelectionStart(e.area) == 0);
    }
}

TEST_CASE("the arrows run through a textarea's text in order, whatever its direction shows") {
    EditedArea e("<meta charset=utf-8><textarea id=t>abc \xd7\x90\xd7\x91\xd7\x92 def</textarea>");
    prHtmlTextControlSetSelectionRange(e.area, 3, 3, PrDomSelectionDirection_None);
    for (uint32_t expected = 4; expected <= 8; ++expected) {
        send(e.page.view, press(PrAppKey_Right, PrAppKeyCode_ArrowRight, ""));
        CHECK(prHtmlTextControlSelectionStart(e.area) == expected);
    }
    send(e.page.view, press(PrAppKey_Left, PrAppKeyCode_ArrowLeft, ""));
    CHECK(prHtmlTextControlSelectionStart(e.area) == 7);
}

TEST_CASE("each paragraph of a textarea with dir=auto decides which way its arrows go") {
    EditedArea e("<meta charset=utf-8><textarea id=t dir=auto>abc\n\xd7\x90\xd7\x91\xd7\x92</textarea>");
    prHtmlTextControlSetSelectionRange(e.area, 5, 5, PrDomSelectionDirection_None);
    send(e.page.view, press(PrAppKey_Right, PrAppKeyCode_ArrowRight, ""));
    CHECK(prHtmlTextControlSelectionStart(e.area) == 4);
    prHtmlTextControlSetSelectionRange(e.area, 1, 1, PrDomSelectionDirection_None);
    send(e.page.view, press(PrAppKey_Right, PrAppKeyCode_ArrowRight, ""));
    CHECK(prHtmlTextControlSelectionStart(e.area) == 2);
}

TEST_CASE("a masked right-to-left input moves as it shows, and a word motion goes to its end") {
    EditedArea e("<input id=t type=password dir=rtl value=abc>");
    prHtmlTextControlSetSelectionRange(e.area, 3, 3, PrDomSelectionDirection_None);
    send(e.page.view, press(PrAppKey_Right, PrAppKeyCode_ArrowRight, ""));
    CHECK(prHtmlTextControlSelectionStart(e.area) == 2);
    std::vector<PrAppEvent> word = press(PrAppKey_Right, PrAppKeyCode_ArrowRight, "");
    for (PrAppEvent& event : word)
        event.modifiers = PrAppModifierFlag_Alt;
    send(e.page.view, word);
    CHECK(prHtmlTextControlSelectionStart(e.area) == 0);
}

TEST_CASE("a textarea under a transform keeps its column between lines") {
    EditedArea e("<textarea id=t style='transform: translateX(50px) rotate(10deg) scale(2)'>abc\ndef\nghi</textarea>");
    prHtmlTextControlSetSelectionRange(e.area, 1, 1, PrDomSelectionDirection_None);
    send(e.page.view, press(PrAppKey_Down, PrAppKeyCode_ArrowDown, ""));
    CHECK(prHtmlTextControlSelectionStart(e.area) == 5);
    send(e.page.view, press(PrAppKey_Down, PrAppKeyCode_ArrowDown, ""));
    CHECK(prHtmlTextControlSelectionStart(e.area) == 9);
}

TEST_CASE("the arrows follow the lines of a vertical textarea") {
    EditedArea e("<textarea id=t style='writing-mode: vertical-rl'>abc\ndef</textarea>");
    CHECK(prHtmlTextControlSelectionStart(e.area) == 7);
    send(e.page.view, press(PrAppKey_Up, PrAppKeyCode_ArrowUp, ""));
    CHECK(prHtmlTextControlSelectionStart(e.area) == 6);
    send(e.page.view, press(PrAppKey_Right, PrAppKeyCode_ArrowRight, ""));
    CHECK(prHtmlTextControlSelectionStart(e.area) == 2);
    send(e.page.view, press(PrAppKey_Left, PrAppKeyCode_ArrowLeft, ""));
    CHECK(prHtmlTextControlSelectionStart(e.area) == 6);
}

TEST_CASE("Backspace in a textarea takes one code point, and an emoji sequence whole") {
    EditedArea hebrew("<meta charset=utf-8><textarea id=t>\xd7\xa9\xd6\xb8</textarea>");
    send(hebrew.page.view, press(PrAppKey_Backspace, PrAppKeyCode_Backspace, ""));
    CHECK(valueOf(hebrew.area) == "\xd7\xa9");
    EditedArea latin("<meta charset=utf-8><textarea id=t>e\xcc\x81</textarea>");
    send(latin.page.view, press(PrAppKey_Backspace, PrAppKeyCode_Backspace, ""));
    CHECK(valueOf(latin.area) == "e");
    EditedArea emoji("<meta charset=utf-8><textarea id=t>\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x92\xbb</textarea>");
    send(emoji.page.view, press(PrAppKey_Backspace, PrAppKeyCode_Backspace, ""));
    CHECK(valueOf(emoji.area) == "");
}

TEST_CASE("keys move in a textarea by grapheme cluster, and Enter inserts a line break") {
    EditedArea e("<meta charset=utf-8><textarea id=t>ae\xcc\x81z</textarea>");
    send(e.page.view, press(PrAppKey_Left, PrAppKeyCode_ArrowLeft, ""));
    CHECK(prHtmlTextControlSelectionStart(e.area) == 3);
    send(e.page.view, press(PrAppKey_Backspace, PrAppKeyCode_Backspace, ""));
    CHECK(valueOf(e.area) == "aez");
    send(e.page.view, press(PrAppKey_Enter, PrAppKeyCode_Enter, ""));
    CHECK(valueOf(e.area) == "ae\nz");
    send(e.page.view, press(PrAppKey_Delete, PrAppKeyCode_Delete, ""));
    CHECK(valueOf(e.area) == "ae\n");
}

TEST_CASE("undo reverts a run of typing in a textarea, and setting the value empties the history") {
    EditedArea e("<textarea id=t>a</textarea>");
    send(e.page.view, press('B', PrAppKeyCode_KeyB, "b"));
    send(e.page.view, press('C', PrAppKeyCode_KeyC, "c"));
    CHECK(valueOf(e.area) == "abc");
    send(e.page.view, press('Z', PrAppKeyCode_KeyZ, "", PrAppModifierFlag_Super));
    CHECK(valueOf(e.area) == "a");
    send(e.page.view, press('Z', PrAppKeyCode_KeyZ, "", PrAppModifierFlag_Super | PrAppModifierFlag_Shift));
    CHECK(valueOf(e.area) == "abc");

    PrHtmlTextAreaElement* area = prHtmlTextAreaElementQuery(prDomNodeAsEventTarget(prDomElementAsNode(e.area)));
    prHtmlTextAreaElementSetValue(area, "set");
    send(e.page.view, press('Z', PrAppKeyCode_KeyZ, "", PrAppModifierFlag_Super));
    CHECK(valueOf(e.area) == "set");
}

TEST_CASE("a composition marks its text in a textarea, and commits it once, ahead of compositionend") {
    EditedArea e("<textarea id=t>a</textarea>");
    PrAppEvent update{.kind = PrAppEventKind_CompositionUpdate, .input = {.text = "k"}};
    prHtmlViewEvent(e.page.view, &update, 0);
    CHECK(valueOf(e.area) == "ak");
    update.input.text = "\xe3\x81\x8b";
    prHtmlViewEvent(e.page.view, &update, 0);
    CHECK(valueOf(e.area) == "a\xe3\x81\x8b");
    PrAppEvent end{.kind = PrAppEventKind_CompositionEnd, .input = {.text = "\xe3\x81\x8b"}};
    prHtmlViewEvent(e.page.view, &end, 0);
    PrAppEvent committed{.kind = PrAppEventKind_Text, .input = {.text = "\xe3\x81\x8b"}};
    prHtmlViewEvent(e.page.view, &committed, 0);
    CHECK(valueOf(e.area) == "a\xe3\x81\x8b");
    CHECK(e.heard.events == std::vector<std::string>{
        "compositionstart",
        "compositionupdate",
        "beforeinput insertCompositionText [k] composing",
        "input insertCompositionText [k] composing",
        "compositionupdate",
        "beforeinput insertCompositionText [\xe3\x81\x8b] composing",
        "input insertCompositionText [\xe3\x81\x8b] composing",
        "beforeinput insertCompositionText [] composing",
        "input insertCompositionText [] composing",
        "beforeinput insertText [\xe3\x81\x8b]",
        "input insertText [\xe3\x81\x8b]",
        "compositionend",
    });
    // Only the commit is a step of the history.
    send(e.page.view, press('Z', PrAppKeyCode_KeyZ, "", PrAppModifierFlag_Super));
    CHECK(valueOf(e.area) == "a");
}

TEST_CASE("a drag in a textarea extends its selection from the press") {
    EditedArea e("<style>body { margin: 0 }</style><textarea id=t cols=40>abcdefghij</textarea>");
    e.page.settle();
    clickAt(e.page, PrVec2{6, 8});
    uint32_t start = prHtmlTextControlSelectionStart(e.area);
    PrAppEvent event{.kind = PrAppEventKind_PointerDown, .pointer = {.position = PrVec2{6, 8}}};
    prHtmlViewEvent(e.page.view, &event, 0);
    event.kind = PrAppEventKind_PointerMove;
    event.pointer.position = PrVec2{60, 8};
    prHtmlViewEvent(e.page.view, &event, 0);
    CHECK(prHtmlTextControlSelectionStart(e.area) == start);
    CHECK(prHtmlTextControlSelectionEnd(e.area) > start);
}

TEST_CASE("a text control's multi-click selects a word, a line, and the text") {
    EditedArea e("<style>body { margin: 0 }</style><textarea id=t cols=40>alpha beta\ngamma</textarea>");
    e.page.settle();
    for (uint32_t clicks : {2u, 3u, 4u}) {
        CAPTURE(clicks);
        PrAppEvent event{.kind = PrAppEventKind_PointerDown, .pointer = {.position = PrVec2{10, 8}, .clicks = 1}};
        prHtmlViewEvent(e.page.view, &event, 0);
        event.kind = PrAppEventKind_PointerUp;
        prHtmlViewEvent(e.page.view, &event, 0);
        event.kind = PrAppEventKind_PointerDown;
        event.pointer.clicks = clicks;
        prHtmlViewEvent(e.page.view, &event, 0);
        event.kind = PrAppEventKind_PointerUp;
        prHtmlViewEvent(e.page.view, &event, 0);
        CHECK(prHtmlTextControlSelectionStart(e.area) == 0);
        CHECK(prHtmlTextControlSelectionEnd(e.area) == (clicks == 2 ? 5 : clicks == 3 ? 10 : 16));
    }
}

TEST_CASE("the keys of a text control move by line and by word, and delete by word") {
    EditedArea e("<style>body { margin: 0 }</style><textarea id=t cols=40 rows=3>one two\nthree</textarea>");
    e.page.settle();
    auto key = [&](PrAppKey k, PrAppModifierFlags modifiers = 0) {
        PrAppEvent event{.kind = PrAppEventKind_KeyDown, .modifiers = modifiers, .keyboard = {.key = k}};
        prHtmlViewEvent(e.page.view, &event, 0);
        event.kind = PrAppEventKind_KeyUp;
        prHtmlViewEvent(e.page.view, &event, 0);
    };
    prHtmlTextControlSetSelectionRange(e.area, 0, 0, PrDomSelectionDirection_None);
    key(PrAppKey_Down);
    CHECK(prHtmlTextControlSelectionStart(e.area) == 8);
    key(PrAppKey_Up);
    CHECK(prHtmlTextControlSelectionStart(e.area) == 0);
    key(PrAppKey_Right, PrAppModifierFlag_Alt);
    CHECK(prHtmlTextControlSelectionStart(e.area) == 3);
    key(PrAppKey_Right, PrAppModifierFlag_Alt | PrAppModifierFlag_Shift);
    CHECK(prHtmlTextControlSelectionStart(e.area) == 3);
    CHECK(prHtmlTextControlSelectionEnd(e.area) == 7);
    key(PrAppKey_End, PrAppModifierFlag_Control);
    CHECK(prHtmlTextControlSelectionStart(e.area) == 13);
    key(PrAppKey_Backspace, PrAppModifierFlag_Alt);
    CHECK(valueOf(e.area) == "one two\n");
}

TEST_CASE("a view asks for frames until a selection that a press made is shown, with no caret to blink") {
    EditedArea e("<style>body { margin: 0 }</style><textarea id=t cols=40>alpha beta</textarea>");
    e.page.settle();
    for (uint32_t clicks : {1u, 2u}) {
        PrAppEvent event{.kind = PrAppEventKind_PointerDown, .pointer = {.position = PrVec2{10, 8}, .clicks = clicks}};
        prHtmlViewEvent(e.page.view, &event, 0);
        event.kind = PrAppEventKind_PointerUp;
        prHtmlViewEvent(e.page.view, &event, 0);
    }
    CHECK(prHtmlTextControlSelectionEnd(e.area) > prHtmlTextControlSelectionStart(e.area));
    // The press is a change, then a job, and then the job's scene, which shows the selection.
    CHECK(prHtmlViewOutput(e.page.view).wantsFrame);
    e.page.settle();
    CHECK(prHtmlViewOutput(e.page.view).wantsFrame);
    e.page.settle();
    CHECK(!prHtmlViewOutput(e.page.view).wantsFrame);
}

TEST_CASE("typing keeps the caret in view") {
    EditedArea e("<style>body { margin: 0 }</style><input id=t style='width: 40px'>");
    e.page.settle();
    e.page.settle();
    CHECK(prCssElementScrollLeft(e.area) == 0);
    for (char c : std::string("a long value that overflows the field")) {
        char text[2] = {c, 0};
        send(e.page.view, press(PrAppKey(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c), PrAppKeyCode_KeyA, text));
        e.page.settle();
        e.page.settle();
    }
    CHECK(prCssElementScrollLeft(e.area) > 0);
    // The caret stands at the end, within the rounding of the integer widths.
    CHECK(prCssElementScrollLeft(e.area) >= prCssElementScrollWidth(e.area) - prCssElementClientWidth(e.area) - 1);
}

TEST_CASE("a textarea scrolls to a caret that typing moves past its last line") {
    EditedArea e("<style>body { margin: 0 }</style><textarea id=t rows=2></textarea>");
    e.page.settle();
    e.page.settle();
    for (uint32_t i = 0; i < 8; ++i) {
        send(e.page.view, press(PrAppKey_Enter, PrAppKeyCode_Enter, ""));
        e.page.settle();
        e.page.settle();
    }
    CHECK(prCssElementScrollTop(e.area) > 0);
}

TEST_CASE("a host that paints the document itself shows a selection at once, with no update and no pointer move") {
    EditedArea e("<style>body { margin: 0 }</style><textarea id=t cols=40>alpha beta</textarea>");
    e.page.settle();
    e.page.settle();
    PrCssPresentation* presentation = prHtmlPagePresentation(e.page.page);
    PrLayoutNode text = prCssPresentationFieldText(presentation, e.area);
    PrLayoutTree* tree = prCssPresentationTree(presentation);
    REQUIRE(text != kPrLayoutNodeNone);
    for (uint32_t clicks : {1u, 2u}) {
        PrAppEvent event{.kind = PrAppEventKind_PointerDown, .pointer = {.position = PrVec2{10, 8}, .clicks = clicks}};
        prHtmlViewEvent(e.page.view, &event, 0);
        event.kind = PrAppEventKind_PointerUp;
        prHtmlViewEvent(e.page.view, &event, 0);
    }
    REQUIRE(prHtmlTextControlSelectionEnd(e.area) == 5);
    CHECK(prLayoutNodeHighlights(tree, text, nullptr, 0) == 0);
    PrDrawList* list = prDrawListCreate();
    prHtmlViewPaint(e.page.view, list);
    prDrawListRelease(list);
    CHECK(prLayoutNodeHighlights(tree, text, nullptr, 0) == 1);
    CHECK(!prHtmlViewOutput(e.page.view).wantsFrame);
}

TEST_CASE("a caret asks for a frame when it next blinks, and comes into view as it moves") {
    EditedArea e("<style>body { margin: 0 }</style><input id=t style='width: 40px' value='a long value that overflows'>");
    e.page.settle();
    CHECK(prHtmlViewOutput(e.page.view).frameTime != 0);
    PrAppEvent event{.kind = PrAppEventKind_KeyDown, .keyboard = {.key = PrAppKey_End}};
    prHtmlViewEvent(e.page.view, &event, 0);
    e.page.settle();
    e.page.settle();
    CHECK(prCssElementScrollLeft(e.area) > 0);
    prHtmlTextControlSelect(e.area);
    e.page.settle();
    e.page.settle();
    CHECK(!prHtmlViewOutput(e.page.view).wantsFrame);
    CHECK(prHtmlViewOutput(e.page.view).frameTime == 0);
}

TEST_CASE("an empty control shows its placeholder, until it has a value") {
    EditedArea e("<textarea id=t placeholder='Say something'></textarea>");
    e.page.settle();
    PrCssPresentation* presentation = prHtmlPagePresentation(e.page.page);
    PrLayoutNode text = prCssPresentationFieldText(presentation, e.area);
    REQUIRE(text != kPrLayoutNodeNone);
    CHECK(prLayoutNodeText(prCssPresentationTree(presentation), text) == "Say something");
    send(e.page.view, press('H', PrAppKeyCode_KeyH, "h"));
    e.page.settle();
    text = prCssPresentationFieldText(presentation, e.area);
    CHECK(prLayoutNodeText(prCssPresentationTree(presentation), text) == "h");
}

TEST_CASE("a password field shows a mark for each code point, unless input-security is none") {
    EditedArea e("<meta charset=utf-8><input id=t type=password value='h\xc3\xa9y'>");
    e.page.settle();
    PrCssPresentation* presentation = prHtmlPagePresentation(e.page.page);
    PrLayoutNode text = prCssPresentationFieldText(presentation, e.area);
    REQUIRE(text != kPrLayoutNodeNone);
    CHECK(prLayoutNodeText(prCssPresentationTree(presentation), text) == "\xe2\x80\xa2\xe2\x80\xa2\xe2\x80\xa2");
    CHECK(prCssPresentationFieldMasked(presentation, e.area));
    CHECK(prHtmlViewOutput(e.page.view).hasTextInput);
    EditedArea plain("<style>input { input-security: none }</style><input id=t type=password value=hey>");
    plain.page.settle();
    presentation = prHtmlPagePresentation(plain.page.page);
    text = prCssPresentationFieldText(presentation, plain.area);
    CHECK(prLayoutNodeText(prCssPresentationTree(presentation), text) == "hey");
}

TEST_CASE("::selection colors what a text control has selected") {
    EditedArea e("<style>textarea::selection { color: rgb(255, 0, 0); background-color: rgb(0, 0, 255) }</style>"
        "<textarea id=t>abc</textarea>");
    e.page.settle();
    PrColor16 color{};
    PrColor16 background{};
    prCssPresentationSelectionColors(prHtmlPagePresentation(e.page.page), e.area, &color, &background);
    PrColor red = prColorFromColor16(color);
    PrColor blue = prColorFromColor16(background);
    CHECK(red.r == doctest::Approx(1));
    CHECK(red.b == doctest::Approx(0));
    CHECK(blue.b == doctest::Approx(1));
    CHECK(blue.r == doctest::Approx(0));
}

TEST_CASE("cut, copy and paste use the clipboard, and fire events that can take them over") {
    static std::string clipboard;
    static uint32_t requested;
    PrApp app{};
    app.setClipboardText = [](PrApp*, PrStringView text) -> PrStatus {
        clipboard = std::string(text.data, text.size);
        return PrStatus_Ok;
    };
    app.requestClipboardText = [](PrApp*) { ++requested; };
    EditedArea e("<textarea id=t>one two</textarea>");
    prHtmlViewSetApp(e.page.view, &app);
    e.page.settle();
    auto command = [&](PrAppKey key) {
        PrAppEvent event{.kind = PrAppEventKind_KeyDown, .modifiers = PrAppModifierFlag_Super, .keyboard = {.key = key}};
        prHtmlViewEvent(e.page.view, &event, 0);
        event.kind = PrAppEventKind_KeyUp;
        prHtmlViewEvent(e.page.view, &event, 0);
    };
    prHtmlTextControlSetSelectionRange(e.area, 0, 3, PrDomSelectionDirection_None);
    command('C');
    CHECK(clipboard == "one");
    CHECK(valueOf(e.area) == "one two");
    command('X');
    CHECK(clipboard == "one");
    CHECK(valueOf(e.area) == " two");
    prHtmlTextControlSetSelectionRange(e.area, 0, 0, PrDomSelectionDirection_None);
    uint32_t before = requested;
    command('V');
    CHECK(requested == before + 1);
    PrAppEvent arrived{.kind = PrAppEventKind_Clipboard, .input = {.text = "pasted"}};
    prHtmlViewEvent(e.page.view, &arrived, 0);
    CHECK(valueOf(e.area) == "pasted two");

    // A listener takes over a copy by canceling it and setting the event's data, and refuses a paste by canceling it.
    PrDomEventListener takeOver{
        [](void*, PrDomEvent* event) {
            if (PrDomClipboardEvent* fired = prDomClipboardEventQuery(event))
                prDomDataTransferSetText(prDomClipboardEventClipboardData(fired), "theirs");
            prDomEventPreventDefault(event);
        },
        nullptr,
        0,
        nullptr,
        nullptr,
    };
    PrDomEventTarget* target = prDomNodeAsEventTarget(prDomElementAsNode(e.area));
    prDomEventTargetAddEventListener(target, "copy", &takeOver, nullptr);
    prDomEventTargetAddEventListener(target, "cut", &takeOver, nullptr);
    prDomEventTargetAddEventListener(target, "paste", &takeOver, nullptr);
    prHtmlTextControlSetSelectionRange(e.area, 0, 6, PrDomSelectionDirection_None);
    command('C');
    CHECK(clipboard == "theirs");
    command('X');
    CHECK(valueOf(e.area) == "pasted two");
    command('V');
    prHtmlViewEvent(e.page.view, &arrived, 0);
    CHECK(valueOf(e.area) == "pasted two");
}

TEST_CASE("a read-only textarea takes no text") {
    EditedArea e("<textarea id=t readonly>ab</textarea>");
    send(e.page.view, press('C', PrAppKeyCode_KeyC, "c"));
    CHECK(valueOf(e.area) == "ab");
}

TEST_CASE("a textarea shows its value, not its children, and the view reports where its caret stands") {
    EditedArea e("<style>body { margin: 0 }</style><textarea id=t rows=3>ab</textarea>");
    PrHtmlTextAreaElement* area = prHtmlTextAreaElementQuery(prDomNodeAsEventTarget(prDomElementAsNode(e.area)));
    e.page.settle();
    prHtmlTextAreaElementSetValue(area, "changed");
    e.page.settle();
    PrCssPresentation* presentation = prHtmlPagePresentation(e.page.page);
    PrLayoutNode text = prCssPresentationFieldText(presentation, e.area);
    REQUIRE(text != kPrLayoutNodeNone);
    CHECK(prLayoutNodeText(prCssPresentationTree(presentation), text) == "changed");
    e.page.settle();
    PrRect rect{};
    PrAppOutput output = prHtmlViewOutput(e.page.view);
    CHECK(output.hasTextInput);
    rect = output.textInput;
    CHECK(rect.left > 0);
}

TEST_CASE("without a focused element, keys go to the body") {
    Viewed page("<body id=body>");
    HeardKeys heard;
    PrDomEventListener listener = keyRecorder(&heard);
    listenForKeys(page.byId("body"), &listener);
    send(page.view, press(PrAppKey_Escape, PrAppKeyCode_Escape, ""));
    CHECK(heard.events == std::vector<std::string>{"keydown Escape 27", "keyup Escape 27"});
}

TEST_CASE("Enter activates a focused button or link as it goes down, and Space as it is released") {
    Viewed page("<button id=b></button>");
    HeardKeys heard;
    PrDomEventListener listener = keyRecorder(&heard);
    listenForKeys(page.byId("b"), &listener);
    prHtmlFocus(page.byId("b"), nullptr);
    send(page.view, press(PrAppKey_Enter, PrAppKeyCode_Enter, ""));
    CHECK(heard.events == std::vector<std::string>{"keydown Enter 13", "keypress Enter 13", "click", "keyup Enter 13"});

    heard.events.clear();
    std::vector<PrAppEvent> space = press(' ', PrAppKeyCode_Space, " ");
    send(page.view, {space[0], space[1]});
    CHECK(page.matches("b", ":active"));
    send(page.view, {space[2]});
    CHECK_FALSE(page.matches("b", ":active"));
    CHECK(heard.events == std::vector<std::string>{"keydown   32", "keypress   32", "keyup   32", "click"});

    // A canceled keydown activates nothing.
    heard.events.clear();
    heard.cancelKeydown = true;
    send(page.view, press(PrAppKey_Enter, PrAppKeyCode_Enter, ""));
    send(page.view, press(' ', PrAppKeyCode_Space, " "));
    CHECK(heard.events == std::vector<std::string>{"keydown Enter 13", "keyup Enter 13", "keydown   32", "keyup   32"});
}

TEST_CASE("Tab past the last area leaves the document with the focus at the viewport, and the next Tab starts again") {
    Viewed page("<button id=a></button><button id=b tabindex=-1></button><button id=c></button>");
    prHtmlViewUpdate(page.view);
    page.settle();
    PrDomNode* document = prDomDocumentAsNode(page.document());
    PrAppEvent tab{.kind = PrAppEventKind_KeyDown, .keyboard = {.key = PrAppKey_Tab, .code = PrAppKeyCode_Tab}};
    CHECK(prHtmlViewEvent(page.view, &tab, 0) == PrHtmlViewEventResult_Handled);
    CHECK(prHtmlActiveElement(document) == page.byId("a"));
    CHECK(page.matches("a", ":focus-visible"));
    CHECK(prHtmlViewEvent(page.view, &tab, 0) == PrHtmlViewEventResult_Handled);
    CHECK(prHtmlActiveElement(document) == page.byId("c"));
    CHECK(prHtmlViewEvent(page.view, &tab, 0) == PrHtmlViewEventResult_FocusLeft);
    CHECK(prHtmlActiveElement(document) == prDomDocumentBody(page.document()));
    CHECK(prHtmlViewEvent(page.view, &tab, 0) == PrHtmlViewEventResult_Handled);
    CHECK(prHtmlActiveElement(document) == page.byId("a"));

    // Shift+Tab goes back, and past the first area leaves the document the other way.
    tab.modifiers = PrAppModifierFlag_Shift;
    CHECK(prHtmlViewEvent(page.view, &tab, 0) == PrHtmlViewEventResult_FocusLeft);
    CHECK(prHtmlViewEvent(page.view, &tab, 0) == PrHtmlViewEventResult_Handled);
    CHECK(prHtmlActiveElement(document) == page.byId("c"));
}

TEST_CASE("a press focuses the nearest focusable ancestor, or the viewport, and shows focus only for typing") {
    Viewed page("<style>body { margin: 0 } div, input { display: block; height: 20px }</style>"
        "<div id=box tabindex=0><span id=inner>text</span></div><input id=field><div id=plain></div>");
    prHtmlViewUpdate(page.view);
    page.settle();
    PrDomNode* document = prDomDocumentAsNode(page.document());
    PrAppEvent down{.kind = PrAppEventKind_PointerDown, .pointer = {.position = PrVec2{5, 10}}};
    PrAppEvent up = down;
    up.kind = PrAppEventKind_PointerUp;
    send(page.view, {down, up});
    CHECK(prHtmlActiveElement(document) == page.byId("box"));
    CHECK_FALSE(page.matches("box", ":focus-visible"));

    // A key shows the focus that a press moved, and a modifier key alone does not.
    PrAppEvent shift{
        .kind = PrAppEventKind_KeyDown,
        .keyboard = {.key = PrAppKey_Shift, .code = PrAppKeyCode_ShiftLeft},
    };
    prHtmlViewEvent(page.view, &shift, 0);
    CHECK_FALSE(page.matches("box", ":focus-visible"));
    send(page.view, press(PrAppKey_Escape, PrAppKeyCode_Escape, ""));
    CHECK(page.matches("box", ":focus-visible"));

    down.pointer.position = up.pointer.position = PrVec2{5, 30};
    send(page.view, {down, up});
    CHECK(prHtmlActiveElement(document) == page.byId("field"));
    CHECK(page.matches("field", ":focus-visible"));

    down.pointer.position = up.pointer.position = PrVec2{5, 50};
    send(page.view, {down, up});
    CHECK(prHtmlActiveElement(document) == prDomDocumentBody(page.document()));
}

TEST_CASE("the first autofocus candidate that is focusable takes the focus at the next rendering update, and shows it") {
    Viewed page("<input id=hidden autofocus hidden><input id=first autofocus><input id=second autofocus>");
    PrDomNode* document = prDomDocumentAsNode(page.document());
    // The rendering update runs at the next rendering opportunity, on the agent's clock.
    while (prHtmlActiveElement(document) == prDomDocumentBody(page.document()) && page.runNextTask()) {
    }
    CHECK(prHtmlActiveElement(document) == page.byId("first"));
    CHECK(page.matches("first", ":focus-visible"));
}

TEST_CASE("an autofocus candidate that becomes focusable before the rendering update is focused") {
    Viewed page("<textarea id=t autofocus disabled></textarea><select id=s autofocus></select>");
    prDomElementRemoveAttribute(page.byId("t"), "disabled");
    PrDomNode* document = prDomDocumentAsNode(page.document());
    while (prHtmlActiveElement(document) == prDomDocumentBody(page.document()) && page.runNextTask()) {
    }
    CHECK(prHtmlActiveElement(document) == page.byId("t"));
}

TEST_CASE("a text field is sized in characters by its size attribute, one line tall, and an author's width wins") {
    Viewed page("<style>input { border: 0; padding: 0; line-height: 20px }</style>"
        "<input id=plain><input id=five size=5><input id=one size=1><input id=wide style='width: 300px'>"
        "<input id=box type=checkbox><input id=sized size=5 style='field-sizing: content'>");
    auto rect = [&](const char* id) { return prCssElementGetBoundingClientRect(page.byId(id)); };
    double average = (rect("plain").width - rect("one").width) / 19;
    CHECK(average > 0);
    CHECK(rect("five").width == doctest::Approx(rect("one").width + 4 * average).epsilon(0.01));
    CHECK(rect("plain").height == doctest::Approx(20));
    CHECK(rect("wide").width == doctest::Approx(300));
    CHECK(rect("box").width != doctest::Approx(rect("plain").width));
    // A field sized by its content is not sized by its characters.
    CHECK(rect("sized").width < rect("one").width);

    // A change of the size attribute sizes the field again.
    prDomElementSetAttribute(page.byId("five"), "size", "1");
    CHECK(rect("five").width == doctest::Approx(rect("one").width));
}

TEST_CASE("keys scroll the nearest scroll container that can move their way, and the viewport past it") {
    Viewed page("<style>body { margin: 0 } #s { overflow: auto; height: 100px } #s div { height: 300px }</style>"
        "<div id=s tabindex=0><div></div></div><div style='height: 5000px'></div>");
    prHtmlViewUpdate(page.view);
    page.settle();
    PrHtmlWindow* window = prHtmlPageWindow(page.page);
    prHtmlFocus(page.byId("s"), nullptr);
    send(page.view, press(PrAppKey_Down, PrAppKeyCode_ArrowDown, ""));
    CHECK(prCssElementScrollTop(page.byId("s")) == 40.0);
    // End moves the container to its end, and once it is there, the viewport.
    send(page.view, press(PrAppKey_End, PrAppKeyCode_End, ""));
    CHECK(prCssElementScrollTop(page.byId("s")) == 200.0);
    CHECK(prHtmlWindowScrollY(window) == 0.0);
    send(page.view, press(PrAppKey_PageDown, PrAppKeyCode_PageDown, ""));
    CHECK(prHtmlWindowScrollY(window) == 600.0 - 40.0);
    // The compositor takes the offsets that the keys scrolled to, with no new scene, and wants a frame to show them.
    for (PrHtmlViewUpdate update = page.settle(); update.committed || update.started; update = page.settle())
        continue;
    prLayoutCompositorFrame(page.compositor);
    uint32_t wanted = page.wanted;
    send(page.view, press(PrAppKey_Up, PrAppKeyCode_ArrowUp, ""));
    CHECK_FALSE(prHtmlViewUpdate(page.view).committed);
    CHECK(page.wanted == wanted + 1);
    CHECK(prCssElementScrollTop(page.byId("s")) == 160.0);
    CHECK(prLayoutCompositorScroll(page.compositor, 0, kPrLayoutNodeNone, prLayoutVecFromPx(PrVec2{0, -1000})).y.raw ==
        prLayoutUnitFromPx(-560).raw);
}

TEST_CASE("the wheel fires at the element under the pointer and scrolls from its box unless a listener cancels it") {
    Viewed page("<style>body { margin: 0 } #s { overflow: auto; height: 100px } #s div { height: 300px }</style>"
        "<div id=s><div id=in></div></div><div style='height: 5000px'></div>");
    prHtmlViewUpdate(page.view);
    page.settle();
    HeardWheels heard;
    PrDomEventListener listener = wheelRecorder(&heard);
    PrDomAddEventListenerOptions blocking{};
    PrDomEventTarget* s = prDomNodeAsEventTarget(prDomElementAsNode(page.byId("s")));
    prDomEventTargetAddEventListener(s, "wheel", &listener, &blocking);

    // The deltas are in pixels, and a wheel's lines are 40 pixels each.
    PrAppEvent event = wheelAt(PrVec2{10, 10}, 30, 0);
    CHECK(prHtmlViewEvent(page.view, &event, 0) == PrHtmlViewEventResult_Handled);
    CHECK(prCssElementScrollTop(page.byId("s")) == 30.0);
    event.scroll.unit = PrAppScrollUnit_Line;
    event.scroll.delta = PrVec2{0, 1};
    prHtmlViewEvent(page.view, &event, 0);
    CHECK(prCssElementScrollTop(page.byId("s")) == 70.0);
    CHECK(heard.events == std::vector<std::string>{"in 30 cancelable", "in 40"});

    // A listener that cancels the first wheel of a sequence keeps the scroll from happening, and may cancel the rest.
    heard.events.clear();
    heard.cancel = true;
    event = wheelAt(PrVec2{10, 20}, 30, 1);
    CHECK(prHtmlViewEvent(page.view, &event, 0) == PrHtmlViewEventResult_Canceled);
    event.time = 1.1;
    CHECK(prHtmlViewEvent(page.view, &event, 0) == PrHtmlViewEventResult_Canceled);
    CHECK(prCssElementScrollTop(page.byId("s")) == 70.0);
    CHECK(heard.events == std::vector<std::string>{"in 30 cancelable", "in 30 cancelable"});

    // A wheel that the host scrolled already fires a wheel event that cannot be canceled, and scrolls nothing again.
    heard.events.clear();
    event = wheelAt(PrVec2{10, 30}, 30, 2);
    CHECK(prHtmlViewEvent(page.view, &event, PrHtmlViewEventFlag_Scrolled) == PrHtmlViewEventResult_Handled);
    CHECK(prCssElementScrollTop(page.byId("s")) == 70.0);
    CHECK(heard.events == std::vector<std::string>{"in 30"});
}

TEST_CASE("a wheel sequence keeps its first target, and a passive listener cannot cancel a wheel") {
    Viewed page("<style>body { margin: 0 } div { height: 50px }</style><div id=a></div><div id=b></div>"
        "<div style='height: 5000px'></div>");
    prHtmlViewUpdate(page.view);
    page.settle();
    HeardWheels heard;
    PrDomEventListener listener = wheelRecorder(&heard);
    // A listener on the window is passive unless it says otherwise.
    PrDomEventTarget* window = prHtmlWindowAsEventTarget(prHtmlPageWindow(page.page));
    prDomEventTargetAddEventListener(window, "wheel", &listener, nullptr);
    HeardWheels ignored;
    PrDomEventListener blocker = wheelRecorder(&ignored);
    PrDomAddEventListenerOptions blocking{};
    PrDomEventTarget* a = prDomNodeAsEventTarget(prDomElementAsNode(page.byId("a")));
    prDomEventTargetAddEventListener(a, "wheel", &blocker, &blocking);

    // The sequence that starts over a keeps it as the document scrolls b under the pointer. Once no listener canceled
    // its first wheel, the rest of it cannot be canceled.
    for (double time : {0.0, 0.1, 0.2}) {
        PrAppEvent event = wheelAt(PrVec2{10, 40}, 10, time);
        prHtmlViewEvent(page.view, &event, 0);
        prHtmlViewUpdate(page.view);
        page.settle();
    }
    CHECK(prHtmlWindowScrollY(prHtmlPageWindow(page.page)) == 30.0);
    CHECK(heard.events == std::vector<std::string>{"a 10 cancelable", "a 10", "a 10"});

    // After a pause, the next sequence starts at what is under the pointer, whose listeners are passive.
    heard.events.clear();
    PrAppEvent event = wheelAt(PrVec2{10, 40}, -10, 0.2 + kPrLayoutWheelSequenceGap / 1000.0 + 0.1);
    prHtmlViewEvent(page.view, &event, 0);
    CHECK(heard.events == std::vector<std::string>{"b -10"});
}

TEST_CASE("a wheel listener that can cancel puts its element's box, or the whole viewport, in the wheel region") {
    Viewed page("<style>body { margin: 0 } div { height: 50px }</style><div id=a></div><div id=b></div>");
    prHtmlViewUpdate(page.view);
    page.settle();
    HeardWheels heard;
    PrDomEventListener listener = wheelRecorder(&heard);
    PrDomEventTarget* a = prDomNodeAsEventTarget(prDomElementAsNode(page.byId("a")));
    auto settled = [&] {
        for (PrHtmlViewUpdate update = page.settle(); update.committed || update.started; update = page.settle())
            continue;
    };
    // The compositor's clock stands still, so each query below is at another point, which starts a new sequence.
    CHECK_FALSE(prLayoutCompositorWheelBlocks(page.compositor, prLayoutVecFromPx(PrVec2{10, 10})));
    PrDomAddEventListenerOptions passive{.passive = true};
    prDomEventTargetAddEventListener(a, "wheel", &listener, &passive);
    settled();
    CHECK_FALSE(prLayoutCompositorWheelBlocks(page.compositor, prLayoutVecFromPx(PrVec2{10, 11})));
    HeardWheels other;
    PrDomEventListener blocker = wheelRecorder(&other);
    PrDomAddEventListenerOptions blocking{};
    prDomEventTargetAddEventListener(a, "wheel", &blocker, &blocking);
    settled();
    CHECK(prLayoutCompositorWheelBlocks(page.compositor, prLayoutVecFromPx(PrVec2{10, 10})));
    CHECK_FALSE(prLayoutCompositorWheelBlocks(page.compositor, prLayoutVecFromPx(PrVec2{10, 60})));
    PrDomEventListenerOptions capture = prDomEventListenerOptionsDefault();
    prDomEventTargetRemoveEventListener(a, "wheel", &blocker, &capture);
    settled();
    CHECK_FALSE(prLayoutCompositorWheelBlocks(page.compositor, prLayoutVecFromPx(PrVec2{10, 11})));

    // A listener on the window that says it is not passive makes the wheel wait anywhere.
    PrDomEventTarget* window = prHtmlWindowAsEventTarget(prHtmlPageWindow(page.page));
    prDomEventTargetAddEventListener(window, "wheel", &blocker, &blocking);
    settled();
    CHECK(prLayoutCompositorWheelBlocks(page.compositor, prLayoutVecFromPx(PrVec2{10, 60})));
}

// A page whose iframe, 30 by 20 at (10, 10) with no border, holds a document 100 pixels tall, `inner` sets its root's
// style. The view lays out the frame's document in the frame's content box.
struct ViewedFrame : Viewed {
    explicit ViewedFrame(const char* inner) :
            Viewed((std::string("<style>body { margin: 0; height: 2000px } iframe { display: block; margin: 10px; "
                "border: 0; width: 30px; height: 20px }</style><iframe id=f srcdoc=\"<style>html { ") +
                inner + " } body { margin: 0 } div { height: 100px }</style><div id=d></div>\"></iframe>") .c_str()) {
        for (uint32_t i = 0; i < 6; ++i) {
            prHtmlViewUpdate(view);
            prHtmlViewWait(view);
        }
        prHtmlViewUpdate(view);
    }

    PrHtmlWindow* frameWindow() {
        return prHtmlIFrameElementContentWindow(
            prHtmlIFrameElementQuery(prDomNodeAsEventTarget(prDomElementAsNode(byId("f")))));
    }

    PrDomElement* inner(const char* id) {
        PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(prHtmlWindowDocument(frameWindow())), id);
        REQUIRE(element != nullptr);
        return element;
    }
};

TEST_CASE("the pointer reaches the elements of a frame's document, in that document's coordinates") {
    ViewedFrame page("");
    std::vector<std::string> heard;
    PrDomEventListener listener{
        [](void* context, PrDomEvent* event) {
            PrDomMouseEventInit mouse = prDomMouseEventMembers(prDomMouseEventQuery(event));
            PrStringView type = prDomEventType(event);
            static_cast<std::vector<std::string>*>(context)->push_back(std::string(type.data, type.size) + " " +
                std::to_string(int(mouse.clientX)) + "," + std::to_string(int(mouse.clientY)));
        },
        &heard,
        1,
        nullptr,
        nullptr,
    };
    PrDomEventTarget* d = prDomNodeAsEventTarget(prDomElementAsNode(page.inner("d")));
    prDomEventTargetAddEventListener(d, "mousedown", &listener, nullptr);
    PrAppEvent event{.kind = PrAppEventKind_PointerMove, .pointer = {.position = PrVec2{15, 20}}};
    prHtmlViewEvent(page.view, &event, 0);
    prHtmlViewUpdate(page.view);
    // The element under the pointer and the frame that holds its document both match :hover.
    bool hovered = false;
    REQUIRE(prCssElementMatches(page.inner("d"), ":hover", &hovered) == PrStatus_Ok);
    CHECK(hovered);
    CHECK(page.matches("f", ":hover"));
    event.kind = PrAppEventKind_PointerDown;
    prHtmlViewEvent(page.view, &event, 0);
    CHECK(heard == std::vector<std::string>{"mousedown 5,10"});
}

TEST_CASE("the wheel scrolls a frame's document, then the page around it, unless the frame's root contains the scroll") {
    for (bool contain : {false, true}) {
        ViewedFrame page(contain ? "overscroll-behavior-y: contain" : "");
        PrHtmlWindow* window = prHtmlPageWindow(page.page);
        PrAppEvent event = wheelAt(PrVec2{15, 20}, 100, 0);
        prHtmlViewEvent(page.view, &event, 0);
        // The frame's document scrolls as far as it can: 100 less its viewport's 20.
        CHECK(prHtmlWindowScrollY(page.frameWindow()) == 80.0);
        CHECK(prHtmlWindowScrollY(window) == (contain ? 0.0 : 20.0));
    }
}

TEST_CASE("keys scroll the focused frame's document, then the page around it") {
    ViewedFrame page("");
    prHtmlFocus(page.byId("f"), nullptr);
    for (uint32_t i = 0; i < 3; ++i)
        send(page.view, press(PrAppKey_Down, PrAppKeyCode_ArrowDown, ""));
    // Two lines of 40 take the frame's document to its end at 80, and the third scrolls the page.
    CHECK(prHtmlWindowScrollY(page.frameWindow()) == 80.0);
    CHECK(prHtmlWindowScrollY(prHtmlPageWindow(page.page)) == 40.0);
}

TEST_CASE("a composition fires its events, its keys report Process, and its committed text fires no keypress") {
    Viewed page("<div id=a tabindex=0></div>");
    HeardKeys heard;
    PrDomEventListener listener = keyRecorder(&heard);
    listenForKeys(page.byId("a"), &listener);
    prHtmlFocus(page.byId("a"), nullptr);
    std::vector<PrAppEvent> k = press('K', PrAppKeyCode_KeyK, "k");
    PrAppEvent update{.kind = PrAppEventKind_CompositionUpdate, .input = {.text = "k"}};
    PrAppEvent end{.kind = PrAppEventKind_CompositionEnd, .input = {.text = "\xe3\x81\x8b"}};
    PrAppEvent commit{.kind = PrAppEventKind_Text, .input = {.text = "\xe3\x81\x8b"}};
    send(page.view, {k[0], update, k[2]});
    std::vector<PrAppEvent> a = press('A', PrAppKeyCode_KeyA, "a");
    update.input.text = "\xe3\x81\x8b";
    send(page.view, {a[0], update, end, commit, a[2]});
    CHECK(heard.events == std::vector<std::string>{
        "keydown k 75", "compositionstart ", "compositionupdate k", "keyup k 75 composing",
        "keydown Process 229 composing", "compositionupdate \xe3\x81\x8b", "compositionend \xe3\x81\x8b",
        "keyup a 65",
    });
}

TEST_CASE("the moves since the view's last update hover once, at the latest") {
    Viewed page("<style>body { margin: 0 } div { height: 50px }</style><div id=a></div><div id=b></div>");
    prHtmlViewUpdate(page.view);
    page.settle();
    PrAppEvent move{.kind = PrAppEventKind_PointerMove};
    for (float y : {10.0f, 20.0f, 30.0f, 60.0f}) {
        move.pointer.position = PrVec2{10, y};
        prHtmlViewEvent(page.view, &move, 0);
    }
    prHtmlViewUpdate(page.view);
    CHECK_FALSE(page.matches("a", ":hover"));
    CHECK(page.matches("b", ":hover"));
}

TEST_CASE("a scroll the compositor made hovers what comes under a pointer that has not moved") {
    Viewed page("<style>body { margin: 0 } div { height: 50px }</style><div id=a></div><div id=b></div>"
        "<div style='height: 5000px'></div>");
    prHtmlViewUpdate(page.view);
    page.settle();
    PrAppEvent move{.kind = PrAppEventKind_PointerMove};
    move.pointer.position = PrVec2{10, 10};
    prHtmlViewEvent(page.view, &move, 0);
    prHtmlViewUpdate(page.view);
    CHECK(page.matches("a", ":hover"));
    prLayoutCompositorScroll(page.compositor, 0, kPrLayoutNodeNone, prLayoutVecFromPx(PrVec2{0, 50}));
    prHtmlViewUpdate(page.view);
    page.settle();
    prHtmlViewUpdate(page.view);
    CHECK(page.matches("b", ":hover"));
}

TEST_CASE("a pointer that moves while the layout thread lays out hovers as the view next updates") {
    Viewed page("<style>body { margin: 0 } div { height: 50px }</style><div id=a></div><div id=b></div>");
    prHtmlViewUpdate(page.view);
    page.settle();
    prDomElementSetAttribute(page.byId("b"), "style", "height: 60px");
    CHECK(prHtmlViewUpdate(page.view).started);
    PrAppEvent move{.kind = PrAppEventKind_PointerMove};
    move.pointer.position = PrVec2{10, 10};
    prHtmlViewEvent(page.view, &move, 0);
    page.settle();
    CHECK(page.matches("a", ":hover"));
}

TEST_CASE("the cursor at the pointer is its element's, and auto shows the text cursor over selectable or editable text") {
    Viewed page("<style>body { margin: 0; font: 20px Tinos } div, input, button { display: block; height: 50px;"
        " width: 200px; margin: 0; padding: 0; border: 0 }</style>"
        "<div id=text>Words</div><div style='cursor: url(hand.png) 2 2, help'></div>"
        "<div><a href=x>Link</a></div><div style='user-select: none'><span>Fixed</span></div><input>"
        "<div style='writing-mode: vertical-rl'>Up</div>");
    prHtmlViewSetViewport(page.view, PrVec2{400, 400});
    prHtmlViewUpdate(page.view);
    page.settle();
    CHECK(prHtmlViewOutput(page.view).cursor == PrAppCursor_Arrow);
    auto cursorAt = [&](float x, float y) {
        PrAppEvent move{.kind = PrAppEventKind_PointerMove, .pointer = {.position = PrVec2{x, y}}};
        prHtmlViewEvent(page.view, &move, 0);
        prHtmlViewUpdate(page.view);
        page.settle();
        return prHtmlViewOutput(page.view).cursor;
    };
    CHECK(cursorAt(5, 10) == PrAppCursor_Text);
    CHECK(cursorAt(150, 10) == PrAppCursor_Arrow);
    // The view shows no images, so it shows the keyword that ends the list.
    CHECK(cursorAt(5, 60) == PrAppCursor_Help);
    // The user agent's style sheet gives links the hand, and their text inherits it.
    CHECK(cursorAt(5, 110) == PrAppCursor_Hand);
    // The span's user-select is auto, which takes the none of its parent.
    CHECK(cursorAt(5, 160) == PrAppCursor_Arrow);
    CHECK(cursorAt(150, 210) == PrAppCursor_Text);
    // A vertical-rl block's first line is at its right edge.
    CHECK(cursorAt(190, 258) == PrAppCursor_VerticalText);

    // A new scene changes the cursor under a pointer that has not moved.
    cursorAt(150, 10);
    prDomElementSetAttribute(page.byId("text"), "style", "cursor: move");
    prHtmlViewUpdate(page.view);
    page.settle();
    CHECK(prHtmlViewOutput(page.view).cursor == PrAppCursor_Move);
}

TEST_CASE("the text of a widget other than a text control shows the arrow") {
    Viewed page("<style>body { margin: 0; font: 20px Tinos } button { display: block; width: 200px; height: 50px }"
        "</style><button>Press</button>");
    prHtmlViewSetViewport(page.view, PrVec2{400, 400});
    prHtmlViewUpdate(page.view);
    page.settle();
    PrAppEvent move{.kind = PrAppEventKind_PointerMove, .pointer = {.position = PrVec2{100, 25}}};
    prHtmlViewEvent(page.view, &move, 0);
    prHtmlViewUpdate(page.view);
    page.settle();
    CHECK(prHtmlViewOutput(page.view).cursor == PrAppCursor_Arrow);
}

TEST_CASE("links restyle once the host says that what it knows of visited links changed") {
    Viewed page("<style>body { margin: 0 } a { display: block; width: 40px; height: 40px }"
        " :link { background-color: rgb(255, 0, 0) } :visited { background-color: rgb(0, 0, 255) }</style>"
        "<a id=a href=x></a>");
    prHtmlViewUpdate(page.view);
    page.settle();
    CHECK(rgbAt(page.paint(), 20, 20) == 0xff0000);
    page.visited = page.byId("a");
    prHtmlPageInvalidateHostStates(page.page, PrDomHostState_Visited);
    CHECK(rgbAt(page.paint(), 20, 20) == 0x0000ff);
    CHECK(verified(prHtmlPagePresentation(page.page)));
    page.visited = nullptr;
    prHtmlPageInvalidateHostStates(page.page, PrDomHostState_Visited);
    CHECK(rgbAt(page.paint(), 20, 20) == 0xff0000);
    CHECK(verified(prHtmlPagePresentation(page.page)));
}

TEST_CASE("an img is a replaced box, sized by its image and drawing it, as the host decodes it") {
    PrCompressCodec* compress = prCompressBuiltinCreate();
    PrImageCodec* images = prImageBuiltinCreate(compress);
    // A 4 by 2 red PNG.
    std::vector<uint8_t> red(4 * 2 * 4, 0);
    for (size_t i = 0; i < red.size(); i += 4) {
        red[i] = 255;
        red[i + 3] = 255;
    }
    PrPixelView pixels{red.data(), 16, {4, 2}};
    size_t size = 0;
    REQUIRE(prImageCodecEncode(images, "image/png", pixels, nullptr, 0, &size) == PrStatus_Ok);
    std::vector<uint8_t> png(size);
    REQUIRE(prImageCodecEncode(images, "image/png", pixels, png.data(), size, &size) == PrStatus_Ok);
    {
        Viewed page("<style>body { margin: 0 } img { display: block }</style><img id=i src=http://test/red.png>"
            "<img id=w src=http://test/red.png style='width: 40px'>", images, &png);
        PrDomAgent* agent = prHtmlPageAgent(page.page);
        page.fetcher.settle(agent);
        CHECK(prCssElementGetBoundingClientRect(page.byId("i")).width == 4.0);
        CHECK(prCssElementGetBoundingClientRect(page.byId("i")).height == 2.0);
        // A width alone keeps the image's ratio.
        CHECK(prCssElementGetBoundingClientRect(page.byId("w")).height == 20.0);

        PrDrawList* list = prDrawListCreate();
        prHtmlViewPaint(page.view, list);
        std::vector<uint8_t> drawn(size_t(40) * 40 * 4, 0);
        PrDrawRenderer* cpu = prDrawCpuCreate(nullptr);
        PrPixels into{drawn.data(), 40 * 4, {40, 40}};
        REQUIRE(prDrawRendererRenderPixels(cpu, list, into, PrPixelFormat_Rgba8) == PrStatus_Ok);
        prDrawRendererDestroy(cpu);
        prDrawListRelease(list);
        auto at = [&](uint32_t x, uint32_t y) { return drawn.data() + (size_t(y) * 40 + x) * 4; };
        CHECK(at(30, 10)[0] == 255);
        CHECK(at(30, 10)[3] == 255);
        CHECK(at(30, 30)[3] == 0);
        CHECK(verified(prHtmlPagePresentation(page.page)));

        // An image that changes while the box stands changes what the box shows, and its size.
        prDomElementSetAttribute(page.byId("i"), "src", "");
        CHECK(prHtmlViewUpdate(page.view).started);
        page.settle();
        CHECK(prCssElementGetBoundingClientRect(page.byId("i")).width == 0.0);
        CHECK(verified(prHtmlPagePresentation(page.page)));
        prDomElementSetAttribute(page.byId("i"), "src", "http://test/red.png");
        page.fetcher.settle(agent);
        CHECK(prHtmlViewUpdate(page.view).started);
        page.settle();
        CHECK(prCssElementGetBoundingClientRect(page.byId("i")).width == 4.0);
        CHECK(verified(prHtmlPagePresentation(page.page)));
    }
    prImageCodecDestroy(images);
    prCompressCodecDestroy(compress);
}

TEST_CASE("an img's content takes the cap of its dynamic-range-limit and its image-rendering, as they change") {
    PrCompressCodec* compress = prCompressBuiltinCreate();
    PrImageCodec* images = prImageBuiltinCreate(compress);
    std::vector<uint8_t> red(4 * 2 * 4, 255);
    PrPixelView pixels{red.data(), 16, {4, 2}};
    size_t size = 0;
    REQUIRE(prImageCodecEncode(images, "image/png", pixels, nullptr, 0, &size) == PrStatus_Ok);
    std::vector<uint8_t> png(size);
    REQUIRE(prImageCodecEncode(images, "image/png", pixels, png.data(), size, &size) == PrStatus_Ok);
    {
        Viewed page("<img id=i src=http://test/red.png style='dynamic-range-limit: standard'>", images, &png);
        PrDomAgent* agent = prHtmlPageAgent(page.page);
        page.fetcher.settle(agent);
        PrCssPresentation* presentation = prHtmlPagePresentation(page.page);
        auto limit = [&]() {
            prHtmlViewUpdate(page.view);
            page.settle();
            PrLayoutNode node = prCssPresentationLayoutNode(presentation, prDomElementAsNode(page.byId("i")));
            PrLayoutContent content = prLayoutNodeContent(prCssPresentationTree(presentation), node);
            CHECK(content.kind == PrLayoutContentKind_Image);
            CHECK(verified(presentation));
            return content.limit;
        };
        setHeadroom(page.view, 4);
        CHECK(limit() == 1.0f);
        prDomElementSetAttribute(page.byId("i"), "style", "dynamic-range-limit: constrained");
        CHECK(limit() == 2.0f);
        prDomElementSetAttribute(page.byId("i"), "style", "");
        CHECK(limit() == 0.0f);
        // Half of no-limit is half the display's stops, so the cap follows the headroom.
        prDomElementSetAttribute(page.byId("i"), "style", "dynamic-range-limit: dynamic-range-limit-mix(standard 50%, no-limit 50%)");
        CHECK(limit() == doctest::Approx(2.0f));
        setHeadroom(page.view, 16);
        CHECK(limit() == doctest::Approx(4.0f));
        // image-rendering reaches the content in the same way.
        prDomElementSetAttribute(page.byId("i"), "style", "image-rendering: pixelated");
        limit();
        PrLayoutNode node = prCssPresentationLayoutNode(presentation, prDomElementAsNode(page.byId("i")));
        PrLayoutContent content = prLayoutNodeContent(prCssPresentationTree(presentation), node);
        CHECK(content.rendering == PrLayoutImageRendering_Pixelated);
    }
    prImageCodecDestroy(images);
    prCompressCodecDestroy(compress);
}

TEST_CASE("an img is sized and drawn as its image's orientation displays it, unless image-orientation is none") {
    PrCompressCodec* compress = prCompressBuiltinCreate();
    PrImageCodec* images = prImageBuiltinCreate(compress);
    // Four by two pixels: a red column on the left, and blue elsewhere.
    std::vector<uint8_t> stored(4 * 2 * 4, 0);
    for (uint32_t i = 0; i < 8; ++i) {
        bool red = i % 4 == 0;
        stored[i * 4] = red ? 255 : 0;
        stored[i * 4 + 2] = red ? 0 : 255;
        stored[i * 4 + 3] = 255;
    }
    PrPixelView pixels{stored.data(), 16, {4, 2}};
    size_t size = 0;
    REQUIRE(prImageCodecEncode(images, "image/png", pixels, nullptr, 0, &size) == PrStatus_Ok);
    std::vector<uint8_t> png(size);
    REQUIRE(prImageCodecEncode(images, "image/png", pixels, png.data(), size, &size) == PrStatus_Ok);
    // A quarter turn clockwise displays it two by four, with the red row on top.
    std::vector<uint8_t> turned = withOrientation(png, 6);
    {
        Viewed page("<img id=i src=http://test/turned.png>", images, &turned);
        PrDomAgent* agent = prHtmlPageAgent(page.page);
        page.fetcher.settle(agent);
        PrCssPresentation* presentation = prHtmlPagePresentation(page.page);
        auto content = [&]() {
            prHtmlViewUpdate(page.view);
            page.settle();
            CHECK(verified(presentation));
            PrLayoutNode node = prCssPresentationLayoutNode(presentation, prDomElementAsNode(page.byId("i")));
            return prLayoutNodeContent(prCssPresentationTree(presentation), node);
        };
        PrLayoutContent oriented = content();
        REQUIRE(oriented.kind == PrLayoutContentKind_Image);
        CHECK(prDrawImageExtent(oriented.image).x == 2);
        CHECK(prDrawImagePixels(oriented.image).data[0] == 255);
        CHECK(prDrawImagePixels(oriented.image).data[4] == 255);
        CHECK(prCssElementGetBoundingClientRect(page.byId("i")).width == 2.0);
        CHECK(prCssElementGetBoundingClientRect(page.byId("i")).height == 4.0);
        PrHtmlImageElement* img = prHtmlImageElementQuery(prDomElementAsEventTarget(page.byId("i")));
        REQUIRE(img != nullptr);
        CHECK(prHtmlImageElementNaturalWidth(img) == 2);

        prDomElementSetAttribute(page.byId("i"), "style", "image-orientation: none");
        PrLayoutContent unoriented = content();
        REQUIRE(unoriented.kind == PrLayoutContentKind_Image);
        CHECK(prDrawImageExtent(unoriented.image).x == 4);
        CHECK(prDrawImagePixels(unoriented.image).data[0] == 255);
        CHECK(prDrawImagePixels(unoriented.image).data[4] == 0);
        CHECK(prCssElementGetBoundingClientRect(page.byId("i")).width == 4.0);
        CHECK(prCssElementGetBoundingClientRect(page.byId("i")).height == 2.0);

        prDomElementSetAttribute(page.byId("i"), "style", "");
        CHECK(content().image == oriented.image);
    }
    prImageCodecDestroy(images);
    prCompressCodecDestroy(compress);
}

TEST_CASE("an img selects the source that matches, decodes and suits the display, and divides its size by its density") {
    PrCompressCodec* compress = prCompressBuiltinCreate();
    PrImageCodec* images = prImageBuiltinCreate(compress);
    std::vector<uint8_t> stored(4 * 2 * 4, 255);
    PrPixelView pixels{stored.data(), 16, {4, 2}};
    size_t size = 0;
    REQUIRE(prImageCodecEncode(images, "image/png", pixels, nullptr, 0, &size) == PrStatus_Ok);
    std::vector<uint8_t> png(size);
    REQUIRE(prImageCodecEncode(images, "image/png", pixels, png.data(), size, &size) == PrStatus_Ok);
    {
        // A picture's source of a type that the codec does not decode is passed over for the next.
        Viewed page("<picture><source type=image/avif srcset=http://test/a.avif>"
            "<source srcset='http://test/one.png, http://test/two.png 2x'>"
            "<img id=p src=http://test/fallback.png></picture>"
            "<img id=w srcset='http://test/w400.png 400w, http://test/w800.png 800w'"
            " sizes='(max-width: 500px) 100vw, 50vw' src=http://test/src.png>", images, &png);
        auto image = [&](const char* id) {
            return prHtmlImageElementQuery(prDomElementAsEventTarget(page.byId(id)));
        };
        auto current = [&](const char* id) {
            PrStringView source = prHtmlImageElementCurrentSrc(image(id));
            return std::string(source.data, source.size);
        };
        CHECK(current("p") == "http://test/one.png");
        CHECK(prHtmlImageElementNaturalWidth(image("p")) == 4);
        // An 800px viewport sizes the image at half its width, which the 400w candidate fills at 1x.
        CHECK(current("w") == "http://test/w400.png");

        // At 2x, the 2x candidate is selected, and the image is half as large as its pixels.
        PrAppDisplay display = prAppDisplayDefault();
        display.pixelRatio = 2;
        prHtmlPageSetDisplay(page.page, &display);
        PrDomElement* source = prDomNodeFirstElementChild(prDomNodeParentNode(prDomElementAsNode(page.byId("p"))));
        source = prDomNodeNextElementSibling(prDomElementAsNode(source));
        prDomElementSetAttribute(source, "srcset", "http://test/one.png, http://test/two.png 2x");
        page.fetcher.settle(prHtmlPageAgent(page.page));
        CHECK(current("p") == "http://test/two.png");
        CHECK(prHtmlImageElementNaturalWidth(image("p")) == 2);
        CHECK(prHtmlImageElementNaturalHeight(image("p")) == 1);

        // A 400px viewport sizes the image at its whole width, which the 800w candidate fills at 2x.
        CHECK(prHtmlPageSetMediaFeature(page.page, PrCssDescriptor_MediaWidth, "400px"));
        prDomElementSetAttribute(page.byId("w"), "sizes", "(max-width: 500px) 100vw, 50vw");
        page.fetcher.settle(prHtmlPageAgent(page.page));
        CHECK(current("w") == "http://test/w800.png");
        CHECK(prHtmlImageElementNaturalWidth(image("w")) == 2);

        // Removing the source that the img selected passes over it, here to the img's own src.
        prDomNodeRemove(prDomElementAsNode(source));
        page.fetcher.settle(prHtmlPageAgent(page.page));
        CHECK(current("p") == "http://test/fallback.png");
        CHECK(prHtmlImageElementNaturalWidth(image("p")) == 4);
    }
    prImageCodecDestroy(images);
    prCompressCodecDestroy(compress);
}

TEST_CASE("a frame navigated to an image holds a media document whose img shows the image in its orientation") {
    PrCompressCodec* compress = prCompressBuiltinCreate();
    PrImageCodec* images = prImageBuiltinCreate(compress);
    std::vector<uint8_t> stored(4 * 2 * 4, 255);
    PrPixelView pixels{stored.data(), 16, {4, 2}};
    size_t size = 0;
    REQUIRE(prImageCodecEncode(images, "image/png", pixels, nullptr, 0, &size) == PrStatus_Ok);
    std::vector<uint8_t> png(size);
    REQUIRE(prImageCodecEncode(images, "image/png", pixels, png.data(), size, &size) == PrStatus_Ok);
    std::vector<uint8_t> turned = withOrientation(png, 6);
    {
        // image-orientation does not inherit into the frame's document.
        Viewed page("<iframe id=f src=http://test/turned.png style='image-orientation: none'></iframe>", images, &turned);
        PrDomAgent* agent = prHtmlPageAgent(page.page);
        page.fetcher.settle(agent);
        PrHtmlIFrameElement* frame = prHtmlIFrameElementQuery(prDomElementAsEventTarget(page.byId("f")));
        REQUIRE(frame != nullptr);
        PrDomDocument* document = prHtmlIFrameElementContentDocument(frame);
        REQUIRE(document != nullptr);
        CHECK(!prDomDocumentQuirks(document));
        PrDomElement* body = prDomDocumentBody(document);
        REQUIRE(body != nullptr);
        PrDomElement* img = prDomNodeFirstElementChild(prDomElementAsNode(body));
        REQUIRE(img != nullptr);
        PrStringView source = prDomElementGetAttribute(img, "src");
        CHECK(source == "http://test/turned.png");
        PrHtmlImageElement* image = prHtmlImageElementQuery(prDomElementAsEventTarget(img));
        REQUIRE(image != nullptr);
        CHECK(prHtmlImageElementNaturalWidth(image) == 2);
        CHECK(prHtmlImageElementNaturalHeight(image) == 4);
    }
    prImageCodecDestroy(images);
    prCompressCodecDestroy(compress);
}

TEST_CASE("an object of an image type is replaced by its image, and shows its fallback content without one") {
    PrCompressCodec* compress = prCompressBuiltinCreate();
    PrImageCodec* images = prImageBuiltinCreate(compress);
    std::vector<uint8_t> red(4 * 2 * 4, 255);
    PrPixelView pixels{red.data(), 16, {4, 2}};
    size_t size = 0;
    REQUIRE(prImageCodecEncode(images, "image/png", pixels, nullptr, 0, &size) == PrStatus_Ok);
    std::vector<uint8_t> png(size);
    REQUIRE(prImageCodecEncode(images, "image/png", pixels, png.data(), size, &size) == PrStatus_Ok);
    {
        Viewed page("<object id=o data=http://test/red.png><span id=f>fallback</span></object>", images, &png);
        PrDomAgent* agent = prHtmlPageAgent(page.page);
        PrCssPresentation* presentation = prHtmlPagePresentation(page.page);
        // Returns the kind of the object's box, and checks that a replaced box shows the image.
        auto kind = [&]() {
            page.fetcher.settle(agent);
            prHtmlViewUpdate(page.view);
            page.settle();
            CHECK(verified(presentation));
            PrLayoutNode node = prCssPresentationLayoutNode(presentation, prDomElementAsNode(page.byId("o")));
            PrLayoutNodeKind found = prLayoutNodeKind(prCssPresentationTree(presentation), node);
            if (found == PrLayoutNodeKind_Replaced)
                CHECK(prLayoutNodeContent(prCssPresentationTree(presentation), node).kind == PrLayoutContentKind_Image);
            return found;
        };
        CHECK(kind() == PrLayoutNodeKind_Replaced);
        CHECK(prCssElementGetBoundingClientRect(page.byId("o")).width == 4.0);
        CHECK(prCssPresentationLayoutNode(presentation, prDomElementAsNode(page.byId("f"))) == kPrLayoutNodeNone);

        // Without data, the object represents its children.
        prDomElementSetAttribute(page.byId("o"), "data", "");
        CHECK(kind() == PrLayoutNodeKind_Element);
        CHECK(prCssPresentationLayoutNode(presentation, prDomElementAsNode(page.byId("f"))) != kPrLayoutNodeNone);

        prDomElementSetAttribute(page.byId("o"), "data", "http://test/red.png");
        CHECK(kind() == PrLayoutNodeKind_Replaced);
        CHECK(prCssPresentationLayoutNode(presentation, prDomElementAsNode(page.byId("f"))) == kPrLayoutNodeNone);
    }
    prImageCodecDestroy(images);
    prCompressCodecDestroy(compress);
}

TEST_CASE("an svg element in HTML is a replaced box that draws its content, again when its box is resized") {
    Viewed page("<style>body { margin: 0 } svg { display: block }</style>"
        "<svg id=s width=20 height=10 viewBox='0 0 20 10'><rect id=r width=10 height=10 fill=red /></svg>"
        "<div style='width: 30px'><svg id=p width=50% height=4></svg></div>");
    PrCssPresentation* presentation = prHtmlPagePresentation(page.page);
    auto drawn = [&]() {
        prHtmlViewUpdate(page.view);
        page.settle();
        // A resized box draws its svg again, which the next update records.
        prHtmlViewUpdate(page.view);
        page.settle();
        CHECK(verified(presentation));
        PrDrawList* list = prDrawListCreate();
        prHtmlViewPaint(page.view, list);
        std::vector<uint8_t> pixels = ::presentation(list);
        prDrawListRelease(list);
        return pixels;
    };
    std::vector<uint8_t> pixels = drawn();
    PrLayoutNode node = prCssPresentationLayoutNode(presentation, prDomElementAsNode(page.byId("s")));
    CHECK(prLayoutNodeKind(prCssPresentationTree(presentation), node) == PrLayoutNodeKind_Replaced);
    CHECK(prCssElementGetBoundingClientRect(page.byId("s")).width == 20.0);
    CHECK(prCssElementGetBoundingClientRect(page.byId("s")).height == 10.0);
    // The width attribute is the CSS width, so a percentage is of the containing block.
    CHECK(prCssElementGetBoundingClientRect(page.byId("p")).width == 15.0);
    CHECK(redAt(pixels, 5, 5) == 255);
    CHECK(redAt(pixels, 15, 5) == 0);

    // The viewBox scales the content to the box that CSS gives the element.
    prDomElementSetAttribute(page.byId("s"), "style", "width: 40px; height: 20px");
    pixels = drawn();
    CHECK(redAt(pixels, 15, 15) == 255);
    CHECK(redAt(pixels, 25, 15) == 0);

    // A change within the svg element draws it again.
    prDomElementSetAttribute(page.byId("r"), "fill", "blue");
    pixels = drawn();
    CHECK(redAt(pixels, 15, 15) == 0);
}

TEST_CASE("a change within an svg element builds again only the nodes of its SVG tree that it reaches") {
    ViewedSvg page("<svg width=40 height=40><g id=g><rect id=a width=10 height=10 fill=red />"
        "<rect id=b x=20 width=10 height=10 fill=red /></g><g id=more></g></svg>");
    CHECK(rgbAt(page.paint(), 5, 5) == 0xff0000);

    // A shape's paint builds the shape alone.
    page.set("a", "fill", "blue");
    CHECK(page.update() == 1);
    CHECK(rgbAt(page.paint(), 5, 5) == 0x0000ff);
    CHECK(rgbAt(page.paint(), 25, 5) == 0xff0000);

    // A group's transform is read again in place, and its children are kept.
    page.set("g", "transform", "translate(0 20)");
    CHECK(page.update() == 0);
    CHECK(rgbAt(page.paint(), 5, 25) == 0x0000ff);
    CHECK(rgbAt(page.paint(), 5, 5) == 0);

    // A child that enters builds alone, and one that leaves builds nothing.
    prHtmlSetInnerHtml(page.node("more"), "<circle cx=35 cy=5 r=4 fill=lime />");
    CHECK(page.update() == 1);
    CHECK(rgbAt(page.paint(), 35, 5) == 0x00ff00);
    REQUIRE(prDomNodeRemoveChild(page.node("g"), page.node("b")) == PrStatus_Ok);
    CHECK(page.update() == 0);
    CHECK(rgbAt(page.paint(), 25, 25) == 0);

    // A shape that stops rendering leaves, and comes back in its place.
    page.set("a", "style", "display: none");
    CHECK(page.update() == 0);
    CHECK(rgbAt(page.paint(), 5, 25) == 0);
    page.set("a", "style", "");
    CHECK(page.update() == 1);
    CHECK(rgbAt(page.paint(), 5, 25) == 0x0000ff);

    // A rule that a class decides reaches the shapes whose styles it changes.
    prHtmlSetInnerHtml(page.node("more"), "<style>.on rect { fill: lime }</style>");
    page.update();
    page.set("g", "class", "on");
    CHECK(page.update() == 1);
    CHECK(rgbAt(page.paint(), 5, 25) == 0x00ff00);
}

TEST_CASE("a foreignObject's content is laid out in its positioning rectangle and drawn there, as it changes") {
    ViewedSvg page("<svg width=40 height=40><rect width=40 height=10 fill=blue />"
        "<foreignObject id=f x=10 y=10 width=20 height=20><div id=d style='height: 10px; background: red'></div>"
        "<div style='position: absolute; right: 0; bottom: 0; width: 5px; height: 5px; background: lime'>"
        "</div></foreignObject></svg>");
    std::vector<uint8_t> pixels = page.paint();
    // The block fills the rectangle's width, and the absolutely positioned box sits in its bottom right corner.
    CHECK(rgbAt(pixels, 15, 15) == 0xff0000);
    CHECK(rgbAt(pixels, 29, 15) == 0xff0000);
    CHECK(rgbAt(pixels, 5, 15) == 0);
    CHECK(rgbAt(pixels, 15, 25) == 0);
    CHECK(rgbAt(pixels, 27, 27) == 0x00ff00);
    CHECK(rgbAt(pixels, 5, 5) == 0x0000ff);

    // A change to the content draws it again, and a change to the rectangle moves and lays it out again.
    page.set("d", "style", "height: 10px; background: blue");
    page.update();
    CHECK(rgbAt(page.paint(), 15, 15) == 0x0000ff);
    page.set("f", "x", "0");
    page.set("f", "width", "40");
    page.update();
    pixels = page.paint();
    CHECK(rgbAt(pixels, 5, 15) == 0x0000ff);
    CHECK(rgbAt(pixels, 37, 27) == 0x00ff00);

    // The foreignObject's transform and opacity are SVG's, and its margin places nothing.
    page.set("f", "transform", "translate(0 10)");
    page.set("f", "style", "margin: 10px");
    page.update();
    pixels = page.paint();
    CHECK(rgbAt(pixels, 5, 25) == 0x0000ff);
    CHECK(rgbAt(pixels, 5, 15) == 0);
}

TEST_CASE("a foreignObject of zero width or height draws nothing, and an svg in one is drawn as its own") {
    ViewedSvg page("<svg width=40 height=40><foreignObject id=f x=0 y=0 width=0 height=20 style='overflow: visible'>"
        "<div style='width: 20px; height: 10px; background: red'></div></foreignObject>"
        "<foreignObject x=0 y=20 width=40 height=20><svg width=20 height=10>"
        "<rect width=20 height=10 fill=lime /></svg></foreignObject></svg>");
    std::vector<uint8_t> pixels = page.paint();
    CHECK(rgbAt(pixels, 5, 5) == 0);
    CHECK(rgbAt(pixels, 5, 25) == 0x00ff00);
    CHECK(rgbAt(pixels, 25, 25) == 0);
    page.set("f", "width", "20");
    page.update();
    CHECK(rgbAt(page.paint(), 5, 5) == 0xff0000);
}

TEST_CASE("the client rects of a foreignObject's content are on the page, through the SVG content's transforms") {
    ViewedSvg page("<div style='height: 5px'></div><svg width=40 height=40 viewBox='0 0 20 20'>"
        "<g transform='translate(2 3)'><foreignObject x=1 y=2 width=10 height=10>"
        "<div id=d style='margin-left: 1px; width: 4px; height: 3px'></div></foreignObject></g></svg>");
    // The div is at 1, 0 in the foreignObject, and at 4, 5 in the svg's user space. The viewBox doubles user space, and
    // the svg is 5px below the top of the page.
    PrDomRect rect = prCssElementGetBoundingClientRect(page.byId("d"));
    CHECK(rect.x == doctest::Approx(8));
    CHECK(rect.y == doctest::Approx(15));
    CHECK(rect.width == doctest::Approx(8));
    CHECK(rect.height == doctest::Approx(6));
}

TEST_CASE("a foreignObject clips its content to its rectangle unless its overflow is visible") {
    ViewedSvg page("<svg width=40 height=40><foreignObject id=f width=40 height=10>"
        "<div style='height: 10px'></div><div style='height: 10px; background: red'></div></foreignObject></svg>");
    CHECK(rgbAt(page.paint(), 5, 15) == 0);
    page.set("f", "style", "overflow: visible");
    page.update();
    CHECK(rgbAt(page.paint(), 5, 15) == 0xff0000);
}

TEST_CASE("a change outside every svg element builds no node of their SVG trees") {
    ViewedSvg page("<div id=d></div><svg width=40 height=40><rect width=10 height=10 fill=red /></svg>");
    page.set("d", "class", "x");
    CHECK(page.update() == 0);
}

TEST_CASE("an SVG image element draws the image that it names once the image has loaded") {
    PrCompressCodec* compress = prCompressBuiltinCreate();
    PrImageCodec* images = prImageBuiltinCreate(compress);
    // A 4 by 2 red PNG.
    std::vector<uint8_t> red(4 * 2 * 4, 0);
    for (size_t i = 0; i < red.size(); i += 4) {
        red[i] = 255;
        red[i + 3] = 255;
    }
    PrPixelView pixels{red.data(), 16, {4, 2}};
    size_t size = 0;
    REQUIRE(prImageCodecEncode(images, "image/png", pixels, nullptr, 0, &size) == PrStatus_Ok);
    std::vector<uint8_t> png(size);
    REQUIRE(prImageCodecEncode(images, "image/png", pixels, png.data(), size, &size) == PrStatus_Ok);
    {
        Viewed page("<style>body { margin: 0 } svg { display: block }</style><svg width=40 height=40>"
            "<image id=i href=http://test/red.png width=40 height=40 /></svg>", images, &png, true);
        PrCssPresentation* presentation = prHtmlPagePresentation(page.page);
        prHtmlViewUpdate(page.view);
        page.settle();
        CHECK(rgbAt(page.paint(), 20, 20) == 0);
        CHECK(page.fetcher.waiting() == 1);

        // The load reaches the image's node, which draws the image in its ratio, centered by preserveAspectRatio.
        page.answer();
        prHtmlViewUpdate(page.view);
        page.settle();
        CHECK(verified(presentation));
        std::vector<uint8_t> drawn = page.paint();
        CHECK(rgbAt(drawn, 20, 20) == 0xff0000);
        CHECK(rgbAt(drawn, 20, 5) == 0);

        // The node built again takes the image that has loaded, and fetches nothing.
        prDomElementSetAttribute(page.byId("i"), "preserveAspectRatio", "none");
        prHtmlViewUpdate(page.view);
        page.settle();
        CHECK(verified(presentation));
        CHECK(rgbAt(page.paint(), 20, 5) == 0xff0000);
        CHECK(page.fetcher.waiting() == 0);
    }
    prImageCodecDestroy(images);
    prCompressCodecDestroy(compress);
}

TEST_CASE("an imported sheet applies in the layer that it names, unless its supports() condition fails") {
    std::vector<uint8_t> none;
    Viewed page("<style>@import url(a.css); @import url(b.css) supports(not (display: block));"
        "@import url(c.css) layer(base);</style><p id=p>text</p><p id=q>text</p>",
        nullptr, &none, true, "https://example.test/page.html");
    page.answer("a.css", "#p { color: rgb(0, 0, 255) } #q { color: rgb(0, 128, 0) }", "text/css");
    page.answer("b.css", "#p { color: rgb(255, 0, 0) }", "text/css");
    page.answer("c.css", "#p { color: rgb(255, 255, 0) } #q { background-color: rgb(0, 0, 255) }", "text/css");
    prHtmlViewUpdate(page.view);
    page.settle();
    auto resolved = [&](const char* id, PrCssProperty property) {
        PrCssDeclarationName name = prCssDeclarationNameFromProperty(property);
        PrRawString read = prCssStyleDeclarationGetPropertyValue(
            prCssStylePropertiesAsCssStyleDeclaration(prCssElementComputedStyle(page.byId(id), "")), &name);
        std::string value(prRawStringView(&read).data, prRawStringView(&read).size);
        prRawStringReset(&read);
        return value;
    };
    // The unlayered import wins over the layered one. The import whose condition fails is fetched, and applies nothing.
    CHECK(resolved("p", PrCssProperty_Color) == "rgb(0, 0, 255)");
    CHECK(resolved("q", PrCssProperty_Color) == "rgb(0, 128, 0)");
    CHECK(resolved("q", PrCssProperty_BackgroundColor) == "rgb(0, 0, 255)");
}

TEST_CASE("a use and a paint server of another document of the page's origin draw once the document loads") {
    std::vector<uint8_t> none;
    Viewed page("<style>body { margin: 0 } svg { display: block }</style><svg width=40 height=40>"
        "<use href=defs.svg#square /><use href=https://elsewhere.test/defs.svg#square y=20 />"
        "<rect x=20 width=20 height=40 fill='url(defs.svg#red) black' /></svg>",
        nullptr, &none, true, "https://example.test/page.html");
    PrCssPresentation* presentation = prHtmlPagePresentation(page.page);
    prHtmlViewUpdate(page.view);
    page.settle();
    // One fetch for both references to the same document, and none for another origin.
    REQUIRE(page.fetcher.waiting() == 1);
    CHECK(TestFetcher::urlOf(page.fetcher.held[0]) == "https://example.test/defs.svg");
    std::vector<uint8_t> before = page.paint();
    CHECK(rgbAt(before, 10, 10) == 0);
    CHECK(rgbAt(before, 30, 10) == 0);
    page.answer("defs.svg",
        "<svg xmlns='http://www.w3.org/2000/svg'><rect id='square' width='20' height='20' fill='#0000ff'/>"
            "<linearGradient id='red'><stop stop-color='#ff0000'/></linearGradient></svg>", "image/svg+xml");
    prHtmlViewUpdate(page.view);
    page.settle();
    CHECK(verified(presentation));
    std::vector<uint8_t> after = page.paint();
    CHECK(rgbAt(after, 10, 10) == 0x0000ff);
    CHECK(rgbAt(after, 10, 30) == 0);
    CHECK(rgbAt(after, 30, 10) == 0xff0000);
}

TEST_CASE("a gradient is built again when its stops change, and the shapes that it paints follow, in any svg element") {
    ViewedSvg page("<svg width=40 height=20><defs><linearGradient id=gr><stop id=s offset=0 stop-color=red />"
        "<stop offset=1 stop-color=red /></linearGradient></defs><rect width=40 height=20 fill=url(#gr) /></svg>"
        "<svg width=40 height=20><rect width=40 height=20 fill=url(#gr) /></svg>");
    CHECK(rgbAt(page.paint(), 20, 10) == 0xff0000);
    CHECK(rgbAt(page.paint(), 20, 30) == 0xff0000);
    page.set("s", "stop-color", "blue");
    CHECK(page.update() == 0);
    std::vector<uint8_t> pixels = page.paint();
    CHECK((rgbAt(pixels, 1, 10) & 0xff) > 200);
    CHECK((rgbAt(pixels, 1, 30) & 0xff) > 200);
}

TEST_CASE("a pattern is built again when its template's content or its own style changes, and its shapes follow") {
    ViewedSvg page("<svg width=40 height=20><defs><pattern id=t width=20 height=20 patternUnits=userSpaceOnUse>"
        "<rect id=c width=10 height=20 /></pattern><pattern id=h href=#t fill=red /></defs>"
        "<rect width=40 height=20 fill=url(#h) /></svg>");
    CHECK(rgbAt(page.paint(), 5, 10) == 0xff0000);
    CHECK(rgbAt(page.paint(), 15, 10) == 0);
    // The template's content is the pattern's shadow tree, whose clones inherit from the pattern.
    page.set("h", "fill", "blue");
    page.update();
    CHECK(rgbAt(page.paint(), 25, 10) == 0x0000ff);
    page.set("c", "width", "20");
    page.update();
    CHECK(rgbAt(page.paint(), 15, 10) == 0x0000ff);
}

TEST_CASE("a clipPath or a mask is built again when its content changes, and what it clips or masks follows") {
    ViewedSvg page("<svg width=40 height=20><defs><clipPath id=c><rect id=cr width=10 height=20 /></clipPath>"
        "<mask id=m maskUnits=userSpaceOnUse><rect id=mr x=20 width=10 height=20 fill=white /></mask>"
        "</defs><rect width=40 height=20 fill=blue clip-path=url(#c) />"
        "<rect id=masked width=40 height=20 fill=red mask=url(#m) /></svg>");
    CHECK(rgbAt(page.paint(), 5, 10) == 0x0000ff);
    CHECK(rgbAt(page.paint(), 15, 10) == 0);
    CHECK(rgbAt(page.paint(), 25, 10) == 0xff0000);
    page.set("cr", "width", "20");
    page.update();
    CHECK(rgbAt(page.paint(), 15, 10) == 0x0000ff);
    page.set("mr", "x", "30");
    page.update();
    CHECK(rgbAt(page.paint(), 25, 10) == 0);
    CHECK(rgbAt(page.paint(), 35, 10) == 0xff0000);
    // A style that changes the reference changes the node that holds it.
    page.set("masked", "style", "mask: none");
    page.update();
    CHECK(rgbAt(page.paint(), 25, 10) == 0xff0000);
}

TEST_CASE("a filter is built again when a primitive changes, and what it filters follows") {
    ViewedSvg page("<svg width=40 height=20><filter id=f filterUnits=userSpaceOnUse x=0 y=0 width=20 height=20>"
        "<feFlood id=flood flood-color=blue /></filter><rect id=r width=40 height=20 fill=red filter=url(#f) /></svg>");
    CHECK(rgbAt(page.paint(), 5, 10) == 0x0000ff);
    CHECK(rgbAt(page.paint(), 30, 10) == 0);
    page.set("flood", "flood-color", "lime");
    page.update();
    CHECK(rgbAt(page.paint(), 5, 10) == 0x00ff00);
    // A style that drops the filter changes the node that held it.
    page.set("r", "style", "filter: none");
    page.update();
    CHECK(rgbAt(page.paint(), 30, 10) == 0xff0000);
}

TEST_CASE("a filter inside an element that displays none still applies, with its primitives' styles") {
    ViewedSvg page("<div style='display: none'><svg><filter id=f><feFlood flood-color=blue /></filter></svg></div>"
        "<svg width=40 height=20><rect x=10 y=5 width=20 height=10 fill=red filter=url(#f) /></svg>");
    CHECK(rgbAt(page.paint(), 20, 10) == 0x0000ff);
}

TEST_CASE("a reference in a shadow tree names the element of its own tree first") {
    ViewedSvg page("<svg width=40 height=20><clipPath id=c><rect width=20 height=20 /></clipPath></svg>"
        "<div id=host></div>");
    PrDomShadowRootInit init = prDomShadowRootInitDefault(PrDomShadowRootMode_Open);
    PrDomShadowRoot* shadow = nullptr;
    REQUIRE(prDomElementAttachShadow(page.byId("host"), &init, &shadow) == PrStatus_Ok);
    prHtmlSetInnerHtml(prDomShadowRootAsNode(shadow),
        "<svg width=40 height=20><clipPath id=c><rect x=20 width=20 height=20 /></clipPath>"
            "<rect width=40 height=20 fill=blue clip-path=url(#c) /></svg>");
    page.update();
    CHECK(rgbAt(page.paint(), 30, 30) == 0x0000ff);
    CHECK(rgbAt(page.paint(), 10, 30) == 0);
}

TEST_CASE("an outermost svg element's clip path is its box's, which clips the content once") {
    ViewedSvg page("<svg width=40 height=20 clip-path=url(#c)><clipPath id=c><rect width=10 height=20 />"
        "</clipPath><rect width=40 height=20 fill=blue /></svg>");
    std::vector<uint8_t> pixels = page.paint();
    CHECK(rgbAt(pixels, 5, 10) == 0x0000ff);
    CHECK(rgbAt(pixels, 30, 10) == 0);
}

TEST_CASE("an HTML box's clip path, mask and filter may reference SVG resources, and follow changes to them") {
    ViewedSvg page("<svg width=0 height=0><clipPath id=c><rect id=r width=10 height=10 /></clipPath>"
        "<mask id=m><rect width=20 height=10 fill=white /></mask>"
        "<filter id=f><feFlood flood-color=red /></filter></svg>"
        "<div style='width: 30px; height: 10px; background: blue; clip-path: url(#c)'></div>"
        "<div style='width: 30px; height: 10px; background: blue; mask: url(#m)'></div>"
        "<div style='width: 30px; height: 10px; background: blue; filter: url(#f)'></div>");
    std::vector<uint8_t> pixels = page.paint();
    CHECK(rgbAt(pixels, 5, 5) == 0x0000ff);
    CHECK(rgbAt(pixels, 25, 5) == 0);
    CHECK(rgbAt(pixels, 15, 15) == 0x0000ff);
    CHECK(rgbAt(pixels, 25, 15) == 0);
    // The filter region reaches 10% past the box on each side.
    CHECK(rgbAt(pixels, 15, 25) == 0xff0000);
    CHECK(rgbAt(pixels, 32, 25) == 0xff0000);
    CHECK(rgbAt(pixels, 35, 25) == 0);
    page.set("r", "width", "30");
    page.update();
    CHECK(rgbAt(page.paint(), 25, 5) == 0x0000ff);
}

TEST_CASE("an HTML box's fill-box is its border box for mask-origin and its content box for mask-clip") {
    // Each box is 18 by 18, with its content box 6 by 6 at 6, 6, and its mask a square 9 by 9 at the corner of the box
    // of mask-origin. The second box is at 18, 0.
    ViewedSvg page("<style>div { float: left; width: 6px; height: 6px; padding: 6px; background: blue;"
        " mask: linear-gradient(black, black) no-repeat; mask-size: 9px 9px }</style>"
        "<div style='mask-origin: fill-box; mask-clip: no-clip'></div><div style='mask-clip: fill-box'></div>");
    std::vector<uint8_t> pixels = page.paint();
    CHECK(rgbAt(pixels, 2, 2) == 0x0000ff);
    CHECK(rgbAt(pixels, 11, 11) == 0);
    CHECK(rgbAt(pixels, 25, 8) == 0x0000ff);
    CHECK(rgbAt(pixels, 20, 2) == 0);
}

TEST_CASE("an HTML box's filter draws an feImage of an element, moved by the subregion's corner") {
    // The region runs from -4 to 36 across the box, so the image of the rect at 10 lands at 6.
    ViewedSvg page("<svg width=0 height=0><filter id=f x=-10% y=0 width=100% height=100%><feImage href=#g /></filter>"
        "<defs><g id=g><rect x=10 width=10 height=10 fill=lime /></g></defs></svg>"
        "<div style='width: 40px; height: 20px; filter: url(#f)'></div>");
    std::vector<uint8_t> pixels = page.paint();
    CHECK(rgbAt(pixels, 10, 5) == 0x00ff00);
    CHECK(rgbAt(pixels, 18, 5) == 0);
}

TEST_CASE("an ID that comes to name another element builds again what references it") {
    ViewedSvg page("<svg width=40 height=40><rect width=40 height=40 fill='url(#paint) blue' />"
        "<defs><linearGradient id=gr><stop offset=0 stop-color=red /><stop offset=1 stop-color=red />"
        "</linearGradient></defs></svg>");
    CHECK(rgbAt(page.paint(), 20, 20) == 0x0000ff);
    page.set("gr", "id", "paint");
    CHECK(page.update() == 1);
    CHECK(rgbAt(page.paint(), 20, 20) == 0xff0000);
    prDomElementRemoveAttribute(page.byId("paint"), "id");
    CHECK(page.update() == 1);
    CHECK(rgbAt(page.paint(), 20, 20) == 0x0000ff);
}

TEST_CASE("a change to what a use element references reaches its instance, and a change to the use builds it again") {
    ViewedSvg page("<svg width=40 height=40><defs><rect id=t width=10 height=10 fill=red /></defs><use id=u href=#t x=20 /></svg>");
    CHECK(rgbAt(page.paint(), 25, 5) == 0xff0000);
    // The instance of the rect takes the attribute in place, and only its node is built again.
    page.set("t", "fill", "blue");
    CHECK(page.update() == 1);
    CHECK(rgbAt(page.paint(), 25, 5) == 0x0000ff);
    page.set("u", "x", "0");
    CHECK(page.update() == 3);
    CHECK(rgbAt(page.paint(), 5, 5) == 0x0000ff);
    CHECK(rgbAt(page.paint(), 25, 5) == 0);
}

TEST_CASE("a use's instances inherit from the use, and match the document's rules within their own tree") {
    // The instance's fill is the use's, which it inherits. The rule with a combinator matches the original, which is in
    // the group, and not its instance, which is the shadow tree's root.
    ViewedSvg page("<style>#holder > rect { fill: red } use { fill: blue } .wide { width: 20px }</style>"
        "<svg width=40 height=40><defs><g id=holder><rect id=t class=wide height=10 /></g></defs><use href=#t x=0 y=20 /></svg>");
    std::vector<uint8_t> pixels = page.paint();
    CHECK(rgbAt(pixels, 5, 25) == 0x0000ff);
    CHECK(rgbAt(pixels, 15, 25) == 0x0000ff);
    CHECK(rgbAt(pixels, 25, 25) == 0);
}

TEST_CASE("a use's instance transitions when its original's class changes, and keeps transitioning through a removal") {
    ViewedSvg page("<style>#r { fill: rgb(0, 0, 0); transition: fill 1000ms linear } .on > #r { fill: rgb(0, 0, 255) }"
        "</style><svg width=40 height=40><defs><g id=g><rect id=r width=10 height=10 />"
        "<text id=gone>x</text></g></defs><use href=#g /></svg>");
    PrCssPresentation* dom = prHtmlPagePresentation(page.page);
    prCssPresentationSetTime(dom, 0);
    page.set("g", "class", "on");
    page.update();
    prCssPresentationSetTime(dom, 500);
    page.update();
    uint32_t half = rgbAt(page.paint(), 5, 5) & 0xff;
    CHECK(half > 100);
    CHECK(half < 160);
    prDomNodeRemove(page.node("gone"));
    page.update();
    uint32_t after = rgbAt(page.paint(), 5, 5) & 0xff;
    CHECK(after > 100);
    CHECK(after < 160);
}

TEST_CASE("an instance of a symbol's child leaves and comes back with its original") {
    ViewedSvg page("<svg width=0 height=0><symbol id=s viewBox='0 0 20 20'><path id=d d='M0 0H20V20H0Z' fill=blue />"
        "</symbol></svg><svg width=20 height=20><use xlink:href=#s /></svg>");
    CHECK(rgbAt(page.paint(), 10, 10) == 0x0000ff);
    PrDomNode* path = page.node("d");
    PrDomNode* symbol = prDomNodeParentNode(path);
    prDomNodeRetain(path);
    prDomNodeRemove(path);
    page.update();
    CHECK(rgbAt(page.paint(), 10, 10) == 0);
    REQUIRE(prDomNodeAppendChild(symbol, path) == PrStatus_Ok);
    prDomNodeRelease(path);
    page.update();
    CHECK(rgbAt(page.paint(), 10, 10) == 0x0000ff);
}

TEST_CASE("text is laid out again when its characters change, and a switch chooses again when its children change") {
    ViewedSvg page("<svg width=40 height=40><text id=t y=20>Hi</text>"
        "<switch><rect id=x systemLanguage=fr width=40 height=10 fill=red />"
        "<rect y=30 width=40 height=10 fill=blue /></switch></svg>");
    prDomNodeSetTextContent(page.node("t"), "Hello");
    CHECK(page.update() == 1);
    CHECK(rgbAt(page.paint(), 20, 35) == 0x0000ff);
    page.set("x", "systemLanguage", "en");
    CHECK(page.update() == 2);
    CHECK(rgbAt(page.paint(), 20, 5) == 0xff0000);
    CHECK(rgbAt(page.paint(), 20, 35) == 0);
}

TEST_CASE("::first-line styles the first line of a block, and ::first-letter makes a box of its first letter") {
    Viewed page("<style>body { margin: 0; font: 10px/10px sans-serif } p { margin: 0; width: 200px }"
        ".big::first-line { font-size: 30px; line-height: 30px }"
        ".drop::first-letter { float: left; font-size: 30px; line-height: 30px }div.big p { color: red }</style>"
        "<p id=a>aa bb cc dd ee</p><p id=b>H<span id=e>ello</span></p><div id=c><p id=d>aa bb</p></div>");
    auto height = [&](const char* id) {
        prHtmlViewUpdate(page.view);
        page.settle();
        return prCssElementGetBoundingClientRect(page.byId(id)).height;
    };
    CHECK(height("a") == 10.0);
    // The first line takes its line height, and the others keep the block's.
    prDomElementSetAttribute(page.byId("a"), "class", "big");
    CHECK(height("a") == 30.0);
    prDomElementSetAttribute(page.byId("a"), "style", "width: 60px");
    CHECK(height("a") > 30.0);
    prDomElementSetAttribute(page.byId("a"), "class", "");
    prDomElementSetAttribute(page.byId("a"), "style", "");
    CHECK(height("a") == 10.0);
    // A floated first letter takes its own room, and the text after it moves over.
    CHECK(height("b") == 10.0);
    double before = prCssElementGetBoundingClientRect(page.byId("e")).x;
    prDomElementSetAttribute(page.byId("b"), "class", "drop");
    CHECK(height("b") == 10.0);
    CHECK(prCssElementGetBoundingClientRect(page.byId("e")).x > before + 5);
    // The first formatted line of a div is its first paragraph's.
    prDomElementSetAttribute(page.byId("c"), "class", "big");
    CHECK(height("d") == 30.0);
}

TEST_CASE("text is laid out again when its ::first-line or ::first-letter style changes, its own style unchanged") {
    ViewedSvg page("<svg width=40 height=40><style>.line::first-line { fill: rgb(0, 0, 255) }</style>"
        "<text id=t y=35 font-size=30 fill=black>II</text></svg>");
    auto blue = [](const std::vector<uint8_t>& pixels) {
        uint32_t count = 0;
        for (uint32_t y = 0; y < 40; ++y) {
            for (uint32_t x = 0; x < 40; ++x)
                count += rgbAt(pixels, x, y) == 0x0000ff ? 1 : 0;
        }
        return count;
    };
    CHECK(blue(page.paint()) == 0);
    page.set("t", "class", "line");
    CHECK(page.update() == 1);
    CHECK(blue(page.paint()) > 0);
    page.set("t", "class", "");
    CHECK(page.update() == 1);
    CHECK(blue(page.paint()) == 0);
}

TEST_CASE("an img of an SVG image is sized by its root's width and height and draws its content, decoded or not") {
    // No host decoder: the engine draws an SVG image itself.
    Viewed page("<style>body { margin: 0 } img { display: block }</style><img id=s src=\"data:image/svg+xml,<svg "
        "xmlns='http://www.w3.org/2000/svg' width='20' height='10'><rect width='20' height='10' "
        "fill='red'/></svg>\" style='width: 40px'>");
    PrDomAgent* agent = prHtmlPageAgent(page.page);
    page.fetcher.settle(agent);
    // A width alone keeps the image's ratio, and its content scales with the box.
    CHECK(prCssElementGetBoundingClientRect(page.byId("s")).width == 40.0);
    CHECK(prCssElementGetBoundingClientRect(page.byId("s")).height == 20.0);
    PrDrawList* list = prDrawListCreate();
    prHtmlViewPaint(page.view, list);
    std::vector<uint8_t> drawn = presentation(list);
    prDrawListRelease(list);
    CHECK(redAt(drawn, 35, 15) == 255);
    CHECK(drawn[(size_t(25) * 40 + 35) * 4 + 3] == 0);
    CHECK(verified(prHtmlPagePresentation(page.page)));
}

TEST_CASE("an SVG image without a width or a height draws at the size it is shown, as an img and as a background") {
    // No natural size: the image fills the box. A viewBox alone gives a ratio, which the viewBox's own
    // preserveAspectRatio fits into the box.
    const char* full = "data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg'><rect width='100%25' "
        "height='100%25' fill='red'/></svg>";
    const char* square = "data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 10 10'><rect "
        "width='10' height='10' fill='red'/></svg>";
    std::string html = std::string("<style>body { margin: 0 } img, div { display: block; width: 40px; height: 10px }"
        "</style><img src=\"") + full + "\"><img src=\"" + square +
        "\"><div style=\"background-image: url(&quot;" + full + "&quot;)\"></div>";
    Viewed page(html.c_str());
    PrDomAgent* agent = prHtmlPageAgent(page.page);
    page.fetcher.settle(agent);
    prHtmlViewUpdate(page.view);
    page.settle();
    PrDrawList* list = prDrawListCreate();
    prHtmlViewPaint(page.view, list);
    std::vector<uint8_t> drawn = presentation(list);
    prDrawListRelease(list);
    CHECK(rgbAt(drawn, 2, 5) == 0xff0000);
    CHECK(rgbAt(drawn, 37, 5) == 0xff0000);
    // The square is 10 by 10 in the middle of the 40 by 10 box.
    CHECK(rgbAt(drawn, 20, 15) == 0xff0000);
    CHECK(rgbAt(drawn, 5, 15) == 0);
    CHECK(rgbAt(drawn, 2, 25) == 0xff0000);
    CHECK(rgbAt(drawn, 37, 25) == 0xff0000);
    CHECK(verified(prHtmlPagePresentation(page.page)));
}

TEST_CASE("an SVG image's root background fills the image, as the background of its canvas") {
    Viewed page("<style>body { margin: 0 } img { display: block; width: 40px }</style><img src=\"data:image/svg+xml,"
        "<svg xmlns='http://www.w3.org/2000/svg' width='20' height='10' style='background: blue'><rect "
        "width='5' height='5' fill='red'/></svg>\">");
    PrDomAgent* agent = prHtmlPageAgent(page.page);
    page.fetcher.settle(agent);
    prHtmlViewUpdate(page.view);
    page.settle();
    PrDrawList* list = prDrawListCreate();
    prHtmlViewPaint(page.view, list);
    std::vector<uint8_t> drawn = presentation(list);
    prDrawListRelease(list);
    CHECK(rgbAt(drawn, 5, 5) == 0xff0000);
    CHECK(rgbAt(drawn, 30, 15) == 0x0000ff);
}

TEST_CASE("an iframe shows its document in its slot, laid out in a viewport of its content box") {
    Viewed page("<style>body { margin: 0 } iframe { display: block; border: 0; width: 40px; height: 20px }</style>"
        "<iframe id=f srcdoc=\"<style>html { background: rgb(255, 0, 0) } div { height: 5px }</style><div></div>\">"
        "</iframe>");
    // The first jobs size the iframe's viewport from the slot that the window's scene gives it, and the next jobs lay
    // it out there.
    for (uint32_t i = 0; i < 4; ++i) {
        prHtmlViewUpdate(page.view);
        prHtmlViewWait(page.view);
    }
    prHtmlViewUpdate(page.view);
    PrDomElement* frame = page.byId("f");
    PrHtmlWindow* inner =
        prHtmlIFrameElementContentWindow(prHtmlIFrameElementQuery(prDomNodeAsEventTarget(prDomElementAsNode(frame))));
    REQUIRE(inner != nullptr);
    CHECK(prHtmlWindowInnerWidth(inner) == 40);
    prLayoutCompositorFrame(page.compositor);
    REQUIRE(!page.pixels.empty());
    CHECK(redAt(page.pixels, 10, 10) == 255);
    CHECK(redAt(page.pixels, 10, 30) == 0);

    // Recorded immediately, the document shows the same.
    PrDrawList* list = prDrawListCreate();
    prHtmlViewPaint(page.view, list);
    std::vector<uint8_t> painted = presentation(list);
    prDrawListRelease(list);
    CHECK(redAt(painted, 10, 10) == 255);
    CHECK(redAt(painted, 10, 30) == 0);
}

TEST_CASE("an iframe's document is laid out in its iframe's content box before its parent is shown, and keeps a scroll") {
    Viewed page("<style>body { margin: 0 } iframe { display: block; border: 0; width: 40px; height: 20px }</style>"
        "<iframe id=f srcdoc=\"<style>body { margin: 0 } div { height: 30px }</style><div></div>"
        "<div style='height: 20px; background: rgb(255, 0, 0)'></div><div></div>\"></iframe>");
    PrDomElement* frame = page.byId("f");
    PrHtmlWindow* inner =
        prHtmlIFrameElementContentWindow(prHtmlIFrameElementQuery(prDomNodeAsEventTarget(prDomElementAsNode(frame))));
    REQUIRE(inner != nullptr);
    // No job of the view has sized the frame yet: a script's query reads the iframe's content box.
    CHECK(prHtmlWindowInnerWidth(inner) == 40);
    CHECK(prHtmlWindowInnerHeight(inner) == 20);
    // The document is 80px tall in a 20px viewport, so it scrolls to the red band, 30px down.
    PrCssScrollToOptions options{.hasTop = true, .top = 30};
    prHtmlWindowScroll(inner, &options);
    CHECK(prHtmlWindowScrollY(inner) == 30);
    PrDrawList* list = prDrawListCreate();
    prHtmlViewPaint(page.view, list);
    std::vector<uint8_t> painted = presentation(list);
    prDrawListRelease(list);
    CHECK(redAt(painted, 10, 10) == 255);
}

TEST_CASE("an iframe's document prefers the iframe's color scheme, and paints an opaque canvas where its own differs") {
    Viewed page("<style>body { margin: 0 } iframe { display: block; border: 0; width: 40px; height: 12px; margin-bottom: "
        "1px }</style><iframe style='color-scheme: light' srcdoc=\"<html style='color-scheme: dark'>\"></iframe>"
        "<iframe style='color-scheme: dark' srcdoc=\"<html style='color-scheme: dark'>\"></iframe>"
        "<iframe style='color-scheme: dark' srcdoc=\"<style>@media (prefers-color-scheme: dark) { html { "
        "background: rgb(0, 0, 255) } }</style>\"></iframe>");
    for (uint32_t i = 0; i < 6; ++i) {
        prHtmlViewUpdate(page.view);
        prHtmlViewWait(page.view);
    }
    PrDrawList* list = prDrawListCreate();
    // The page's own canvas is the light Canvas, which the host fills.
    PrColor canvas = prColorFromColor16(prHtmlViewPaint(page.view, list));
    CHECK(canvas.r == 1.0f);
    CHECK(canvas.a == 1.0f);
    std::vector<uint8_t> painted = presentation(list);
    prDrawListRelease(list);
    auto at = [&](uint32_t y, uint32_t channel) { return painted[(size_t(y) * 40 + 20) * 4 + channel]; };
    // A light iframe around a dark document: the dark Canvas, opaque.
    CHECK(at(6, 3) == 255);
    CHECK(at(6, 0) < 64);
    // A dark iframe around a dark document: a transparent canvas.
    CHECK(at(19, 3) == 0);
    // A dark iframe: its document prefers dark.
    CHECK(at(32, 2) == 255);
    CHECK(at(32, 0) == 0);
}

TEST_CASE("a view paints its composited animations at the presentation's time") {
    Viewed page("<style>body { margin: 0 } div { width: 20px; height: 20px; background: rgb(255, 0, 0) }"
        "@keyframes move { from { transform: none } to { transform: translateX(20px) } }"
        "#m { animation: move 1000ms linear }</style><div id=m></div>");
    PrCssPresentation* dom = prHtmlPagePresentation(page.page);
    auto paintAt = [&](double time) {
        prCssPresentationSetTime(dom, time);
        PrDrawList* list = prDrawListCreate();
        prHtmlViewPaint(page.view, list);
        std::vector<uint8_t> drawn = presentation(list);
        prDrawListRelease(list);
        return drawn;
    };
    std::vector<uint8_t> start = paintAt(0);
    CHECK(prCssPresentationTracks(dom, nullptr, 0) == 1);
    CHECK(redAt(start, 5, 10) == 255);
    CHECK(redAt(start, 35, 10) == 0);
    // Halfway, the box has moved by 10px, as a compositor's frame at that time shows it.
    std::vector<uint8_t> half = paintAt(500);
    CHECK(redAt(half, 5, 10) == 0);
    CHECK(redAt(half, 25, 10) == 255);
}

TEST_CASE("an important declaration overrides an animation of its property, composited or not") {
    Viewed page("<style>body { margin: 0 } div { width: 20px; height: 20px; background: rgb(255, 0, 0) }"
        "@keyframes move { from { transform: none; margin-left: 0 } to { transform: translateX(20px); margin-left: 20px } }"
        "#m { animation: move 1000ms linear; transform: none !important; margin-left: 0 !important }</style><div id=m></div>");
    PrCssPresentation* dom = prHtmlPagePresentation(page.page);
    prCssPresentationSetTime(dom, 500);
    PrDrawList* list = prDrawListCreate();
    prHtmlViewPaint(page.view, list);
    std::vector<uint8_t> drawn = presentation(list);
    prDrawListRelease(list);
    CHECK(redAt(drawn, 5, 10) == 255);
    CHECK(redAt(drawn, 25, 10) == 0);
}

TEST_CASE("a view paints a composited 3D transform under its parent's perspective through the projection") {
    Viewed page("<style>body { margin: 0 } #p { perspective: 40px; width: 40px; height: 40px }"
        "#c { width: 40px; height: 40px; background: rgb(255, 0, 0) }"
        "@keyframes turn { from { transform: rotateY(0deg) } to { transform: rotateY(120deg) } }"
        "#c { animation: turn 1000ms linear }</style><div id=p><div id=c></div></div>");
    PrCssPresentation* dom = prHtmlPagePresentation(page.page);
    auto paintAt = [&](double time) {
        prCssPresentationSetTime(dom, time);
        PrDrawList* list = prDrawListCreate();
        prHtmlViewPaint(page.view, list);
        std::vector<uint8_t> drawn = presentation(list);
        prDrawListRelease(list);
        return drawn;
    };
    std::vector<uint8_t> flat = paintAt(0);
    CHECK(redAt(flat, 2, 2) == 255);
    CHECK(redAt(flat, 37, 2) == 255);
    // At 30deg the near (left) edge grows past the box and the far (right) edge shrinks toward the center: the corner
    // at the far side's top is empty, and the near side still covers its middle.
    std::vector<uint8_t> turned = paintAt(250);
    CHECK(redAt(turned, 37, 1) == 0);
    CHECK(redAt(turned, 2, 20) == 255);
}

TEST_CASE("a page that the wheel scrolled keeps the reader's place as its window narrows and content grows above") {
    // Like most pages, it adapts its padding to the width, which changes values on the path to every anchor.
    std::string html = "<style>body { margin: 0; padding: 0 40px } p { margin: 0 0 16px }"
        "@media (max-width: 700px) { body { padding: 0 8px } }</style>";
    for (int i = 0; i < 80; ++i) {
        html += "<p id=p" + std::to_string(i) + ">Paragraph " + std::to_string(i);
        for (int j = 0; j < 4 + i % 7; ++j)
            html += " runs on with words that wrap at narrower widths";
        html += "</p>";
    }
    Viewed page(html.c_str());
    PrHtmlWindow* window = prHtmlPageWindow(page.page);
    // Each frame of the host: the viewport at the window's size, an update that commits the last job's scene and starts
    // the next, and a present.
    auto frame = [&](PrVec2 extent) {
        prHtmlViewSetViewport(page.view, extent);
        prHtmlViewUpdate(page.view);
        page.settle();
        prLayoutCompositorFrame(page.compositor);
    };
    // The compositor's offset, which is what the window shows.
    auto shown = [&] {
        PrLayoutVec back = prLayoutCompositorScroll(page.compositor, 0, kPrLayoutNodeNone, prLayoutVecFromPx(PrVec2{0, -1e7f}));
        prLayoutCompositorScroll(page.compositor, 0, kPrLayoutNodeNone, PrLayoutVec{back.x, -back.y});
        return -double(prLayoutUnitToPx(back.y));
    };
    frame(PrVec2{800, 600});
    // The wheel scrolls to the middle of the page through the compositor.
    prLayoutCompositorScrollAt(page.compositor, prLayoutVecFromPx(PrVec2{400, 300}), prLayoutVecFromPx(PrVec2{0, 2000}));
    frame(PrVec2{800, 600});
    REQUIRE(prHtmlWindowScrollY(window) == 2000.0);
    PrDomElement* anchor = prCssDocumentElementFromPoint(page.document(), 60, 1);
    REQUIRE(anchor != nullptr);
    double top = prCssElementGetBoundingClientRect(anchor).y;

    // The window narrows a step each frame, as a user drags its edge, and the text above wraps onto more lines.
    for (float width = 780; width >= 500; width -= 20)
        frame(PrVec2{width, 600});
    CHECK(prHtmlWindowScrollY(window) > 2000.0);
    CHECK(prCssElementGetBoundingClientRect(anchor).y == doctest::Approx(top));
    CHECK(shown() == doctest::Approx(prHtmlWindowScrollY(window)));

    // Content grows above, as an image that loads would.
    double before = prHtmlWindowScrollY(window);
    prDomElementSetAttribute(page.byId("p2"), "style", "height: 900px");
    frame(PrVec2{500, 600});
    CHECK(prHtmlWindowScrollY(window) > before);
    CHECK(prCssElementGetBoundingClientRect(anchor).y == doctest::Approx(top));
    CHECK(shown() == doctest::Approx(prHtmlWindowScrollY(window)));
}

TEST_CASE("SVG content is hit over a shape's fill or stroke as pointer-events says, or else the svg is") {
    ViewedSvg page("<svg id=s width=200 height=60><circle id=ring cx=30 cy=30 r=20 fill=none stroke=black stroke-width=6 />"
        "<circle id=all cx=90 cy=30 r=20 fill=none pointer-events=all />"
        "<rect id=hidden x=120 width=20 height=20 visibility=hidden />"
        "<rect id=painted x=120 y=30 width=20 height=20 visibility=hidden pointer-events=painted />"
        "<g pointer-events=none><rect id=none x=160 width=20 height=20 />"
        "<rect id=back x=160 y=30 width=20 height=20 pointer-events=auto /></g></svg>");
    CHECK(hitAt(page, 30, 30) == "s");
    CHECK(hitAt(page, 30, 11) == "ring");
    CHECK(hitAt(page, 90, 30) == "all");
    CHECK(hitAt(page, 130, 10) == "s");
    CHECK(hitAt(page, 130, 40) == "painted");
    CHECK(hitAt(page, 170, 10) == "s");
    CHECK(hitAt(page, 170, 40) == "back");
}

TEST_CASE("a hit follows transforms and viewports, and a viewport's overflow clips it") {
    ViewedSvg page("<svg id=s width=100 height=40>"
        "<g transform='translate(40 0)'><svg x=0 y=0 width=20 height=20 viewBox='0 0 10 10'>"
        "<rect id=inner width=5 height=5 /><rect id=outside x=12 width=5 height=5 /></svg></g>"
        "<rect id=turned x=0 y=0 width=10 height=10 transform='translate(10 20) rotate(45)' /></svg>");
    CHECK(hitAt(page, 42, 2) == "inner");
    CHECK(hitAt(page, 55, 15) == "s");
    CHECK(hitAt(page, 66, 2) == "s");
    CHECK(hitAt(page, 10, 26) == "turned");
    CHECK(hitAt(page, 14, 21) == "s");
}

TEST_CASE("a clip path clips where SVG content is hit, and a mask and a filter do not") {
    ViewedSvg page("<svg id=s width=200 height=40>"
        "<clipPath id=left clipPathUnits=objectBoundingBox><rect width=0.5 height=1 /></clipPath>"
        "<mask id=clear x=0 y=0 width=1 height=1 maskContentUnits=objectBoundingBox><rect width=0.5 height=1 fill=white /></mask>"
        "<filter id=shifted x=-1 y=-1 width=3 height=3><feOffset dx=40 /></filter>"
        "<rect id=clipped width=40 height=40 clip-path='url(#left)' />"
        "<rect id=masked x=50 width=40 height=40 mask='url(#clear)' />"
        "<rect id=filtered x=100 width=40 height=40 filter='url(#shifted)' />"
        "<rect id=shape x=150 width=40 height=40 style='clip-path: circle(10px at 20px 20px)' /></svg>");
    CHECK(hitAt(page, 10, 20) == "clipped");
    CHECK(hitAt(page, 30, 20) == "s");
    CHECK(hitAt(page, 80, 20) == "masked");
    CHECK(hitAt(page, 110, 20) == "filtered");
    CHECK(hitAt(page, 150, 20) == "s");
    CHECK(hitAt(page, 170, 20) == "shape");
    CHECK(hitAt(page, 152, 2) == "s");
}

TEST_CASE("a use element's clip path applies where it is hit after its x and y, and its client rect holds them") {
    // The clip is 0 to 40 in the space that x moves by 60, and so 120 to 200 once scaled: the instance, at 130 to 170,
    // is whole.
    ViewedSvg page("<svg id=s width=300 height=60><defs><rect id=r x=5 y=5 width=20 height=10 />"
        "<clipPath id=c><rect width=40 height=100 /></clipPath></defs>"
        "<use id=u href='#r' x=60 y=10 transform='scale(2 1)' clip-path='url(#c)' /></svg>");
    CHECK(hitAt(page, 135, 20) == "u");
    CHECK(hitAt(page, 165, 20) == "u");
    CHECK(hitAt(page, 30, 20) == "s");
    PrDomRect client = prCssElementGetBoundingClientRect(page.byId("u"));
    CHECK(client.x == doctest::Approx(130));
    CHECK(client.width == doctest::Approx(40));
}

TEST_CASE("text is hit by its characters' cells, the innermost element's, and markers are not hit") {
    ViewedSvg page("<svg id=s width=200 height=60><marker id=m markerWidth=20 markerHeight=20>"
        "<rect id=mark width=20 height=20 /></marker>"
        "<text id=t x=10 y=40 font-size=30>II<tspan id=span>II</tspan></text>"
        "<line id=line x1=150 y1=10 x2=190 y2=10 stroke=black marker-end='url(#m)' /></svg>");
    PrDomRect text = prCssElementGetBoundingClientRect(page.byId("t"));
    PrDomRect span = prCssElementGetBoundingClientRect(page.byId("span"));
    REQUIRE(span.width > 0);
    CHECK(span.x > text.x);
    CHECK(hitAt(page, text.x + 1, text.y + text.height / 2) == "t");
    CHECK(hitAt(page, span.x + span.width / 2, span.y + span.height / 2) == "span");
    CHECK(hitAt(page, 170, 10) == "line");
    CHECK(hitAt(page, 195, 20) == "s");
}

TEST_CASE("a foreignObject's content is hit in its rectangle, before the foreignObject") {
    ViewedSvg page("<svg id=s width=100 height=60><foreignObject id=f x=20 y=20 width=40 height=20>"
        "<div id=d style='height: 10px'></div></foreignObject></svg>");
    CHECK(hitAt(page, 30, 25) == "d");
    CHECK(hitAt(page, 30, 35) == "f");
    CHECK(hitAt(page, 10, 10) == "s");
}

TEST_CASE("an SVG element's client rect is its object bounding box, mapped through every transform") {
    ViewedSvg page("<div style='height: 10px'></div><svg width=100 height=60 viewBox='0 0 50 30' "
        "style='padding: 5px'><rect id=r x=5 y=5 width=10 height=10 stroke=black stroke-width=4 />"
        "<g id=g><rect x=20 y=5 width=5 height=5 /><rect x=30 y=20 width=5 height=5 /></g>"
        "<rect id=gone width=5 height=5 display=none /></svg>");
    PrDomRect r = prCssElementGetBoundingClientRect(page.byId("r"));
    CHECK(r.x == doctest::Approx(15));
    CHECK(r.y == doctest::Approx(25));
    CHECK(r.width == doctest::Approx(20));
    CHECK(r.height == doctest::Approx(20));
    PrDomRect g = prCssElementGetBoundingClientRect(page.byId("g"));
    CHECK(g.x == doctest::Approx(45));
    CHECK(g.width == doctest::Approx(30));
    CHECK(g.height == doctest::Approx(40));
    CHECK(prCssElementGetClientRects(page.byId("gone"), nullptr, 0) == 0);
}

TEST_CASE("an SVG element's client rect and hit test follow its svg element's box when a viewBox sizes the content") {
    ViewedSvg page("<div id=w style='width: 100px'><svg viewBox='0 0 10 10' style='width: 100%'>"
        "<rect id=r x=5 y=5 width=5 height=5 /></svg></div>");
    PrDomRect before = prCssElementGetBoundingClientRect(page.byId("r"));
    CHECK(before.x == doctest::Approx(50));
    CHECK(before.width == doctest::Approx(50));
    // The box doubles, and the viewBox scales the content with it, whose layout reads nothing of the box's size.
    page.set("w", "style", "width: 200px");
    page.update();
    PrDomRect after = prCssElementGetBoundingClientRect(page.byId("r"));
    CHECK(after.x == doctest::Approx(100));
    CHECK(after.width == doctest::Approx(100));
    CHECK(prCssDocumentElementFromPoint(page.document(), 150, 150) == page.byId("r"));
}

TEST_CASE("the pointer hovers and clicks SVG content, and a click on an SVG link follows its href") {
    ViewedSvg page("<svg width=100 height=40><a id=a href='#t'><rect id=r width=20 height=20 /></a>"
        "<rect id=other x=40 width=20 height=20 /></svg><p id=t>target</p>");
    Heard heard;
    PrDomEventListener listener = recorder(&heard);
    prDomEventTargetAddEventListener(prDomNodeAsEventTarget(page.node("other")), "click", &listener, nullptr);
    PrAppEvent move{.kind = PrAppEventKind_PointerMove, .pointer = {.position = PrVec2{10, 10}}};
    prHtmlViewEvent(page.view, &move, 0);
    prHtmlViewUpdate(page.view);
    CHECK(page.matches("r", ":hover"));
    CHECK(page.matches("a", ":hover"));
    CHECK(page.matches("a", ":link"));
    clickAt(page, PrVec2{50, 10});
    CHECK(heard.types == std::vector<std::string>{"click"});
    CHECK_FALSE(page.matches("t", ":target"));
    clickAt(page, PrVec2{10, 10});
    CHECK(page.matches("t", ":target"));
}

TEST_CASE("an SVG a element inside another link is no link, and an XLink href makes one") {
    ViewedSvg page("<a href='#x'><svg width=10 height=10><a id=nested href='#y'><rect /></a></svg></a>"
        "<svg width=10 height=10><a id=xlink xlink:href='#y'><rect /></a><a id=none><rect /></a></svg>");
    CHECK_FALSE(page.matches("nested", ":link"));
    CHECK_FALSE(page.matches("nested", ":any-link"));
    CHECK(page.matches("xlink", ":link"));
    CHECK_FALSE(page.matches("none", ":any-link"));
}

TEST_CASE("an SVG a element inside a link is restyled as a link once the link loses its href") {
    ViewedSvg page("<style>svg a:link rect { fill: rgb(255, 0, 0) }</style><a id=outer href='#x'><svg width=10 "
        "height=10><a href='#y'><rect width=10 height=10 /></a></svg></a>");
    CHECK(rgbAt(page.paint(), 5, 5) == 0);
    prDomElementRemoveAttribute(page.byId("outer"), "href");
    page.update();
    CHECK(rgbAt(page.paint(), 5, 5) == 0xff0000);
}

TEST_CASE("an instance in a use element's shadow tree is hit, its events retarget to the use, and its original's "
        "listeners hear them") {
    ViewedSvg page("<svg width=100 height=40><defs><rect id=button width=20 height=20 /></defs>"
        "<use id=use href='#button' x=40 /></svg>");
    Heard original;
    PrDomEventListener originalListener = recorder(&original);
    prDomEventTargetAddEventListener(prDomNodeAsEventTarget(page.node("button")), "click", &originalListener, nullptr);
    PrDomEventTarget* target = nullptr;
    PrDomEventListener documentListener{
        [](void* context, PrDomEvent* event) {
            *static_cast<PrDomEventTarget**>(context) = prDomEventTarget(event);
        },
        &target,
        0,
        nullptr,
        nullptr,
    };
    prDomEventTargetAddEventListener(prDomNodeAsEventTarget(prDomDocumentAsNode(page.document())), "click",
        &documentListener, nullptr);
    CHECK(hitAt(page, 50, 10) == "use");
    clickAt(page, PrVec2{50, 10});
    CHECK(original.types == std::vector<std::string>{"click"});
    CHECK(target == prDomNodeAsEventTarget(page.node("use")));
    clickAt(page, PrVec2{10, 10});
    CHECK(original.types.size() == 1);
}

TEST_CASE("an SVG image element fires load or error once what its href names has loaded, whether or not it renders") {
    PrCompressCodec* compress = prCompressBuiltinCreate();
    PrImageCodec* images = prImageBuiltinCreate(compress);
    std::vector<uint8_t> red(4 * 4, 255);
    PrPixelView pixels{red.data(), 8, {2, 2}};
    size_t size = 0;
    REQUIRE(prImageCodecEncode(images, "image/png", pixels, nullptr, 0, &size) == PrStatus_Ok);
    std::vector<uint8_t> png(size);
    REQUIRE(prImageCodecEncode(images, "image/png", pixels, png.data(), size, &size) == PrStatus_Ok);
    {
        Viewed page("<svg width=40 height=40><image id=found href=http://test/red.png width=40 height=40 />"
            "<image id=missing href=http://test/missing.png display=none /></svg>", images, &png, true);
        Heard found;
        Heard missing;
        PrDomEventListener foundListener = recorder(&found);
        PrDomEventListener missingListener = recorder(&missing);
        for (const char* type : {"load", "error"}) {
            prDomEventTargetAddEventListener(prDomNodeAsEventTarget(prDomElementAsNode(page.byId("found"))), type,
                &foundListener, nullptr);
            prDomEventTargetAddEventListener(prDomNodeAsEventTarget(prDomElementAsNode(page.byId("missing"))), type,
                &missingListener, nullptr);
        }
        prHtmlViewUpdate(page.view);
        page.settle();
        REQUIRE(page.fetcher.waiting() == 2);
        std::vector<PrFetchInterceptRequest*> answering = page.fetcher.held;
        for (PrFetchInterceptRequest* request : answering) {
            if (TestFetcher::urlOf(request).find("missing") != std::string::npos)
                page.fetcher.respondTo(request, "", 404);
            else
                page.respond(request);
        }
        PrDomAgent* agent = prHtmlPageAgent(page.page);
        page.fetcher.settle(agent);
        CHECK(found.types == std::vector<std::string>{"load"});
        CHECK(missing.types == std::vector<std::string>{"error"});

        // A new href fires the event again. An href that names an image that has already loaded fires at once.
        prDomElementSetAttribute(page.byId("missing"), "href", "http://test/red.png");
        page.fetcher.settle(agent);
        CHECK(missing.types == std::vector<std::string>{"error", "load"});
    }
    prImageCodecDestroy(images);
    prCompressCodecDestroy(compress);
}

TEST_CASE("pointer-events: none lets the pointer through a box to what lies beneath, and a descendant may take it") {
    Viewed page("<style>body { margin: 0 } div { position: absolute; width: 40px; height: 40px }</style>"
        "<div id=under></div><div id=over style='pointer-events: none'>"
        "<span id=taker style='pointer-events: auto'>x</span></div>");
    prHtmlViewUpdate(page.view);
    page.settle();
    CHECK(hitAt(page, 30, 30) == "under");
    PrDomRect taker = prCssElementGetBoundingClientRect(page.byId("taker"));
    CHECK(hitAt(page, taker.x + taker.width / 2, taker.y + taker.height / 2) == "taker");
}

TEST_CASE("an svg element that takes no pointer lets it through, and its content may take it back") {
    Viewed page("<style>body { margin: 0 } div { position: absolute; width: 40px; height: 40px }"
        "svg { position: absolute; display: block }</style><div id=under></div>"
        "<svg id=svg width=40 height=40 style='pointer-events: none'><rect id=none width=20 height=40 />"
        "<rect id=taker x=20 width=20 height=40 pointer-events=auto /></svg>"
        "<img id=img style='position: absolute; top: 50px; width: 10px; height: 10px; pointer-events: none'>");
    prHtmlViewUpdate(page.view);
    page.settle();
    CHECK(hitAt(page, 10, 20) == "under");
    CHECK(hitAt(page, 30, 20) == "taker");
    CHECK(hitAt(page, 5, 55) != "img");
}

TEST_CASE("a paged view lays its document out in the page boxes that its @page rules give, and paints each page") {
    Viewed v(R"(<style>
        @page { size: 200px 300px; margin: 20px }
        @page :first { margin-top: 50px }
        @page wide { size: 400px 300px }
        html, body { margin: 0 }
        div { height: 100px; background: red }
        .after { break-before: page }
        .wide { page: wide }
    </style><div></div><div></div><div></div><div class=after></div><div class=wide></div>)");
    PrLayoutPageBox initial = prLayoutPageBoxDefault();
    prHtmlViewSetPaged(v.view, &initial, 0);
    // The first page's area is 230px tall and holds two blocks and the start of the third, the second's holds the rest
    // of the third, a forced break puts the fourth on the third page, and a block of another page name goes on a page
    // of that name.
    REQUIRE(prHtmlViewPageCount(v.view) == 4);
    PrLayoutPage first = prHtmlViewPage(v.view, 0);
    CHECK(prLayoutUnitToPx(first.box.extent.x) == 200);
    CHECK(prLayoutUnitToPx(first.box.margin[PrRectSide_Top]) == 50);
    CHECK(prLayoutUnitToPx(prHtmlViewPage(v.view, 1).box.margin[PrRectSide_Top]) == 20);
    PrLayoutPage wide = prHtmlViewPage(v.view, 3);
    CHECK(prLayoutUnitToPx(wide.box.extent.x) == 400);
    PrDrawList* list = prDrawListCreate();
    prHtmlViewPaintPage(v.view, 0, list);
    std::vector<uint8_t> drawn(size_t(200) * 300 * 4, 0);
    PrDrawRenderer* cpu = prDrawCpuCreate(nullptr);
    PrPixels into{drawn.data(), 200 * 4, {200, 300}};
    prDrawRendererRenderPixels(cpu, list, into, PrPixelFormat_Rgba8);
    prDrawRendererDestroy(cpu);
    prDrawListRelease(list);
    // The blocks are red, and nothing fills the margins. The third block's fixed height breaks at the end of the first
    // page's area.
    auto rgba = [&](uint32_t x, uint32_t y) {
        const uint8_t* p = &drawn[(size_t(y) * 200 + x) * 4];
        return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    };
    CHECK(rgba(100, 60) == 0xff0000ffu);
    CHECK(rgba(100, 240) == 0xff0000ffu);
    CHECK(rgba(100, 260) == 0xff0000ffu);
    CHECK(rgba(100, 290) == 0u);
    CHECK(rgba(10, 60) == 0u);
    prHtmlViewSetPaged(v.view, nullptr, 0);
    CHECK(prHtmlViewPageCount(v.view) == 0);
}

TEST_CASE("a paged view centers its pages across a viewport whose extent changes") {
    // The pages' boxes are the same in any viewport, so a new extent changes no style or box. Only the pages' places
    // change.
    Viewed v(R"(<style>@page { size: 200px 300px; margin: 0 }</style><p>text)");
    PrLayoutPageBox initial = prLayoutPageBoxDefault();
    prHtmlViewSetPaged(v.view, &initial, 0);
    prHtmlViewSetViewport(v.view, PrVec2{800, 600});
    REQUIRE(prHtmlViewPageCount(v.view) == 1);
    CHECK(prLayoutUnitToPx(prHtmlViewPage(v.view, 0).offset.x) == 300);
    prHtmlViewSetViewport(v.view, PrVec2{400, 600});
    REQUIRE(prHtmlViewPageCount(v.view) == 1);
    CHECK(prLayoutUnitToPx(prHtmlViewPage(v.view, 0).offset.x) == 100);
    prHtmlViewSetPaged(v.view, nullptr, 0);
}

TEST_CASE("@page's marks and bleed give a page's box its marks and its bleed, which is 6pt for crop marks by default") {
    struct Expected {
        const char* rule;
        PrLayoutPageMarkFlags marks;
        float bleed;                // px
    };
    const Expected cases[] = {
        {"", PrLayoutPageMarkFlag_None, 0},
        {"marks: crop", PrLayoutPageMarkFlag_Crop, 8},
        {"marks: cross", PrLayoutPageMarkFlag_Cross, 0},
        {"marks: cross crop", PrLayoutPageMarkFlag_All, 8},
        {"marks: none; bleed: 3pt", PrLayoutPageMarkFlag_None, 4},
        {"marks: crop; bleed: -6px", PrLayoutPageMarkFlag_Crop, -6},
        {"marks: crop; bleed: auto", PrLayoutPageMarkFlag_Crop, 8},
    };
    for (const Expected& expected : cases) {
        CAPTURE(expected.rule);
        std::string html = std::string("<style>@page { size: 100px 60px; ") + expected.rule + " }</style>x";
        Viewed v(html.c_str());
        PrLayoutPageBox initial = prLayoutPageBoxDefault();
        prHtmlViewSetPaged(v.view, &initial, 0);
        REQUIRE(prHtmlViewPageCount(v.view) == 1);
        PrLayoutPageBox box = prHtmlViewPage(v.view, 0).box;
        CHECK(box.marks == expected.marks);
        CHECK(prLayoutUnitToPx(box.bleed) == expected.bleed);
    }
}

TEST_CASE("a paged view prints and previews each page on its sheet, with its marks and its content in the bleed area") {
    Viewed v(R"(<style>
        @page { size: 100px 60px; margin: 10px; bleed: 4px; marks: crop; background: blue }
        html, body { margin: 0 }
        div { margin-left: -20px; width: 30px; height: 20px; background: red }
    </style><div></div>)");
    PrLayoutPageBox initial = prLayoutPageBoxDefault();
    prHtmlViewSetPaged(v.view, &initial, 0);
    REQUIRE(prHtmlViewPageCount(v.view) == 1);
    // A crop mark starts 8 px from the page box, outside the 4 px bleed, runs 32 px, and the sheet ends 8 px past it.
    PrLayoutPageBox box = prHtmlViewPage(v.view, 0).box;
    PrLayoutPageSheet sheet = prLayoutPageBoxSheet(&box);
    REQUIRE(prLayoutVecToPx(sheet.extent) == PrVec2{196, 156});
    CHECK(prLayoutRectToPx(sheet.trim).left == 48);
    CHECK(prLayoutRectToPx(sheet.bleed).left == 44);
    // Returns the RGBA pixels of `list` drawn into `width` by `height` px.
    auto render = [](PrDrawList* list, uint32_t width, uint32_t height) {
        std::vector<uint8_t> drawn(size_t(width) * height * 4, 0);
        PrDrawRenderer* cpu = prDrawCpuCreate(nullptr);
        PrPixels into{drawn.data(), size_t(width) * 4, {width, height}};
        prDrawRendererRenderPixels(cpu, list, into, PrPixelFormat_Rgba8);
        prDrawRendererDestroy(cpu);
        prDrawListRelease(list);
        return drawn;
    };
    // Returns the color of the pixel at `x`, `y` as 0xRRGGBBAA.
    auto rgba = [](const std::vector<uint8_t>& drawn, uint32_t width, uint32_t x, uint32_t y) {
        const uint8_t* p = &drawn[(size_t(y) * width + x) * 4];
        return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
    };
    PrDrawList* list = prDrawListCreate();
    prHtmlViewPaintPage(v.view, 0, list);
    std::vector<uint8_t> printed = render(list, 196, 156);
    // The block overflows the page box into the bleed area and is clipped at its edge, and the page's background fills
    // the rest of the bleed area. A crop mark extends the page box's left edge above it.
    CHECK(rgba(printed, 196, 46, 65) == 0xff0000ffu);
    CHECK(rgba(printed, 196, 40, 65) == 0u);
    CHECK(rgba(printed, 196, 46, 90) == 0x0000ffffu);
    CHECK((rgba(printed, 196, 47, 20) | rgba(printed, 196, 48, 20)) != 0u);
    CHECK(rgba(printed, 196, 48, 42) == 0u);
    // The preview shows the same sheet on white paper, with the page box at the page's offset, over the backdrop.
    list = prDrawListCreate();
    prHtmlViewPaint(v.view, list);
    std::vector<uint8_t> shown = render(list, 800, 600);
    PrVec2 offset = prLayoutVecToPx(prHtmlViewPage(v.view, 0).offset);
    uint32_t x = uint32_t(offset.x) - 48;
    uint32_t y = uint32_t(offset.y) - 48;
    REQUIRE(x + 200 <= 800);
    REQUIRE(y + 156 <= 600);
    CHECK(rgba(shown, 800, x + 46, y + 65) == 0xff0000ffu);
    CHECK(rgba(shown, 800, x + 40, y + 65) == 0xffffffffu);
    CHECK(rgba(shown, 800, x + 46, y + 90) == 0x0000ffffu);
    CHECK((rgba(shown, 800, x + 47, y + 20) & rgba(shown, 800, x + 48, y + 20)) != 0xffffffffu);
    CHECK(rgba(shown, 800, x + 196 + 2, y + 20) != 0xffffffffu);
}

TEST_CASE("a paged view's viewport units are of the first page's area, and its media queries see the paper") {
    // The first page's area is 280 by 180 px once the @page rules apply. The paper is portrait, so #b matches the query
    // in print, and the screen's 800 by 600 viewport is landscape.
    Viewed v(R"(<style>
        @page { size: 300px 200px; margin: 20px 30px }
        @page :first { margin: 10px }
        html, body { margin: 0 }
        #a { width: 50vw; height: 50vh }
        #b { height: 10px }
        @media (orientation: portrait) { #b { width: 10px } }
    </style><div id=a></div><div id=b></div>)");
    auto extent = [&](const char* id) {
        PrDomRect rect = prCssElementGetBoundingClientRect(v.byId(id));
        return std::to_string(int(rect.width)) + "x" + std::to_string(int(rect.height));
    };
    CHECK(extent("a") == "400x300");
    CHECK(extent("b") == "800x10");
    PrLayoutPageBox paper = prLayoutPageBoxDefault();
    paper.extent = PrLayoutVec{prLayoutUnitFromPx(200), prLayoutUnitFromPx(400)};
    prHtmlViewSetPaged(v.view, &paper, 0);
    REQUIRE(prHtmlViewPageCount(v.view) == 1);
    CHECK(extent("a") == "140x90");
    CHECK(extent("b") == "10x10");
    // The screen's viewport applies again once the view shows the screen.
    prHtmlViewSetPaged(v.view, nullptr, 0);
    CHECK(extent("a") == "400x300");
    CHECK(extent("b") == "800x10");
}

TEST_CASE("a paged view's first page takes the name of the root's start page value, which sizes viewport units") {
    Viewed v(R"(<style>
        @page { size: 300px 200px; margin: 0 }
        @page wide { size: 600px 200px }
        html, body { margin: 0 }
        #a { width: 10vw; height: 10px; page: wide }
    </style><div id=a></div>)");
    PrLayoutPageBox paper = prLayoutPageBoxDefault();
    prHtmlViewSetPaged(v.view, &paper, 0);
    REQUIRE(prHtmlViewPageCount(v.view) == 1);
    CHECK(prLayoutUnitToPx(prHtmlViewPage(v.view, 0).box.extent.x) == 600);
    CHECK(prCssElementGetBoundingClientRect(v.byId("a")).width == 60.0);
    prHtmlViewSetPaged(v.view, nullptr, 0);
}

TEST_CASE("a paged view starts a page wherever the used page value changes between siblings") {
    // The body forces no break by its break values. The page value of the first block differs from the body's, and the
    // text after the second block is an anonymous block box of the body's page value, so each starts a page. The white
    // space between the blocks generates no box.
    Viewed v(R"(<style>
        @page { size: 200px 300px; margin: 20px }
        @page b { size: 400px 300px }
        html, body { margin: 0 }
    </style>
    <div style="page: b">a</div>
    <div><div style="page: b">b</div></div>
    text)");
    PrLayoutPageBox initial = prLayoutPageBoxDefault();
    prHtmlViewSetPaged(v.view, &initial, 0);
    REQUIRE(prHtmlViewPageCount(v.view) == 2);
    CHECK(prLayoutUnitToPx(prHtmlViewPage(v.view, 0).box.extent.x) == 400);
    CHECK(prLayoutUnitToPx(prHtmlViewPage(v.view, 1).box.extent.x) == 200);
    prHtmlViewSetPaged(v.view, nullptr, 0);
}

TEST_CASE("a paged view starts a titled sidebar's long list on the page where the sidebar starts") {
    // The markup of an Asciidoctor sidebar, whose title avoids a break after it. The page areas are 300 px tall: 150 px
    // of filler, the sidebar's 2 px border and 20 px title, and 30 items of 20 px fill two pages and a little of a
    // third.
    Viewed v(R"(<style>
        @page { size: 200px 300px; margin: 0 }
        html, body { margin: 0 }
        .filler { height: 150px }
        .sidebarblock { border: 1px solid }
        .title { height: 20px; break-after: avoid }
        ul, p { margin: 0 }
        p { height: 20px }
    </style><div class=filler></div><div class=sidebarblock><div class=content><div class=title></div>
        <div class=ulist><ul>)"
        "<li><p></p><li><p></p><li><p></p><li><p></p><li><p></p><li><p></p><li><p></p><li><p></p><li><p></p><li><p></p>"
        "<li><p></p><li><p></p><li><p></p><li><p></p><li><p></p><li><p></p><li><p></p><li><p></p><li><p></p><li><p></p>"
        "<li><p></p><li><p></p><li><p></p><li><p></p><li><p></p><li><p></p><li><p></p><li><p></p><li><p></p><li><p></p>"
        "</ul></div></div></div>");
    PrLayoutPageBox initial = prLayoutPageBoxDefault();
    prHtmlViewSetPaged(v.view, &initial, 0);
    CHECK(prHtmlViewPageCount(v.view) == 3);
    prHtmlViewSetPaged(v.view, nullptr, 0);
}

TEST_CASE("a paged view keeps a heading on the page of the content after it") {
    Viewed v(R"(<style>
        @page { size: 200px 300px; margin: 0 }
        html, body { margin: 0 }
        div { height: 220px }
        h2 { height: 40px; margin: 0; background: red }
        p { height: 100px; margin: 0; overflow: auto }
    </style><div></div><h2></h2><p></p>)");
    PrLayoutPageBox initial = prLayoutPageBoxDefault();
    prHtmlViewSetPaged(v.view, &initial, 0);
    REQUIRE(prHtmlViewPageCount(v.view) == 2);
    auto red = [&](uint32_t page, uint32_t y) {
        PrDrawList* list = prDrawListCreate();
        prHtmlViewPaintPage(v.view, page, list);
        std::vector<uint8_t> drawn(size_t(200) * 300 * 4, 0);
        PrDrawRenderer* cpu = prDrawCpuCreate(nullptr);
        PrPixels into{drawn.data(), 200 * 4, {200, 300}};
        prDrawRendererRenderPixels(cpu, list, into, PrPixelFormat_Rgba8);
        prDrawRendererDestroy(cpu);
        prDrawListRelease(list);
        const uint8_t* p = &drawn[(size_t(y) * 200 + 100) * 4];
        return p[0] == 255 && p[1] == 0 && p[2] == 0;
    };
    // The heading would fit at the end of the first page, but the user agent's break-after: avoid-page moves it to the
    // second, with the paragraph after it, which does not fit on the first and, as a scroll container, cannot break.
    CHECK(!red(0, 240));
    CHECK(red(1, 20));
    prHtmlViewSetPaged(v.view, nullptr, 0);
}

TEST_CASE("a paged view draws the content of its pages' margin boxes, with the page and pages counters") {
    Viewed v(R"(<style>
        @page { size: 200px 300px; margin: 40px 20px;
            @bottom-center { content: "Page " counter(page) " of " counter(pages); font-size: 12px; color: blue }
            @top-right { content: counter(pages); color: blue } }
        @page :first { @top-right { content: none } }
        html, body { margin: 0 }
        div { height: 200px }
    </style><div></div><div></div><div></div>)");
    PrLayoutPageBox initial = prLayoutPageBoxDefault();
    prHtmlViewSetPaged(v.view, &initial, 0);
    REQUIRE(prHtmlViewPageCount(v.view) == 3);
    // Paints the page at `index`, and returns the number of its blue pixels in the rows from `top` to `bottom`, and the
    // columns that they span.
    auto blueIn = [&](uint32_t index, uint32_t top, uint32_t bottom, uint32_t* left, uint32_t* right) {
        PrDrawList* list = prDrawListCreate();
        prHtmlViewPaintPage(v.view, index, list);
        std::vector<uint8_t> drawn(size_t(200) * 300 * 4, 0);
        PrDrawRenderer* cpu = prDrawCpuCreate(nullptr);
        PrPixels into{drawn.data(), 200 * 4, {200, 300}};
        prDrawRendererRenderPixels(cpu, list, into, PrPixelFormat_Rgba8);
        prDrawRendererDestroy(cpu);
        prDrawListRelease(list);
        uint32_t count = 0;
        *left = 200;
        *right = 0;
        for (uint32_t y = top; y < bottom; ++y) {
            for (uint32_t x = 0; x < 200; ++x) {
                const uint8_t* p = &drawn[(size_t(y) * 200 + x) * 4];
                if (p[3] != 0 && p[2] > p[0]) {
                    ++count;
                    *left = x < *left ? x : *left;
                    *right = x + 1 > *right ? x + 1 : *right;
                }
            }
        }
        return count;
    };
    uint32_t left = 0;
    uint32_t right = 0;
    // The first page's top right margin box has no content, and later pages' show the number of pages at the right of
    // the page area.
    CHECK(blueIn(0, 0, 40, &left, &right) == 0);
    CHECK(blueIn(1, 0, 40, &left, &right) > 0);
    CHECK(left > 150);
    CHECK(right <= 180);
    // The bottom margin holds the text, centered in the page area.
    CHECK(blueIn(1, 260, 300, &left, &right) > 0);
    CHECK(left > 20);
    CHECK(right < 180);
    CHECK(left + right > 180);
    CHECK(left + right < 220);
    prHtmlViewSetPaged(v.view, nullptr, 0);
}

TEST_CASE("a page's margin boxes show their strings and counters, and a box with no content is not generated") {
    Viewed v(R"(<style>
        @page { size: 200px 300px; margin: 40px 20px;
            @top-left { content: "Doc" }
            @top-right { content: counter(page, upper-roman) "/" counter(pages) }
            @bottom-center { content: normal }
            @left-middle { content: "" } }
        @page :left { counter-increment: page 2 }
        html, body { margin: 0 }
        div { height: 200px }
    </style><div></div><div></div><div></div>)");
    REQUIRE(paged(v) == 3);
    // A right page counts by one, and a left page by two, as its page context says.
    CHECK(marginText(v, 0, PrLayoutMarginBox_TopRight) == "I/3");
    CHECK(marginText(v, 1, PrLayoutMarginBox_TopRight) == "III/3");
    CHECK(marginText(v, 2, PrLayoutMarginBox_TopRight) == "IV/3");
    CHECK(marginText(v, 2, PrLayoutMarginBox_TopLeft) == "Doc");
    // Content normal computes to none, and generates no box. An empty string generates an empty box.
    CHECK(marginText(v, 0, PrLayoutMarginBox_BottomCenter) == "-");
    CHECK(marginText(v, 0, PrLayoutMarginBox_LeftMiddle).empty());
    CHECK(marginText(v, 0, PrLayoutMarginBox_TopCenter) == "-");
    // Printing again keeps the boxes that are unchanged.
    PrLayoutTree* tree = prCssPresentationTree(prHtmlPagePresentation(v.page));
    PrLayoutNode before[PrLayoutMarginBox_Count];
    prLayoutTreePageMargins(tree, 1, before);
    REQUIRE(prHtmlViewPageCount(v.view) == 3);
    PrLayoutNode after[PrLayoutMarginBox_Count];
    prLayoutTreePageMargins(tree, 1, after);
    CHECK(before[PrLayoutMarginBox_TopRight] == after[PrLayoutMarginBox_TopRight]);
    prHtmlViewSetPaged(v.view, nullptr, 0);
    CHECK(prHtmlViewPageCount(v.view) == 0);
}

TEST_CASE("a margin box shows the document's counters as they are at the start of its page") {
    Viewed v(R"(<style>
        @page { size: 200px 300px; margin: 40px 20px; @top-center { content: "Section " counter(section) } }
        html, body { margin: 0 }
        h2 { counter-increment: section; height: 100px; margin: 0 }
        body { counter-reset: section 10 }
    </style><h2>A</h2><h2>B</h2><h2>C</h2><h2>D</h2><h2>E</h2>)");
    // Each page area holds two headings. The counter at the start of a page is the value before its first heading.
    REQUIRE(paged(v) == 3);
    CHECK(marginText(v, 0, PrLayoutMarginBox_TopCenter) == "Section 10");
    CHECK(marginText(v, 1, PrLayoutMarginBox_TopCenter) == "Section 12");
    CHECK(marginText(v, 2, PrLayoutMarginBox_TopCenter) == "Section 14");
    prHtmlViewSetPaged(v.view, nullptr, 0);
}

TEST_CASE("named strings take the assignment that first, start, last and first-except name on each page") {
    Viewed v(R"(<style>
        @page { size: 200px 300px; margin: 40px 20px;
            @top-left { content: string(title) }
            @top-center { content: string(title, start) }
            @top-right { content: string(title, last) }
            @bottom-left { content: string(title, first-except) } }
        html, body { margin: 0 }
        h1 { string-set: title content(); height: 50px; margin: 0; font-size: 10px }
        p { height: 100px; margin: 0 }
        .first { break-before: page }
    </style><h1>One</h1><h1>Two</h1><p class=first></p><h1 class=first>Three</h1>)");
    // Page one sets One and Two, page two sets none, and page three starts with Three.
    REQUIRE(paged(v) == 3);
    std::vector<std::string> one = marginTexts(v, 0);
    CHECK(one[PrLayoutMarginBox_TopLeft] == "One");
    CHECK(one[PrLayoutMarginBox_TopCenter] == "One");
    CHECK(one[PrLayoutMarginBox_TopRight] == "Two");
    CHECK(one[PrLayoutMarginBox_BottomLeft].empty());
    std::vector<std::string> two = marginTexts(v, 1);
    CHECK(two[PrLayoutMarginBox_TopLeft] == "Two");
    CHECK(two[PrLayoutMarginBox_TopCenter] == "Two");
    CHECK(two[PrLayoutMarginBox_TopRight] == "Two");
    CHECK(two[PrLayoutMarginBox_BottomLeft] == "Two");
    std::vector<std::string> three = marginTexts(v, 2);
    CHECK(three[PrLayoutMarginBox_TopCenter] == "Three");
    CHECK(three[PrLayoutMarginBox_BottomLeft].empty());
    prHtmlViewSetPaged(v.view, nullptr, 0);
}

TEST_CASE("a running element leaves the flow, and a copy of it shows in the margin boxes of its pages") {
    Viewed v(R"(<style>
        @page { size: 200px 300px; margin: 40px 20px; @top-center { content: element(header) } }
        html, body { margin: 0 }
        .header { position: running(header); font-size: 10px }
        .page { height: 100px; break-before: page }
    </style><div class=header>First <a href="https://example.com/">link</a></div><div class=page id=a>A</div>
        <div class=page>B</div><div class=page><div class=header>Second</div>C</div>)");
    REQUIRE(paged(v) == 3);
    CHECK(marginText(v, 0, PrLayoutMarginBox_TopCenter) == "First link");
    CHECK(marginText(v, 1, PrLayoutMarginBox_TopCenter) == "First link");
    CHECK(marginText(v, 2, PrLayoutMarginBox_TopCenter) == "Second");
    // The running element takes no room in the flow.
    CHECK(prCssElementGetBoundingClientRect(v.byId("a")).y == 40.0);
    // The structure makes a link of each copy of the link, and leaves the rest of the margin boxes as artifacts.
    PrDrawStructure* structure = prDrawStructureCreate(nullptr);
    prHtmlStructureAppend(v.page, structure);
    PrRawString dump = prDrawStructureDump(structure);
    std::string text(prRawStringView(&dump).data, prRawStringView(&dump).size);
    size_t links = 0;
    for (size_t at = text.find("Link"); at != std::string::npos; at = text.find("Link", at + 1))
        ++links;
    CHECK(links == 2);
    CHECK(text.find("First") == std::string::npos);
    prRawStringReset(&dump);
    prDrawStructureDestroy(structure);
    prHtmlViewSetPaged(v.view, nullptr, 0);
}

TEST_CASE("a margin box shows an image that its content names, once the image loads") {
    Viewed v(R"(<style>
        @page { size: 200px 300px; margin: 40px 20px;
            @top-left { content: "A" url("data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' width='10' height='10'/>") } }
        html, body { margin: 0 }
    </style><div>A</div>)");
    REQUIRE(paged(v) == 1);
    PrLayoutTree* tree = prCssPresentationTree(prHtmlPagePresentation(v.page));
    auto replacedIn = [&]() {
        PrLayoutNode boxes[PrLayoutMarginBox_Count];
        prLayoutTreePageMargins(tree, 0, boxes);
        REQUIRE(boxes[PrLayoutMarginBox_TopLeft] != kPrLayoutNodeNone);
        uint32_t count = 0;
        for (PrLayoutNode child = prLayoutNodeFirstChild(tree, boxes[PrLayoutMarginBox_TopLeft]);
                child != kPrLayoutNodeNone; child = prLayoutNodeNextSibling(tree, child))
            count += prLayoutNodeKind(tree, child) == PrLayoutNodeKind_Replaced ? 1 : 0;
        return count;
    };
    // The image's fetch is answered in a task, and the next pass shows the image after the text.
    for (uint32_t i = 0; i < 4 && replacedIn() == 0; ++i) {
        v.settle();
        REQUIRE(prHtmlViewPageCount(v.view) == 1);
    }
    CHECK(replacedIn() == 1);
    CHECK(marginText(v, 0, PrLayoutMarginBox_TopLeft) == "A");
    prHtmlViewSetPaged(v.view, nullptr, 0);
}
