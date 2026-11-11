#include "pr/css.h"
#include "pr/css_intersection_observer.h"
#include "pr/css_presentation.h"
#include "pr/css_resize_observer.h"
#include "pr/css_view.h"
#include "pr/draw_commands.h"
#include "pr/draw_list.h"
#include "pr/float.h"
#include "pr/html.h"
#include "pr/html_page.h"
#include "pr/html_view.h"
#include "pr/layout_animation.h"
#include "pr/layout_query.h"
#include "pr/layout_scene.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/locale.h"
#include "pr/text_harfbuzz.h"

#include "css/src/css_presentation.h"

#include "tools/harness/test.h"

#include <atomic>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace pr_impl {

namespace {

// Sets the headroom of the display that the context shows its document on, in sRGB's gamut.
void setHeadroom(PrHtmlPage* page, float headroom) {
    PrAppDisplay display = prAppDisplayDefault();
    display.headroom = headroom;
    prHtmlPageSetDisplay(page, &display);
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

// A host's pool `threads` wide, which runs each loop's indices in turn on the caller's thread, and records the loops
// that it ran and the widest of them, which is the number of threads that the loop divided its work among.
struct HostPool : PrTaskPool {
    uint32_t threads;
    uint32_t loops = 0;
    uint32_t widest = 0;

    explicit HostPool(uint32_t count) : PrTaskPool{}, threads(count) {
        width = [](const PrTaskPool* base) -> uint32_t { return static_cast<const HostPool*>(base)->threads; };
        run = [](PrTaskPool* base, uint32_t count, uint32_t wide, PrTaskFn fn, void* PR_NULLABLE context) {
            HostPool* pool = static_cast<HostPool*>(base);
            ++pool->loops;
            pool->widest = wide > pool->widest ? wide : pool->widest;
            for (uint32_t i = 0; i < count; ++i)
                fn(context, i, i % wide);
        };
        post = [](PrTaskPool*, PrTaskJobFn fn, void* PR_NULLABLE context) { fn(context); };
    }
};

// Returns agent options whose restyles share their work with `pool`.
PrDomAgentOptions withPool(HostPool& pool) {
    PrDomAgentOptions options = prDomAgentOptionsDefault();
    options.restylePool = &pool;
    return options;
}

// Returns the pixels of a length that is in pixels, and NaN for any other length.
float px(PrLayoutLength length) {
    return prLayoutWordKind(length.bits) == PrLayoutWordKind_Px ? prLayoutUnitToPx(prLayoutWordPxValue(length.bits)) : kPrF32Nan;
}

const PrFont* load(const char* name) {
    std::string path = std::string(PR_FONTS_DIR) + "/" + name;
    const PrFont* font = nullptr;
    REQUIRE(prFontCreateFromFile(path, 0, nullptr, &font) == PrStatus_Ok);
    return font;
}

// A document rendered over Tinos for serif text and Arimo for sans-serif, as an update lays it out.
struct Page {
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextHarfBuzzCreate();
    PrTextFont* defaultFont = nullptr;
    PrDomAgent* agent = nullptr;
    PrDomDocument* document = nullptr;
    PrCssPresentation* presentation = nullptr;

    // `sheets` follow HTML's user agent sheets.
    explicit Page(const char* html, PrVec2 viewport = PrVec2{800, 600},
            PrDomAgentOptions agentOptions = prDomAgentOptionsDefault(), const std::vector<PrCssUserAgentSheet>& sheets = {}) {
        const char* files[] = {"Tinos-Regular.ttf", "Arimo-Regular.ttf", "Ahem.ttf"};
        for (const char* file : files) {
            const PrFont* font = load(file);
            PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
            prFontSetAdd(fonts, &descriptor, font);
            prFontRelease(font);
        }
        prFontSetSetGeneric(fonts, PrFontGeneric_Serif, "Tinos");
        prFontSetSetGeneric(fonts, PrFontGeneric_SansSerif, "Arimo");
        prFontSetSetDefaultFamily(fonts, prFontFamilyFromName("Tinos"));
        PrTextFontDesc defaultFontDesc{.set = fonts, .style = prFontStyleDefault()};
        defaultFont = prTextFontCreate(&defaultFontDesc);
        document = prHtmlDomParserParseFromString(html, PrHtmlDomParserSupportedType_TextHtml);
        agent = prDomAgentCreate(&agentOptions);
        uint32_t htmlCount = 0;
        const PrCssUserAgentSheet* htmlSheets = prHtmlUserAgentSheets(&htmlCount);
        std::vector<PrCssUserAgentSheet> all(htmlSheets, htmlSheets + htmlCount);
        all.insert(all.end(), sheets.begin(), sheets.end());
        PrCssPresentationDesc renderDesc{
            .document = document,
            .font = defaultFont,
            .shaper = shaper,
            .agent = agent,
            .userAgentSheets = all.data(),
            .userAgentSheetCount = uint32_t(all.size()),
        };
        presentation = prCssPresentationCreate(&renderDesc);
        prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(viewport));
    }

    ~Page() {
        prCssPresentationDestroy(presentation);
        prDomDocumentRelease(document);
        prDomAgentDestroy(agent);
        prTextFontRelease(defaultFont);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
    }

    // Returns the layout node that presents the element with `id`.
    PrLayoutNode node(const char* id) {
        PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(document), id);
        REQUIRE(element != nullptr);
        return prCssPresentationLayoutNode(presentation, prDomElementAsNode(element));
    }

    PrRect bounds(const char* id) {
        return prLayoutRectToPx(prLayoutNodeBounds(prCssPresentationTree(presentation), node(id),
            PrLayoutCoordinates_Viewport));
    }

    PrLayoutStyle style(const char* id) {
        PrLayoutTree* tree = prCssPresentationTree(presentation);
        return prLayoutStyleRecord(tree, prLayoutNodeStyle(tree, node(id)));
    }

    PrLayoutBoxStyle box(const char* id) {
        return prLayoutBoxStyleRecord(prCssPresentationTree(presentation), style(id).groups[PrLayoutGroup_Box]);
    }
    PrLayoutFontStyle font(const char* id) {
        return prLayoutFontStyleRecord(prCssPresentationTree(presentation), style(id).groups[PrLayoutGroup_Font]);
    }
    PrLayoutFlexStyle flex(const char* id) {
        return prLayoutFlexStyleRecord(prCssPresentationTree(presentation), style(id).groups[PrLayoutGroup_Flex]);
    }
    PrLayoutGridStyle grid(const char* id) {
        return prLayoutGridStyleRecord(prCssPresentationTree(presentation), style(id).groups[PrLayoutGroup_Grid]);
    }
    // Returns the text font of the font group of the element `id`, or the tree's default if the element names none.
    PrTextFont* textFont(const char* id) {
        PrTextFont* PR_NULLABLE named = font(id).font;
        return named != nullptr ? named : prLayoutTreeDefaultFont(prCssPresentationTree(presentation));
    }
    PrFontStyle fontStyle(const char* id) { return prTextFontStyle(textFont(id)); }
    PrLocale locale(const char* id) { return prTextFontLocale(textFont(id)); }
    PrLayoutColorStyle color(const char* id) {
        return prLayoutColorStyleRecord(prCssPresentationTree(presentation), style(id).groups[PrLayoutGroup_Color]);
    }
    PrLayoutDecorationStyle decoration(const char* id) {
        PrLayoutGroupIndex decoration = style(id).groups[PrLayoutGroup_Decoration];
        return prLayoutDecorationStyleRecord(prCssPresentationTree(presentation), decoration);
    }
    PrLayoutEffectStyle effects(const char* id) {
        return prLayoutEffectStyleRecord(prCssPresentationTree(presentation), style(id).groups[PrLayoutGroup_Effect]);
    }
};

bool sameColor(PrColor a, PrColor b) {
    auto near = [](float x, float y) { return x - y < 0.01f && y - x < 0.01f; };
    return near(a.r, b.r) && near(a.g, b.g) && near(a.b, b.b) && near(a.a, b.a);
}
bool sameColor(PrColor16 a, PrColor b) {
    return sameColor(prColorFromColor16(a), b);
}

PrDomElement* byId(Page& page, const char* id) {
    PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(page.document), id);
    REQUIRE(element != nullptr);
    return element;
}

// Updates the page and checks that the cached state matches a full recomputation.
PrCssPresentationStats update(Page& page) {
    prHtmlPresentationUpdate(page.presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(verified(page.presentation));
    return prCssPresentationStats(page.presentation);
}

// Returns the resolved value of `property` of the element with `id`, or of its pseudo-element that `pseudoElement`
// names, as a script reads it.
std::string resolved(Page& page, const char* id, PrCssProperty property, const char* pseudoElement = "") {
    PrCssDeclarationName name = prCssDeclarationNameFromProperty(property);
    PrCssStyleProperties* computed = prCssElementComputedStyle(byId(page, id), pseudoElement);
    PrRawString read = prCssStyleDeclarationGetPropertyValue(prCssStylePropertiesAsCssStyleDeclaration(computed), &name);
    std::string value(prRawStringView(&read).data, prRawStringView(&read).size);
    prRawStringReset(&read);
    return value;
}

// Moves the presentation's time to `time`, updates and paints it, and checks the result against one from scratch.
// Returns what painting recorded again.
PrLayoutTreeStats frame(Page& page, double time) {
    prCssPresentationSetTime(page.presentation, time);
    update(page);
    PrDrawList* list = prDrawListCreate();
    prLayoutTreePaint(prCssPresentationTree(page.presentation), list);
    prDrawListRelease(list);
    CHECK(verified(page.presentation));
    return prLayoutTreeStats(prCssPresentationTree(page.presentation));
}

// A host whose clock moves only when the test steps it.
struct SteppedClock {
    double now = 0;
};

double steppedNow(void* PR_NULLABLE context) {
    return static_cast<SteppedClock*>(context)->now;
}

// Runs the tasks that are due, including the rendering updates among them.
void runDue(PrHtmlPage* page) {
    PrDomAgent* agent = prHtmlPageAgent(page);
    while (prDomAgentNextTaskDelay(agent) == 0 && prDomAgentRunTask(agent)) {
    }
}

// What a page with a long CSS animation observes as a host steps its clock until the page is idle: each animation
// event, with its elapsed time and the timeline's time at which it fires, the timeline's time that a timer reads, and
// the frames that ran.
struct Observed {
    std::vector<std::string> events;
    double timerSaw = -1;
    uint32_t frames = 0;
    PrHtmlDocumentTimeline* PR_NULLABLE timeline = nullptr;
};

Observed observeAnimation(PrHtmlAnimationFrames frames) {
    Page fonts("");
    struct Host {
        SteppedClock clock;
        Observed seen;
    } stepped;
    SteppedClock& clock = stepped.clock;
    Observed& seen = stepped.seen;
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &stepped;
    host.now = [](void* PR_NULLABLE c) { return static_cast<Host*>(c)->clock.now; };
    host.rendered = [](void* PR_NULLABLE c, PrHtmlPage*) { ++static_cast<Host*>(c)->seen.frames; };
    host.font = fonts.defaultFont;
    host.shaper = fonts.shaper;
    host.animationFrames = frames;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(page, "<style>@keyframes grow { to { width: 300px } }"
        "#a { width: 100px; animation: grow 10s linear 500ms 2 }</style><div id=a></div>", "about:blank", nullptr) ==
        PrStatus_Ok);
    PrHtmlWindow* window = prHtmlPageWindow(page);
    PrDomDocument* document = prHtmlWindowDocument(window);
    PrDomElement* a = prDomNodeGetElementById(prDomDocumentAsNode(document), "a");
    REQUIRE(a != nullptr);
    seen.timeline = prHtmlDocumentTimelineCreate(document, 0);
    PrDomEventListener record{
        [](void* PR_NULLABLE c, PrDomEvent* event) {
            Observed* o = static_cast<Observed*>(c);
            PrStringView type = prDomEventType(event);
            double time = -1;
            prHtmlDocumentTimelineCurrentTime(o->timeline, &time);
            char line[128];
            snprintf(line, sizeof(line), "%.*s %.3f at %.1f", int(type.size), type.data,
                prCssAnimationEventElapsedTime(prCssAnimationEventQuery(event)), time);
            o->events.push_back(line);
        },
        &seen,
        1,
        nullptr,
        nullptr,
    };
    for (const char* type : {"animationstart", "animationiteration", "animationend"})
        prDomEventTargetAddEventListener(prDomNodeAsEventTarget(prDomElementAsNode(a)), type, &record, nullptr);
    PrHtmlTimerHandler timer{
        [](void* PR_NULLABLE c) {
            Observed* o = static_cast<Observed*>(c);
            prHtmlDocumentTimelineCurrentTime(o->timeline, &o->timerSaw);
        },
        &seen,
        nullptr,
        nullptr,
    };
    prHtmlWindowSetTimeout(window, &timer, 12345, false);
    PrDomAgent* agent = prHtmlPageAgent(page);
    for (uint32_t step = 0; step < 100000 && !prDomAgentIsIdle(agent); ++step) {
        double delay = prDomAgentNextTaskDelay(agent);
        if (delay < 0)
            break;
        clock.now += delay;
        prDomAgentRunTask(agent);
    }
    prHtmlDocumentTimelineDestroy(seen.timeline);
    seen.timeline = nullptr;
    prHtmlPageDestroy(page);
    return seen;
}

// Returns the boxes under `node`, joined by `separator`: the text of each text box, and the boxes under each other box
// in brackets, joined by nothing.
std::string boxesUnder(PrLayoutTree* tree, PrLayoutNode node, const char* separator) {
    std::string out;
    for (PrLayoutNode c = prLayoutNodeFirstChild(tree, node); c != kPrLayoutNodeNone; c = prLayoutNodeNextSibling(tree, c)) {
        if (c != prLayoutNodeFirstChild(tree, node))
            out += separator;
        if (prLayoutNodeKind(tree, c) == PrLayoutNodeKind_Text) {
            PrStringView text = prLayoutNodeText(tree, c);
            out.append(text.data, text.size);
        } else {
            out += '[' + boxesUnder(tree, c, "") + ']';
        }
    }
    return out;
}

// Returns the boxes under the box of the element with `id`, joined by spaces (boxesUnder).
std::string childBoxes(Page& page, const char* id) {
    return boxesUnder(prCssPresentationTree(page.presentation), page.node(id), " ");
}

// Attaches an open shadow root to the element with `id`, holding `markup`.
PrDomShadowRoot* attachShadow(Page& page, const char* id, const char* markup) {
    PrDomShadowRootInit init = prDomShadowRootInitDefault(PrDomShadowRootMode_Open);
    PrDomShadowRoot* shadow = nullptr;
    REQUIRE(prDomElementAttachShadow(byId(page, id), &init, &shadow) == PrStatus_Ok);
    prHtmlSetInnerHtml(prDomShadowRootAsNode(shadow), markup);
    return shadow;
}

// Returns the element of the shadow tree that `selector` finds.
PrDomElement* inShadow(PrDomShadowRoot* shadow, const char* selector) {
    PrDomElement* element = nullptr;
    REQUIRE(prCssNodeQuerySelector(prDomShadowRootAsNode(shadow), selector, &element) == PrStatus_Ok);
    REQUIRE(element != nullptr);
    return element;
}

// Returns the resolved value of `property` of `element`, as a script reads it.
std::string resolvedOf(PrDomElement* element, PrCssProperty property) {
    PrCssDeclarationName name = prCssDeclarationNameFromProperty(property);
    PrRawString read = prCssStyleDeclarationGetPropertyValue(
        prCssStylePropertiesAsCssStyleDeclaration(prCssElementComputedStyle(element, "")), &name);
    std::string value(prRawStringView(&read).data, prRawStringView(&read).size);
    prRawStringReset(&read);
    return value;
}

// Returns the used appearance of the element with `id`, or of its ::before.
CssAppearance usedAppearance(Page& page, const char* id, bool before = false) {
    const CssPresentation* presentation = page.presentation;
    const PrDomNode* node = byId(page, id);
    if (before) {
        uint32_t at = cssPresentationNodeEntry(presentation, node);
        REQUIRE(at != kHashMissing);
        node = presentation->entries.data[at].pseudos[kBeforePseudo];
        REQUIRE(node != nullptr);
    }
    uint32_t at = cssPresentationNodeEntry(presentation, node);
    REQUIRE(at != kHashMissing);
    return presentation->entries.data[at].style->appearance;
}

// Returns the commands that `html` paints at 100ms and at 1100ms, as text. The page runs in a browsing context on a
// clock that the test steps, and is painted through a view, as a host shows it.
std::pair<std::string, std::string> paintedAtTwoTimes(const char* html) {
    Page fonts("");
    SteppedClock clock;
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &clock;
    host.now = steppedNow;
    host.font = fonts.defaultFont;
    host.shaper = fonts.shaper;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(page, html, "about:blank", nullptr) == PrStatus_Ok);
    PrDomAgent* agent = prHtmlPageAgent(page);
    // Returns the commands painted once the clock reaches `time`, as text.
    auto paintedAt = [&](double time) {
        for (uint32_t step = 0; step < 10000; ++step) {
            double delay = prDomAgentNextTaskDelay(agent);
            if (delay < 0 || clock.now + delay > time)
                break;
            clock.now += delay;
            prDomAgentRunTask(agent);
        }
        clock.now = time;
        runDue(page);
        PrHtmlViewDesc desc{.page = page};
        PrHtmlView* view = prHtmlViewCreate(&desc);
        prHtmlViewSetViewport(view, PrVec2{800, 600});
        prHtmlPresentationUpdate(prHtmlPagePresentation(page), prLayoutVecFromPx(PrVec2{800, 600}));
        PrDrawList* list = prDrawListCreate();
        prHtmlViewPaint(view, list);
        prHtmlViewDestroy(view);
        PrRawString dump = prDrawListDump(list);
        std::string text(prRawStringView(&dump).data, prRawStringView(&dump).size);
        prRawStringReset(&dump);
        prDrawListRelease(list);
        return text;
    };
    std::string start = paintedAt(100);
    std::string later = paintedAt(1100);
    prHtmlPageDestroy(page);
    return {start, later};
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a stretch size reaches layout, and fills the containing block less the margins") {
    Page page("<style>body { margin: 0 } #a { width: stretch; margin-left: 30px; height: 10px }</style><div id=a></div>");
    CHECK(page.box("a").width.bits == prLayoutLengthStretch().bits);
    CHECK(page.bounds("a").left == 30.0f);
    CHECK(page.bounds("a").right == 800.0f);
}

TEST_CASE("the user agent sheet styles HTML's elements") {
    Page page("<body id=b><h1 id=h>Title</h1><p id=p>Text</p><span id=s>x</span></body>");
    CHECK(page.box("b").display == PrLayoutDisplay_Block);
    CHECK(page.box("s").display == PrLayoutDisplay_Inline);
    CHECK(px(page.box("b").margin[PrRectSide_Left]) == 8.0f);
    CHECK(page.fontStyle("h").size == 32.0f);
    CHECK(page.fontStyle("h").weight == 700.0f);
    // 0.67em of 32px is 21.44px, which a length holds as 1372/64.
    CHECK(px(page.box("h").margin[PrRectSide_Top]) == 21.4375f);
    CHECK(px(page.box("p").margin[PrRectSide_Top]) == 16.0f);
    // The body's margin, with the heading's margin collapsing through it, in layout's units: 21.44px truncates to
    // 1372/64.
    CHECK(page.bounds("h").left == 8.0f);
    CHECK(page.bounds("h").top == 21.4375f);
}

TEST_CASE("author rules apply in order of specificity, importance and appearance") {
    Page page("<style>#a { width: 10px } .c { width: 20px } div { width: 30px }"
        ".i { width: 40px !important } #i { width: 50px }div.late { height: 1px } div.late { height: 2px }</style>"
        "<div id=a class=c></div><div id=i class=i></div><div id=late class=late></div>"
        "<div id=inline style='width: 60px' class=c></div>");
    CHECK(px(page.box("a").width) == 10.0f);
    CHECK(px(page.box("i").width) == 40.0f);
    CHECK(px(page.box("late").height) == 2.0f);
    CHECK(px(page.box("inline").width) == 60.0f);
}

TEST_CASE("a pseudo-element's rules style it, under its originating element's features, and it inherits from it") {
    Page page("<style>#a { color: rgb(255, 0, 0); width: 10px; list-style-type: square }"
        "#a::before { content: 'x'; width: 20px }div.c::after { content: counter(n) ' ' }"
        "section > div::marker { color: rgb(0, 0, 255) }#a:before { font-size: 30px }"
        "#a::before::marker { content: 'm' }#a::before:hover { width: 40px }</style>"
        "<section><div id=a class=c></div></section><div id=b></div>");
    CHECK(resolved(page, "a", PrCssProperty_Width) == "10px");
    CHECK(resolved(page, "a", PrCssProperty_Content) == "normal");
    CHECK(resolved(page, "a", PrCssProperty_Width, "::before") == "20px");
    CHECK(resolved(page, "a", PrCssProperty_Content, "::before") == "\"x\"");
    CHECK(resolved(page, "a", PrCssProperty_FontSize, ":before") == "30px");
    CHECK(resolved(page, "a", PrCssProperty_Color, "::before") == "rgb(255, 0, 0)");
    CHECK(resolved(page, "a", PrCssProperty_ListStyleType, "::before") == "square");
    CHECK(resolved(page, "a", PrCssProperty_Content, "::after") == "counter(n) \" \"");
    CHECK(resolved(page, "a", PrCssProperty_Color, "::marker") == "rgb(0, 0, 255)");
    CHECK(resolved(page, "b", PrCssProperty_Color, "::marker") == "rgb(0, 0, 0)");
    // On ::before and ::after, content: normal computes to none. On ::marker it stays normal.
    CHECK(resolved(page, "b", PrCssProperty_Content, "::before") == "none");
    CHECK(resolved(page, "b", PrCssProperty_Content, "::marker") == "normal");
    // A marker of a ::before inherits from the ::before.
    CHECK(resolved(page, "a", PrCssProperty_FontSize, "::before::marker") == "30px");
    CHECK(resolved(page, "a", PrCssProperty_Content, "::before::marker") == "\"m\"");
    // An argument without a colon names the element, and so does a pseudo-element that the cascade does not style.
    CHECK(resolved(page, "a", PrCssProperty_Width, "before") == "10px");
    CHECK(resolved(page, "a", PrCssProperty_Width, "::selection") == "10px");
    // An argument that is not a pseudo-element gives an empty style.
    CHECK(resolved(page, "a", PrCssProperty_Width, ":hover").empty());
    CHECK(resolved(page, "a", PrCssProperty_Width, "::nonsense").empty());
    CHECK(resolved(page, "a", PrCssProperty_Width, "::part(x)").empty());
}

TEST_CASE("a computed style gives the element's custom properties, read one at a time and listed after the longhands") {
    Page page("<style>#a { --fill: rgba(0,0,0,0.48); --blur: 20px; --ref: var(--blur) 1px }#b { --own: x }</style>"
        "<div id=a><div id=b></div></div>");
    auto custom = [&](const char* id, const char* text) {
        PrCssStyleDeclaration* computed =
            prCssStylePropertiesAsCssStyleDeclaration(prCssElementComputedStyle(byId(page, id), ""));
        PrCssDeclarationName name{};
        REQUIRE(prCssStyleDeclarationNameFromText(computed, text, &name));
        PrRawString read = prCssStyleDeclarationGetPropertyValue(computed, &name);
        std::string value(prRawStringView(&read).data, prRawStringView(&read).size);
        prRawStringReset(&read);
        prCssDeclarationNameReset(&name);
        return value;
    };
    CHECK(custom("a", "--fill") == "rgba(0,0,0,0.48)");
    CHECK(custom("a", "--ref") == "20px 1px");
    // Custom properties inherit.
    CHECK(custom("b", "--blur") == "20px");
    CHECK(custom("b", "--own") == "x");
    CHECK(custom("a", "--own").empty());
    PrRawString text = prCssStyleDeclarationCssText(
        prCssStylePropertiesAsCssStyleDeclaration(prCssElementComputedStyle(byId(page, "b"), "")));
    std::string listed(prRawStringView(&text).data, prRawStringView(&text).size);
    prRawStringReset(&text);
    CHECK(listed.find("--own: x;") != std::string::npos);
    CHECK(listed.find("--blur: 20px;") > listed.find("width:"));
}

TEST_CASE("::first-line and ::first-letter take their rules, and the first letter inherits from the first line") {
    Page page("<style>#a { color: rgb(255, 0, 0); font-size: 10px; width: 10px }"
        "#a::first-line { color: rgb(0, 0, 255); font-size: 20px; direction: rtl; writing-mode: vertical-rl }"
        "#a::first-letter { font-weight: 700 }</style><p id=a>text</p><p id=b>text</p>");
    CHECK(resolved(page, "a", PrCssProperty_Color, "::first-line") == "rgb(0, 0, 255)");
    CHECK(resolved(page, "a", PrCssProperty_FontSize, "::first-line") == "20px");
    // A property that does not inherit takes its initial value.
    CHECK(resolved(page, "a", PrCssProperty_Width, "::first-line") == "auto");
    // writing-mode and direction never apply to ::first-line, which takes them from its element.
    CHECK(resolved(page, "a", PrCssProperty_Direction, "::first-line") == "ltr");
    CHECK(resolved(page, "a", PrCssProperty_WritingMode, "::first-line") == "horizontal-tb");
    CHECK(resolved(page, "a", PrCssProperty_Color, "::first-letter") == "rgb(0, 0, 255)");
    CHECK(resolved(page, "a", PrCssProperty_FontSize, "::first-letter") == "20px");
    CHECK(resolved(page, "a", PrCssProperty_FontWeight, "::first-letter") == "700");
    CHECK(resolved(page, "b", PrCssProperty_FontWeight, "::first-letter") == "400");
}

TEST_CASE("::first-line and ::first-letter ignore the properties that do not apply to them") {
    Page page("<style>#a::first-line, #a::first-letter { margin-top: 5px; position: absolute; order: 2; opacity: 0.5 }"
        "</style><p id=a>text</p>");
    CHECK(resolved(page, "a", PrCssProperty_Opacity, "::first-line") == "0.5");
    CHECK(resolved(page, "a", PrCssProperty_MarginTop, "::first-line") == "0px");
    CHECK(resolved(page, "a", PrCssProperty_Position, "::first-line") == "static");
    CHECK(resolved(page, "a", PrCssProperty_MarginTop, "::first-letter") == "5px");
    CHECK(resolved(page, "a", PrCssProperty_Position, "::first-letter") == "static");
    CHECK(resolved(page, "a", PrCssProperty_Order, "::first-letter") == "0");
}

TEST_CASE("::before and ::after boxes take their content, first and last among the element's children") {
    Page page("<!doctype html><style>.g::before { content: 'B' attr(id) } .g::after { content: \"A\" / 'alt' }"
        ".n::before { content: none } .d::after { display: none }"
        "img::before, br::before { content: 'x' } .contents { display: contents }</style>"
        "<p id=p class=g>text</p><p id=n class='g n d'>t</p><img class=g id=i>"
        "<div id=outer><span class='g contents' id=c>in</span></div><div id=plain>y</div>");
    CHECK(childBoxes(page, "p") == "[Bp] text [A]");
    CHECK(childBoxes(page, "n") == "t");
    CHECK(childBoxes(page, "plain") == "y");
    // The boxes of the pseudo-elements of an element of display contents are its parent's children.
    CHECK(childBoxes(page, "outer") == "[Bc] in [A]");
    // A point on a pseudo-element's box hits its originating element.
    PrRect p = page.bounds("p");
    PrDomNode* hit = prCssPresentationHitTest(page.presentation, prLayoutVecFromPx(PrVec2{p.left + 1, p.top + 1}));
    CHECK(hit == prDomElementAsNode(byId(page, "p")));
    // A change of class, of display or of an attribute that content reads changes the boxes.
    prDomElementSetAttribute(byId(page, "plain"), "class", "g");
    update(page);
    CHECK(childBoxes(page, "plain") == "[Bplain] y [A]");
    prDomElementSetAttribute(byId(page, "plain"), "id", "q");
    update(page);
    CHECK(childBoxes(page, "q") == "[Bq] y [A]");
    prDomElementSetAttribute(byId(page, "q"), "class", "g d");
    update(page);
    CHECK(childBoxes(page, "q") == "[Bq] y");
    prDomNodeRemove(prDomElementAsNode(byId(page, "q")));
    update(page);
}

TEST_CASE("a list item's inside marker is the first of its boxes, and shows its list-item counter") {
    Page page("<!doctype html><style>.in { list-style-position: inside } .gone { display: none }</style>"
        "<ol class=in><li id=a>x</li><li id=b value=5>y<p id=p>z</p></li><li id=hidden class=gone></li>"
        "<li id=c>z</li></ol><ol class=in reversed><li id=r1>a</li><li id=r2>b</li><li id=r3>c</li></ol>"
        "<ul><li id=outside>t</li><li id=disc class=in>u</li></ul><ol class=in type=i start=3><li id=roman>v</li></ol>"
        "<ol class=in><li>x</li><li>y</li><ol><li>n</li></ol><li id=afterNested>z</li></ol>");
    CHECK(childBoxes(page, "a") == "[1. ] x");
    CHECK(childBoxes(page, "b") == "[5. ] y [z]");
    // An element with no box increments no counter.
    CHECK(childBoxes(page, "c") == "[6. ] z");
    CHECK(childBoxes(page, "r1") == "[3. ] a");
    CHECK(childBoxes(page, "r3") == "[1. ] c");
    CHECK(childBoxes(page, "disc") == "[\xE2\x80\xA2 ] u");
    CHECK(childBoxes(page, "roman") == "[iii. ] v");
    // A list-item counter that a list resets is the list's alone, so the items after a list nested directly in a list
    // continue the outer list.
    CHECK(childBoxes(page, "afterNested") == "[3. ] z");
    // An outside marker's box is the first of the item's boxes too, and layout places it outside the item's content.
    CHECK(childBoxes(page, "outside") == "[\xE2\x80\xA2 ] t");
    CHECK(resolved(page, "a", PrCssProperty_Content, "::marker") == "normal");
    // A point on an outside marker, left of its item's content, hits the item.
    PrRect item = page.bounds("outside");
    PrDomNode* hit = prCssPresentationHitTest(page.presentation, prLayoutVecFromPx(PrVec2{item.left - 4, item.top + 4}));
    CHECK(hit == prDomElementAsNode(byId(page, "outside")));
    // A marker that moves inside its item takes its place on the item's line, and one that moves out leaves it.
    prDomElementSetAttribute(byId(page, "outside"), "class", "in");
    update(page);
    CHECK(prCssPresentationHitTest(page.presentation, prLayoutVecFromPx(PrVec2{item.left - 4, item.top + 4})) !=
        prDomElementAsNode(byId(page, "outside")));
    prDomElementSetAttribute(byId(page, "disc"), "class", "");
    update(page);
    PrRect disc = page.bounds("disc");
    CHECK(prCssPresentationHitTest(page.presentation, prLayoutVecFromPx(PrVec2{disc.left - 4, disc.top + 4})) ==
        prDomElementAsNode(byId(page, "disc")));
}

TEST_CASE("a ::before or ::after that is a list item increments list-item, and has a marker of its own") {
    Page page("<!doctype html><style>li { list-style-position: inside }.b::before, .a::after { display: list-item; content: 'B' }"
        ".b::before::marker { content: counter(list-item) ':' }</style>"
        "<ol><li id=one class=b>x</li><li id=two class=a>y</li><li id=three>z</li></ol>");
    // The first item is 1, its ::before 2, the second item 3 and its ::after 4.
    CHECK(childBoxes(page, "one") == "[1. ] [[2:]B] x");
    CHECK(childBoxes(page, "two") == "[3. ] y [[4. ]B]");
    CHECK(childBoxes(page, "three") == "[5. ] z");
    // A ::before that stops being a list item loses its marker, and stops counting.
    prDomElementSetAttribute(byId(page, "one"), "class", "");
    update(page);
    CHECK(childBoxes(page, "two") == "[2. ] y [[3. ]B]");
    prDomElementSetAttribute(byId(page, "three"), "class", "b");
    update(page);
    CHECK(childBoxes(page, "three") == "[4. ] [[5:]B] z");
}

TEST_CASE("the counter pass walks only what follows the last pass when the changes all follow it") {
    Page page("<!doctype html><style>li { list-style-position: inside } ol ol { counter-reset: none }</style>"
        "<body><ol id=list><li>a</li><li>b<ol id=inner><li id=deep>c</li></ol></li></ol></body>");
    CHECK(childBoxes(page, "deep") == "[3. ] c");
    auto append = [&](const char* parent, const char* id) {
        PrDomElement* item = nullptr;
        REQUIRE(prDomElementCreate(page.document, "li", nullptr, &item) == PrStatus_Ok);
        prDomElementSetAttribute(item, "id", id);
        prDomNodeAppendChild(prDomElementAsNode(byId(page, parent)), prDomElementAsNode(item));
    };
    // Items appended to the deepest list, and then to the outer one, continue the numbering, and the pass walks the new
    // items and the ends of what holds them.
    append("inner", "d");
    PrCssPresentationStats stats = update(page);
    CHECK(childBoxes(page, "d") == "[4. ]");
    CHECK(stats.counterSteps < 10);
    append("list", "e");
    append("list", "f");
    stats = update(page);
    CHECK(childBoxes(page, "e") == "[5. ]");
    CHECK(childBoxes(page, "f") == "[6. ]");
    CHECK(stats.counterSteps < 12);
    // An item inserted before the others numbers them all again.
    PrDomElement* first = nullptr;
    REQUIRE(prDomElementCreate(page.document, "li", nullptr, &first) == PrStatus_Ok);
    PrDomNode* list = prDomElementAsNode(byId(page, "list"));
    REQUIRE(prDomNodeInsertBefore(list, prDomElementAsNode(first), prDomNodeFirstChild(list)) == PrStatus_Ok);
    stats = update(page);
    CHECK(childBoxes(page, "f") == "[7. ]");
    CHECK(stats.counterSteps > 20);
    // A reversed list counts every item, so an item appended to it numbers the others again.
    prDomElementSetAttribute(byId(page, "list"), "reversed", "");
    update(page);
    CHECK(childBoxes(page, "f") == "[1. ]");
    append("list", "g");
    update(page);
    CHECK(childBoxes(page, "f") == "[2. ]");
    CHECK(childBoxes(page, "g") == "[1. ]");
}

TEST_CASE("open-quote and close-quote insert the marks of the quote depth, which steps in tree order") {
    Page page("<!doctype html><p id=p><q id=a>a <q id=b>b <q id=c>c</q></q></q> <q id=d>d</q></p>"
        "<p style='quotes: \"<\" \">\" \"(\" \")\"'><q id=e>e <q id=f>f <q id=g>g</q></q></q></p>"
        "<p style='quotes: none'><q id=h>h</q></p>");
    CHECK(childBoxes(page, "a") == "[“] a  [[‘]b [[‘]c[’]][’]] [”]");
    CHECK(childBoxes(page, "d") == "[“] d [”]");
    // Past the last pair, the last pair repeats.
    CHECK(childBoxes(page, "g") == "[(] g [)]");
    CHECK(childBoxes(page, "e") == "[<] e  [[(]f [[(]g[)]][)]] [>]");
    // quotes: none inserts no marks, and still steps the depth.
    CHECK(childBoxes(page, "h") == "[] h []");
    CHECK(verified(page.presentation));
}

TEST_CASE("a closing quote at depth 0 inserts nothing, and the no- quotes step the depth without marks") {
    Page page("<!doctype html><style>.c::after { content: close-quote 'x' } .n::before { content: no-open-quote }"
        ".m::after { content: no-close-quote } span::before { content: open-quote }</style>"
        "<div id=a class=c>a</div><div id=b class=n></div><span id=c>c</span><div id=d class=m></div><span id=e>e</span>");
    CHECK(childBoxes(page, "a") == "a [x]");
    // After no-open-quote, the open quote is at depth 1, and after no-close-quote at depth 1 again.
    CHECK(childBoxes(page, "c") == "[‘] c");
    CHECK(childBoxes(page, "e") == "[‘] e");
    CHECK(verified(page.presentation));
}

TEST_CASE("quotes: auto takes the marks of the language of the q's parent, which a change of language reaches") {
    Page page("<!doctype html><html lang=en><p id=p><q id=a>a <q id=b lang=ja>b <q id=c lang=fr>c</q></q></q></p>"
        "<p lang=fr-XX><q id=d>d</q></p><p lang=zz><q id=e>e</q></p></html>");
    // b is in Japanese inside English text, so it takes English marks, and c takes Japanese ones.
    CHECK(childBoxes(page, "b") == "[‘] b  [[『]c[』]] [’]");
    // A region that the registry does not list leaves the language, and an unknown language takes CLDR's root marks.
    CHECK(childBoxes(page, "d") == "[«] d [»]");
    CHECK(childBoxes(page, "e") == "[“] e [”]");
    prDomElementSetAttribute(prDomNodeGetElementById(prDomDocumentAsNode(page.document), "p"), "lang", "de");
    prHtmlPresentationUpdate(page.presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(childBoxes(page, "a") == "[„] a  [[‚]b [[『]c[』]][‘]] [“]");
    CHECK(verified(page.presentation));
}

TEST_CASE("quotes: match-parent takes its parent's quotes, or the marks of its parent's language for auto") {
    Page page("<!doctype html><html lang=en><div lang=fr style='quotes: match-parent'><p lang=de><q id=a>a</q></p></div>"
        "<div style='quotes: \"[\" \"]\"'><q id=b style='quotes: match-parent'>b</q></div></html>");
    // The div declares match-parent, so it and what inherits from it quote in its parent's language.
    CHECK(childBoxes(page, "a") == "[“] a [”]");
    CHECK(childBoxes(page, "b") == "[[] b []]");
    CHECK(verified(page.presentation));
}

TEST_CASE("an open quote added before others, or a change of quotes, gives the later quotes their new marks") {
    Page page("<!doctype html><style>.o::before { content: open-quote }</style>"
        "<p id=p><span id=s></span><q id=a>a</q></p>");
    auto update = [&]() { prHtmlPresentationUpdate(page.presentation, prLayoutVecFromPx(PrVec2{800, 600})); };
    auto element = [&](const char* id) { return prDomNodeGetElementById(prDomDocumentAsNode(page.document), id); };
    CHECK(childBoxes(page, "a") == "[“] a [”]");
    // An open quote that nothing closes leaves the depth at 1 for the q after it.
    prDomElementSetAttribute(element("s"), "class", "o");
    update();
    CHECK(childBoxes(page, "a") == "[‘] a [’]");
    CHECK(verified(page.presentation));
    prDomElementSetAttribute(element("p"), "style", "quotes: '<' '>' '(' ')'");
    update();
    CHECK(childBoxes(page, "a") == "[(] a [)]");
    CHECK(verified(page.presentation));
    prDomElementRemoveAttribute(element("s"), "class");
    update();
    CHECK(childBoxes(page, "a") == "[<] a [>]");
    CHECK(verified(page.presentation));
}

TEST_CASE("a gradient marker image is a replaced box of 0.4em square that paints the gradient, followed by a space") {
    Page page("<!doctype html><ul style='font-size: 20px; list-style: inside linear-gradient(red, blue)'>"
        "<li id=a>a</li></ul><ul style='list-style: outside radial-gradient(red, blue)'><li id=b>b</li></ul>");
    CHECK(childBoxes(page, "a") == "[[] ] a");
    CHECK(childBoxes(page, "b") == "[[] ] b");
    PrLayoutTree* tree = prCssPresentationTree(page.presentation);
    PrLayoutNode image = prLayoutNodeFirstChild(tree, prLayoutNodeFirstChild(tree, page.node("a")));
    REQUIRE(prLayoutNodeKind(tree, image) == PrLayoutNodeKind_Replaced);
    PrLayoutNaturalSize natural = prLayoutNodeNaturalSize(tree, image);
    CHECK(prLayoutUnitToPx(natural.width) == doctest::Approx(8.0f).epsilon(0.01));
    CHECK(prLayoutUnitToPx(natural.height) == doctest::Approx(8.0f).epsilon(0.01));
    PrLayoutStyle record = prLayoutStyleRecord(tree, prLayoutNodeStyle(tree, image));
    PrLayoutDecorationStyle decoration = prLayoutDecorationStyleRecord(tree, record.groups[PrLayoutGroup_Decoration]);
    REQUIRE(prLayoutBackgroundLayerCount(tree, decoration.backgroundLayers) == 1);
    CHECK(prLayoutBackgroundLayer(tree, decoration.backgroundLayers, 0).kind == PrLayoutImageKind_Gradient);
}

TEST_CASE("a filter() marker image and a filter() in a content list are replaced boxes that paint the filtered image") {
    Page page("<!doctype html><ul style='list-style: inside filter(linear-gradient(red, blue), invert(1))'>"
        "<li id=a>a</li></ul><p id=b>b</p><style>#b::before { content: 'x' filter(linear-gradient(red, blue), sepia(1)) }</style>");
    CHECK(childBoxes(page, "a") == "[[] ] a");
    PrLayoutTree* tree = prCssPresentationTree(page.presentation);
    auto layerOf = [&](PrLayoutNode image) {
        PrLayoutStyle record = prLayoutStyleRecord(tree, prLayoutNodeStyle(tree, image));
        PrLayoutDecorationStyle decoration =
            prLayoutDecorationStyleRecord(tree, record.groups[PrLayoutGroup_Decoration]);
        REQUIRE(prLayoutBackgroundLayerCount(tree, decoration.backgroundLayers) == 1);
        return prLayoutBackgroundLayer(tree, decoration.backgroundLayers, 0);
    };
    PrLayoutNode marker = prLayoutNodeFirstChild(tree, prLayoutNodeFirstChild(tree, page.node("a")));
    REQUIRE(prLayoutNodeKind(tree, marker) == PrLayoutNodeKind_Replaced);
    PrLayoutBackgroundLayer a = layerOf(marker);
    CHECK(a.kind == PrLayoutImageKind_Gradient);
    REQUIRE(a.filterCount == 1);
    CHECK(a.filters[0].kind == PrLayoutFilterKind_Invert);
    PrLayoutNode before = prLayoutNodeFirstChild(tree, page.node("b"));
    PrLayoutNode image = prLayoutNodeNextSibling(tree, prLayoutNodeFirstChild(tree, before));
    REQUIRE(prLayoutNodeKind(tree, image) == PrLayoutNodeKind_Replaced);
    CHECK(layerOf(image).filters[0].kind == PrLayoutFilterKind_Sepia);
    CHECK(verified(page.presentation));
}

TEST_CASE("a document's @counter-style rules define counter styles, resolved by extends, in place of predefined ones") {
    Page page("<!doctype html><style>li { list-style-position: inside }"
        "@counter-style stars { system: cyclic; symbols: '*' '+'; suffix: ' ' }"
        "@counter-style corners { system: fixed; symbols: \u25f0 \u25f3; suffix: ' ' }"
        "@counter-style paren { system: extends decimal; prefix: '('; suffix: ') '; pad: 3 '0' }"
        "@counter-style lower-roman { system: cyclic; symbols: X; suffix: '' }"
        "@counter-style decimal { system: cyclic; symbols: D }@counter-style few { system: alphabetic; symbols: a }"
        "@counter-style loop-a { system: extends loop-b } @counter-style loop-b { system: extends loop-a }"
        "@counter-style small { system: fixed 1; symbols: one two; fallback: stars }"
        "@counter-style neg { system: extends decimal; negative: '(' ')'; suffix: '' }</style>"
        "<ol style='list-style-type: stars'><li id=s1>a</li><li id=s2>b</li><li id=s3>c</li></ol>"
        "<ol style='list-style-type: corners'><li>a</li><li id=c2>b</li></ol>"
        "<ol style='list-style-type: paren'><li id=p>a</li></ol>"
        "<ol style='list-style-type: LOWER-ROMAN'><li id=r>a</li></ol>"
        "<ol style='list-style-type: decimal'><li id=d>a</li></ol><ol style='list-style-type: few'><li id=f>a</li></ol>"
        "<ol style='list-style-type: loop-a'><li id=l>a</li></ol>"
        "<ol style='list-style-type: small'><li>a</li><li>b</li><li id=fb>c</li></ol>"
        "<ol style='list-style-type: neg' start=-2><li id=n>a</li></ol><div id=late></div>");
    CHECK(childBoxes(page, "s1") == "[* ] a");
    CHECK(childBoxes(page, "s3") == "[* ] c");
    // A symbol may be an ident of any code point above ASCII.
    CHECK(childBoxes(page, "c2") == "[\u25f3 ] b");
    CHECK(childBoxes(page, "p") == "[(001) ] a");
    // A rule takes the place of a predefined style, whose name matches ASCII case-insensitively.
    CHECK(childBoxes(page, "r") == "[X] a");
    // No rule may define decimal, a rule with too few symbols for its system defines no style, and a cycle of extends
    // extends decimal.
    CHECK(childBoxes(page, "d") == "[1. ] a");
    CHECK(childBoxes(page, "f") == "[1. ] a");
    CHECK(childBoxes(page, "l") == "[1. ] a");
    // A value outside a fixed style's symbols uses its fallback, a document's style, inside the list's own suffix.
    CHECK(childBoxes(page, "fb") == "[*. ] c");
    CHECK(childBoxes(page, "n") == "[(2)] a");
    // A rule added later numbers the markers again.
    PrDomElement* style = nullptr;
    REQUIRE(prDomElementCreate(page.document, "style", nullptr, &style) == PrStatus_Ok);
    prDomNodeSetTextContent(prDomElementAsNode(style), PR_SV("@counter-style stars { system: cyclic; symbols: '#' }"));
    prDomNodeAppendChild(prDomElementAsNode(byId(page, "late")), prDomElementAsNode(style));
    update(page);
    CHECK(childBoxes(page, "s1") == "[#. ] a");
    CHECK(childBoxes(page, "fb") == "[#. ] c");
}

TEST_CASE("a @counter-style descriptor's calculation resolves its lengths against the initial font") {
    Page page("<!doctype html><style>li { list-style-position: inside }"
        "@counter-style a { system: extends upper-roman; pad: calc(3 + sign(100em - 1px)) '*' }"
        "@counter-style b { system: fixed calc(1 + sign(100em - 1px)); symbols: g h }"
        "@counter-style c { system: additive; additive-symbols: calc(2 + sign(100em - 1px)) c, 2 b, 1 a }"
        "</style><ol style='list-style-type: a'><li id=a></li></ol>"
        "<ol style='list-style-type: b'><li id=b1></li><li id=b2></li></ol>"
        "<ol style='list-style-type: c' start=3><li id=c></li></ol>");
    CHECK(childBoxes(page, "a") == "[***I. ]");
    CHECK(childBoxes(page, "b1") == "[1. ]");
    CHECK(childBoxes(page, "b2") == "[g. ]");
    CHECK(childBoxes(page, "c") == "[c. ]");
}

TEST_CASE("a reversed list starts from the items whose increments reach its counter, outside style containment") {
    // The items in a subtree with style containment increment a counter of their own
    // (css-contain-2/#containment-style).
    Page page("<!doctype html><style>li { list-style-position: inside }</style>"
        "<ol reversed><li id=a>A</li><li>B</li><div style='contain: style'><li id=x>X</li><li id=y>Y</li></div>"
        "<li id=c>C</li><li id=d>D</li></ol>");
    CHECK(childBoxes(page, "a") == "[4. ] A");
    CHECK(childBoxes(page, "x") == "[1. ] X");
    CHECK(childBoxes(page, "y") == "[2. ] Y");
    CHECK(childBoxes(page, "c") == "[2. ] C");
    CHECK(childBoxes(page, "d") == "[1. ] D");
}

TEST_CASE("the counter pass numbers the items again as items come and go and as their counters change") {
    Page page("<!doctype html><style>li { list-style-position: inside }</style>"
        "<ol id=list><li id=a>a</li><li id=b>b</li></ol>");
    CHECK(childBoxes(page, "b") == "[2. ] b");
    PrDomElement* item = nullptr;
    REQUIRE(prDomElementCreate(page.document, "li", nullptr, &item) == PrStatus_Ok);
    prDomElementSetAttribute(item, "id", "first");
    PrDomNode* list = prDomElementAsNode(byId(page, "list"));
    REQUIRE(prDomNodeInsertBefore(list, prDomElementAsNode(item), prDomElementAsNode(byId(page, "a"))) == PrStatus_Ok);
    update(page);
    CHECK(childBoxes(page, "first") == "[1. ]");
    CHECK(childBoxes(page, "b") == "[3. ] b");
    prDomElementSetAttribute(byId(page, "a"), "value", "10");
    update(page);
    CHECK(childBoxes(page, "b") == "[11. ] b");
    prDomElementSetAttribute(byId(page, "a"), "style", "display: none");
    update(page);
    CHECK(childBoxes(page, "b") == "[2. ] b");
    prDomNodeRemove(prDomElementAsNode(byId(page, "first")));
    update(page);
    CHECK(childBoxes(page, "b") == "[1. ] b");
    prDomElementSetAttribute(byId(page, "list"), "style", "list-style-type: upper-alpha");
    update(page);
    CHECK(childBoxes(page, "b") == "[A. ] b");
}

TEST_CASE("counter() and counters() in content read the counters in the pseudo-element's scope") {
    Page page("<!doctype html><style>section { counter-reset: s } h2 { counter-increment: s }"
        "h2::before { content: counters(s, '.') ': ' counter(s, lower-roman) }"
        "p::after { content: '[' counter(none-yet) ']' }li::marker { content: 'Item ' counter(list-item) ' of ' }"
        "</style><section><h2 id=one>A</h2><div><section><h2 id=inner>B</h2><h2 id=inner2>C</h2></section></div>"
        "<h2 id=two>D</h2><section><h2 id=sibling>E</h2></section><h2 id=after>F</h2></section><p id=p>x</p>"
        "<ol><li id=item style='list-style-position: inside'>y</li></ol>");
    CHECK(childBoxes(page, "one") == "[1: i] A");
    CHECK(childBoxes(page, "inner") == "[1.1: i] B");
    CHECK(childBoxes(page, "inner2") == "[1.2: ii] C");
    CHECK(childBoxes(page, "two") == "[2: ii] D");
    // The scope of a counter includes the following siblings of the element that instantiates it.
    CHECK(childBoxes(page, "sibling") == "[2.1: i] E");
    CHECK(childBoxes(page, "after") == "[2.2: ii] F");
    // A counter that no node instantiated is instantiated where it is read.
    CHECK(childBoxes(page, "p") == "x [[0]]");
    CHECK(childBoxes(page, "item") == "[Item 1 of ] y");
}

TEST_CASE("attr() substitutes an attribute as a string, a number, a dimension or a syntax, or its fallback") {
    Page page("<!doctype html><style>#a { width: attr(data-w px); height: attr(data-missing px, 7px); content: attr(data-s) }"
        "#a::before { content: attr(data-s) ' ' attr(data-missing) }"
        "#b { width: attr(data-w type(<length>), 3px); background-image: url(attr(data-s)) }"
        "#b::after { content: attr(DATA-S raw-string) }"
        "</style><div id=a data-w=12 data-s='x\"y'></div><div id=b data-w=5em data-s=z></div>");
    CHECK(resolved(page, "a", PrCssProperty_Width) == "12px");
    CHECK(resolved(page, "a", PrCssProperty_Height) == "7px");
    CHECK(resolved(page, "a", PrCssProperty_Content) == "\"x\\\"y\"");
    CHECK(resolved(page, "a", PrCssProperty_Content, "::before") == "\"x\\\"y\" \" \" \"\"");
    CHECK(resolved(page, "b", PrCssProperty_Width) == "80px");
    CHECK(resolved(page, "b", PrCssProperty_Content, "::after") == "\"z\"");
    // A change to the attribute restyles the element.
    prDomElementSetAttribute(byId(page, "a"), "data-w", "20");
    CHECK(update(page).elementsStyled == 1);
    CHECK(resolved(page, "a", PrCssProperty_Width) == "20px");
}

TEST_CASE("a list's type, start, reversed and value attributes map to its list style and counters") {
    Page page("<!doctype html><ol id=a type=a start=10><li id=v value=' 7x' type=I></li></ol>"
        "<ol id=r reversed start=20></ol><ol id=n reversed></ol><ol id=o type=CIRCLE></ol>"
        "<ul id=u type=SQUARE><li id=nested></li><ul><li id=deep><ul id=third></ul></li></ul></ul>");
    CHECK(resolved(page, "a", PrCssProperty_ListStyleType) == "lower-alpha");
    CHECK(resolved(page, "a", PrCssProperty_CounterReset) == "list-item 9");
    CHECK(resolved(page, "v", PrCssProperty_ListStyleType) == "upper-roman");
    CHECK(resolved(page, "v", PrCssProperty_CounterSet) == "list-item 7");
    CHECK(resolved(page, "r", PrCssProperty_CounterReset) == "reversed(list-item) 21");
    CHECK(resolved(page, "n", PrCssProperty_CounterReset) == "reversed(list-item)");
    // An ol's type is compared exactly, and a ul's ASCII case-insensitively.
    CHECK(resolved(page, "o", PrCssProperty_ListStyleType) == "decimal");
    CHECK(resolved(page, "u", PrCssProperty_ListStyleType) == "square");
    CHECK(resolved(page, "nested", PrCssProperty_ListStyleType) == "square");
    CHECK(resolved(page, "deep", PrCssProperty_ListStyleType) == "circle");
    CHECK(resolved(page, "third", PrCssProperty_ListStyleType) == "square");
    CHECK(resolved(page, "v", PrCssProperty_ListStylePosition) == "outside");
    prDomElementSetAttribute(byId(page, "v"), "value", "3");
    update(page);
    CHECK(resolved(page, "v", PrCssProperty_CounterSet) == "list-item 3");
}

TEST_CASE("in quirks mode, a list item's marker is inside unless a list holds the item") {
    Page page("<li id=loose></li><ul><li id=held></li></ul>");
    CHECK(resolved(page, "loose", PrCssProperty_ListStylePosition) == "inside");
    CHECK(resolved(page, "held", PrCssProperty_ListStylePosition) == "outside");
}

TEST_CASE("the user agent's rules for quirks mode apply in quirks mode alone") {
    const char* body = "<b><form id=f></form><table id=t></table><textarea id=a></textarea>"
        "<input id=i><input id=m type=image></b>";
    Page quirks(body);
    CHECK(resolved(quirks, "f", PrCssProperty_MarginBottom) == "16px");
    CHECK(resolved(quirks, "t", PrCssProperty_FontWeight) == "400");
    CHECK(resolved(quirks, "a", PrCssProperty_BoxSizing) == "border-box");
    CHECK(resolved(quirks, "i", PrCssProperty_BoxSizing) == "border-box");
    CHECK(resolved(quirks, "m", PrCssProperty_BoxSizing) == "content-box");
    Page standard((std::string("<!doctype html>") + body).c_str());
    CHECK(resolved(standard, "f", PrCssProperty_MarginBottom) == "0px");
    CHECK(resolved(standard, "t", PrCssProperty_FontWeight) == "700");
    CHECK(resolved(standard, "a", PrCssProperty_BoxSizing) == "content-box");
}

TEST_CASE("the rules of presentational hints precede every author rule, whatever their specificity") {
    Page page("<!doctype html><style>p { text-align: right }</style>"
        "<p id=a align=center></p><h1 id=b align=center></h1><td><br id=c clear=all></td>");
    CHECK(resolved(page, "a", PrCssProperty_TextAlign) == "right");
    CHECK(resolved(page, "b", PrCssProperty_TextAlign) == "center");
    CHECK(resolved(page, "c", PrCssProperty_Clear) == "both");
    prDomElementSetAttribute(byId(page, "b"), "align", "left");
    update(page);
    CHECK(resolved(page, "b", PrCssProperty_TextAlign) == "left");
}

TEST_CASE("a table's border attribute styles its cells unless it is equivalent to zero") {
    Page page("<!doctype html><table id=t border><tr><td id=c></table>"
        "<table id=z border=0><tr><td id=d></table><table id=e border=x><tr><td id=f></table>");
    CHECK(resolved(page, "t", PrCssProperty_BorderTopStyle) == "outset");
    CHECK(resolved(page, "t", PrCssProperty_BorderTopWidth) == "1px");
    CHECK(resolved(page, "c", PrCssProperty_BorderTopStyle) == "inset");
    CHECK(resolved(page, "z", PrCssProperty_BorderTopStyle) == "none");
    CHECK(resolved(page, "d", PrCssProperty_BorderTopStyle) == "none");
    CHECK(resolved(page, "e", PrCssProperty_BorderTopWidth) == "1px");
    CHECK(resolved(page, "f", PrCssProperty_BorderTopStyle) == "inset");
    prDomElementSetAttribute(byId(page, "t"), "border", "0");
    update(page);
    CHECK(resolved(page, "c", PrCssProperty_BorderTopStyle) == "none");
}

TEST_CASE("will-change makes the containing block and the stacking context that the properties it names would") {
    Page page("<!doctype html><style>body { margin: 0 }</style>"
        "<div id=t style='will-change: transform; margin-left: 50px; height: 10px'>"
        "<div id=f style='position: fixed; left: 0; width: 5px; height: 5px'></div></div>"
        "<div id=o style='will-change: opacity'></div><div id=n style='will-change: scroll-position'></div>");
    CHECK(page.bounds("f").left == 50.0f);
    CHECK(page.box("t").transformed);
    CHECK(page.effects("o").layer);
    CHECK(!page.box("n").transformed);
    CHECK(!page.effects("n").layer);
}

TEST_CASE("presentational hints map the legacy attributes of each element") {
    Page page("<!doctype html><font id=f size=+2 face=Arimo color=red>x</font><div id=d align=middle></div>"
        "<hr id=h noshade size=4 align=left><img id=i border=2 hspace=3 width=40 height=20>"
        "<canvas id=c width=4 height=2></canvas><textarea id=t wrap=off></textarea><pre id=p wrap></pre>"
        "<caption id=cap align=bottom></caption>");
    CHECK(resolved(page, "f", PrCssProperty_FontSize) == "24px");
    CHECK(resolved(page, "f", PrCssProperty_FontFamily) == "Arimo");
    CHECK(resolved(page, "f", PrCssProperty_Color) == "rgb(255, 0, 0)");
    CHECK(resolved(page, "d", PrCssProperty_TextAlign) == "center");
    CHECK(resolved(page, "h", PrCssProperty_BorderTopStyle) == "solid");
    CHECK(resolved(page, "h", PrCssProperty_BorderTopWidth) == "2px");
    CHECK(resolved(page, "h", PrCssProperty_MarginLeft) == "0px");
    CHECK(resolved(page, "i", PrCssProperty_BorderLeftWidth) == "2px");
    CHECK(resolved(page, "i", PrCssProperty_BorderLeftStyle) == "solid");
    CHECK(resolved(page, "i", PrCssProperty_MarginLeft) == "3px");
    CHECK(resolved(page, "i", PrCssProperty_AspectRatio) == "auto 40 / 20");
    CHECK(resolved(page, "c", PrCssProperty_AspectRatio) == "auto 4 / 2");
    CHECK(resolved(page, "t", PrCssProperty_WhiteSpaceCollapse) == "preserve");
    CHECK(resolved(page, "p", PrCssProperty_TextWrapMode) == "wrap");
}

TEST_CASE("a table's attributes style its cells, which share their style") {
    Page page("<!doctype html><table id=t rules=cols cellpadding=5><tr><td id=a>1<td id=b>2</table>");
    CHECK(resolved(page, "a", PrCssProperty_PaddingTop) == "5px");
    CHECK(resolved(page, "a", PrCssProperty_BorderLeftStyle) == "solid");
    CHECK(resolved(page, "a", PrCssProperty_BorderTopStyle) == "none");
    CHECK(resolved(page, "t", PrCssProperty_BorderTopStyle) == "hidden");
    prDomElementSetAttribute(byId(page, "t"), "cellpadding", "7");
    update(page);
    CHECK(resolved(page, "b", PrCssProperty_PaddingLeft) == "7px");
    prDomElementSetAttribute(byId(page, "t"), "rules", "all");
    update(page);
    CHECK(resolved(page, "b", PrCssProperty_BorderTopStyle) == "solid");
}

TEST_CASE("a body's link colors style the document's links") {
    Page page("<!doctype html><body link=green><a id=a href=#x>x</a><a id=n>y</a>");
    CHECK(resolved(page, "a", PrCssProperty_Color) == "rgb(0, 128, 0)");
    CHECK(resolved(page, "n", PrCssProperty_Color) != "rgb(0, 128, 0)");
    prDomElementSetAttribute(prDomDocumentBody(page.document), "link", "red");
    update(page);
    CHECK(resolved(page, "a", PrCssProperty_Color) == "rgb(255, 0, 0)");
}

TEST_CASE("only the engine's own sheets name its private pseudo-classes") {
    Page page("<!doctype html><style>:-pr-list-box, #s:not(:-pr-list-box) { color: red }</style>"
        "<select id=s></select><select id=l size=3></select>");
    // The author's rule is dropped whole, and the user agent's rules tell a drop-down box from a list box.
    CHECK(resolved(page, "s", PrCssProperty_Color) != "rgb(255, 0, 0)");
    CHECK(resolved(page, "l", PrCssProperty_Color) != "rgb(255, 0, 0)");
    CHECK(resolved(page, "s", PrCssProperty_Display) == "inline-block");
    CHECK(resolved(page, "l", PrCssProperty_Display) == "inline");
    prDomElementSetAttribute(byId(page, "l"), "size", "1");
    update(page);
    CHECK(resolved(page, "l", PrCssProperty_Display) == "inline-block");
}

TEST_CASE("appearance computes to its keyword, which -webkit-appearance sets too") {
    Page page("<!doctype html><input id=i><meter id=m></meter><progress id=p></progress><div id=d></div>"
        "<input id=h type=hidden><input id=w style='-webkit-appearance: none'><input id=c style='appearance: checkbox'>");
    CHECK(resolved(page, "i", PrCssProperty_Appearance) == "auto");
    CHECK(resolved(page, "m", PrCssProperty_Appearance) == "auto");
    CHECK(resolved(page, "p", PrCssProperty_Appearance) == "auto");
    CHECK(resolved(page, "d", PrCssProperty_Appearance) == "none");
    CHECK(resolved(page, "h", PrCssProperty_Appearance) == "none");
    CHECK(resolved(page, "w", PrCssProperty_Appearance) == "none");
    CHECK(resolved(page, "c", PrCssProperty_Appearance) == "checkbox");
    // A legacy name alias reads the property that it is an alias of.
    CHECK(resolved(page, "i", PrCssProperty_WebkitAppearance) == "auto");
}

TEST_CASE("the used appearance follows appearance, the kind of widget, and the author's devolving declarations") {
    Page page("<!doctype html><style>.bg { background-color: red } .logical { border-inline-start-color: red }"
        ".reverted { background-color: red } .reverted { background-color: revert }"
        ".none { appearance: none } .base { appearance: base } .select { appearance: base-select }</style>"
        "<input id=text><input id=none class=none><input id=bg class=bg><input id=logical class=logical>"
        "<input id=reverted class=reverted><input id=inline style='border-top-width: 0'>"
        "<input id=check type=checkbox class=bg><input id=range type=range class=none>"
        "<meter id=meter class=base></meter><select id=select class=select></select>"
        "<button id=button class=select></button><input id=compat style='appearance: textfield'>"
        "<div id=div style='appearance: auto'></div>"
        "<div class=none><input id=inherit style='appearance: inherit'></div>");
    update(page);
    CHECK(usedAppearance(page, "text") == CssAppearance_Native);
    CHECK(usedAppearance(page, "none") == CssAppearance_Primitive);
    CHECK(usedAppearance(page, "bg") == CssAppearance_Primitive);
    CHECK(usedAppearance(page, "logical") == CssAppearance_Primitive);
    CHECK(usedAppearance(page, "reverted") == CssAppearance_Native);
    CHECK(usedAppearance(page, "inline") == CssAppearance_Primitive);
    // A checkbox is non-devolvable, and keeps its native appearance under an author's background.
    CHECK(usedAppearance(page, "check") == CssAppearance_Native);
    CHECK(usedAppearance(page, "range") == CssAppearance_Primitive);
    CHECK(usedAppearance(page, "meter") == CssAppearance_Base);
    CHECK(usedAppearance(page, "select") == CssAppearance_Base);
    CHECK(usedAppearance(page, "button") == CssAppearance_Native);
    CHECK(usedAppearance(page, "compat") == CssAppearance_Native);
    CHECK(usedAppearance(page, "div") == CssAppearance_None);
    CHECK(usedAppearance(page, "inherit") == CssAppearance_Primitive);
    // A change of type changes the kind of widget.
    prDomElementSetAttribute(byId(page, "bg"), "type", "radio");
    update(page);
    CHECK(usedAppearance(page, "bg") == CssAppearance_Native);
}

TEST_CASE("a sheet for one appearance styles the widgets rendered with it, in its place in the cascade") {
    std::vector<PrCssUserAgentSheet> sheets{
        {
            .source = "input { margin-left: 7px } div, .a::before { color: rgb(0, 128, 0) }",
            .appearance = PrCssSheetAppearance_Native,
        },
        {.source = "input { margin-left: 9px }", .appearance = PrCssSheetAppearance_Base},
    };
    Page page("<!doctype html><style>.pad { margin-left: 5px } .none { appearance: none } .base { appearance: base }"
        ".bg { background-color: red } .a::before { content: 'x' }</style>"
        "<input id=native><input id=pad class=pad><input id=none class=none><input id=base class=base>"
        "<input id=bg class=bg><div id=div style='appearance: auto'></div>"
        "<button id=button class=a></button><div id=plain class=a></div>",
        PrVec2{800, 600}, prDomAgentOptionsDefault(), sheets);
    CHECK(resolved(page, "native", PrCssProperty_MarginLeft) == "7px");
    CHECK(resolved(page, "pad", PrCssProperty_MarginLeft) == "5px");
    CHECK(resolved(page, "none", PrCssProperty_MarginLeft) == "0px");
    CHECK(resolved(page, "base", PrCssProperty_MarginLeft) == "9px");
    CHECK(resolved(page, "bg", PrCssProperty_MarginLeft) == "0px");
    CHECK(resolved(page, "div", PrCssProperty_Color) != "rgb(0, 128, 0)");
    // A pseudo-element follows the used appearance of the element that originates it.
    CHECK(resolved(page, "button", PrCssProperty_Color, "::before") == "rgb(0, 128, 0)");
    CHECK(resolved(page, "plain", PrCssProperty_Color, "::before") != "rgb(0, 128, 0)");
    // A change of appearance restyles the element as any property does.
    prDomElementSetAttribute(byId(page, "none"), "class", "");
    update(page);
    CHECK(resolved(page, "none", PrCssProperty_MarginLeft) == "7px");
}

TEST_CASE("controls take a font of 10pt") {
    Page page("<!doctype html><input id=i><button id=b>x</button><select id=s></select><textarea id=t></textarea><p id=p>");
    // 10pt is 13.333px, which a length holds as 853/64.
    for (const char* id : {"i", "b", "s", "t"})
        CHECK(page.fontStyle(id).size == 13.328125f);
    CHECK(page.fontStyle("p").size == 16.0f);
}

TEST_CASE("a control rendered with its base appearance takes its colors and font from the page") {
    Page page("<!doctype html><style>body { color: rgb(0, 128, 0); font-size: 20px } .base { appearance: base }"
        "</style><input id=text class=base><button id=button class=base>x</button><input id=native>");
    for (const char* id : {"text", "button"}) {
        CHECK(resolved(page, id, PrCssProperty_Color) == "rgb(0, 128, 0)");
        CHECK(resolved(page, id, PrCssProperty_FontSize) == "20px");
        CHECK(resolved(page, id, PrCssProperty_BoxSizing) == "border-box");
        CHECK(resolved(page, id, PrCssProperty_BackgroundColor) == "rgba(0, 0, 0, 0)");
        CHECK(resolved(page, id, PrCssProperty_BorderTopColor) == "rgb(0, 128, 0)");
    }
    CHECK(resolved(page, "native", PrCssProperty_BorderTopWidth) == "1px");
    CHECK(resolved(page, "native", PrCssProperty_FontSize) != "20px");
}

TEST_CASE("light-dark() resolves to the color that the scheme chooses, written as that color is") {
    Page page("<!doctype html><div id=legacy style='background-color: light-dark(transparent, red)'></div>"
        "<div id=modern style='background-color: light-dark(color(srgb 1 0 0), red)'></div>");
    CHECK(resolved(page, "legacy", PrCssProperty_BackgroundColor) == "rgba(0, 0, 0, 0)");
    CHECK(resolved(page, "modern", PrCssProperty_BackgroundColor) == "color(srgb 1 0 0)");
}

TEST_CASE("pseudo-elements that match the same entries share a style only under the same appearance") {
    Page page("<!doctype html><style>.a::before { content: 'x' }</style>"
        "<div id=d class=a></div><button id=b class=a></button><div id=e class=a></div>");
    update(page);
    CHECK(usedAppearance(page, "d", true) == CssAppearance_None);
    CHECK(usedAppearance(page, "b", true) == CssAppearance_Native);
    CHECK(usedAppearance(page, "e", true) == CssAppearance_None);
}

TEST_CASE("a body's margin attributes map to its margins, which are otherwise 8px") {
    Page page("<!doctype html><body id=b marginheight=3 leftmargin=5 marginwidth=x>");
    CHECK(resolved(page, "b", PrCssProperty_MarginTop) == "3px");
    CHECK(resolved(page, "b", PrCssProperty_MarginBottom) == "3px");
    // marginwidth is present, so it decides, and it fails to parse.
    CHECK(resolved(page, "b", PrCssProperty_MarginLeft) == "8px");
    CHECK(resolved(page, "b", PrCssProperty_MarginRight) == "8px");
}

TEST_CASE("the user agent's rules for HTML documents apply to an HTML document") {
    Page page("<!doctype html><table><tr><form id=f></form></tr></table>");
    CHECK(resolved(page, "f", PrCssProperty_Display) == "none");
}

TEST_CASE("a change to a feature that a pseudo-element's selector reads restyles its originating element") {
    Page page("<style>.on::before { content: 'x' }</style><div id=a></div>");
    CHECK(resolved(page, "a", PrCssProperty_Content, "::before") == "none");
    prDomElementSetAttribute(byId(page, "a"), "class", "on");
    CHECK(update(page).elementsStyled == 1);
    CHECK(resolved(page, "a", PrCssProperty_Content, "::before") == "\"x\"");
}

TEST_CASE("inherited properties inherit, and relative values compute against the parent") {
    Page page("<style>#outer { color: rgb(255, 0, 0); font-size: 20px; line-height: 1.5 }"
        "#inner { font-size: 2em; padding-left: 1em; border-color: currentcolor }</style>"
        "<div id=outer><div id=inner>text</div></div>");
    CHECK(sameColor(page.color("inner").color, PrColor{1, 0, 0, 1}));
    CHECK(page.fontStyle("inner").size == 40.0f);
    CHECK(page.font("inner").lineHeight.bits == prLayoutLineHeightNumber(1.5f).bits);
    CHECK(px(page.box("inner").padding[PrRectSide_Left]) == 40.0f);
    CHECK(sameColor(page.decoration("inner").borderColor[PrRectSide_Top], PrColor{1, 0, 0, 1}));
}

TEST_CASE("custom properties substitute into values") {
    Page page("<style>:root { --w: 25px; --c: blue }#a { width: var(--w); background-color: var(--c) }"
        "#b { width: var(--missing, 7px) }#c { --w: 3px; width: calc(var(--w) * 2) }</style>"
        "<div id=a></div><div id=b></div><div id=c></div>");
    CHECK(px(page.box("a").width) == 25.0f);
    CHECK(sameColor(page.decoration("a").background, PrColor{0, 0, 1, 1}));
    CHECK(px(page.box("b").width) == 7.0f);
    CHECK(px(page.box("c").width) == 6.0f);
}

TEST_CASE("custom properties layer over the root's, and hide, inherit and override them") {
    // The root declares many properties, so its descendants' sets layer over it rather than copy it.
    std::string html = "<style>:root {";
    for (int i = 0; i < 100; ++i)
        html += " --r" + std::to_string(i) + ": " + std::to_string(i) + "px;";
    html += " --w: 10px }#outer { --a: 4px; --w: 20px }#hide { --w: initial; width: var(--w, 1px) }"
        "#cycle { --w: var(--w); width: var(--w, 2px) }#inherit { --w: inherit; width: var(--w) }"
        "#inner { --b: calc(var(--a) + var(--r5)); width: var(--b) }"
        "#deep { --c: 1px; width: calc(var(--w) + var(--a) + var(--c) + var(--r99)) }#root-only { width: var(--w) }"
        "</style><div id=root-only></div><div id=outer><div id=hide></div><div id=cycle></div><div id=inherit></div>"
        "<div id=inner><div id=deep></div></div></div>";
    Page page(html.c_str());
    CHECK(px(page.box("root-only").width) == 10.0f);
    CHECK(px(page.box("hide").width) == 1.0f);
    CHECK(px(page.box("cycle").width) == 2.0f);
    CHECK(px(page.box("inherit").width) == 20.0f);
    CHECK(px(page.box("inner").width) == 9.0f);
    CHECK(px(page.box("deep").width) == 124.0f);
    CHECK(verified(page.presentation));

    // Changing a property on the root restyles the elements that inherit it, through every layer.
    PrDomElement* root = prDomDocumentElement(page.document);
    prDomElementSetAttribute(root, "style", "--w: 30px; --r99: 1px");
    prHtmlPresentationUpdate(page.presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(px(page.box("root-only").width) == 30.0f);
    CHECK(px(page.box("inherit").width) == 20.0f);
    CHECK(px(page.box("deep").width) == 26.0f);
    CHECK(verified(page.presentation));
}

TEST_CASE("the width and height attributes of an image map to its width and height, under author rules") {
    Page page("<style>#ruled { width: 7px }</style>"
        "<img id=sized width=40 height='25.5'><img id=percent width='50%'><img id=ruled width=40>"
        "<div id=plain width=40></div>");
    CHECK(px(page.box("sized").width) == 40.0f);
    CHECK(px(page.box("sized").height) == 25.5f);
    CHECK(prLayoutWordKind(page.box("percent").width.bits) == PrLayoutWordKind_Percent);
    CHECK(px(page.box("ruled").width) == 7.0f);
    CHECK(prLayoutWordKind(page.box("plain").width.bits) != PrLayoutWordKind_Px);

    // Changing a mapped attribute restyles the element.
    PrDomElement* sized = prDomNodeGetElementById(prDomDocumentAsNode(page.document), "sized");
    prDomElementSetAttribute(sized, "width", "60");
    prHtmlPresentationUpdate(page.presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(prCssPresentationStats(page.presentation).elementsStyled == 1);
    CHECK(px(page.box("sized").width) == 60.0f);
    prDomElementRemoveAttribute(sized, "height");
    prHtmlPresentationUpdate(page.presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(prLayoutWordKind(page.box("sized").height.bits) != PrLayoutWordKind_Px);
    CHECK(verified(page.presentation));
}

TEST_CASE("invalidation reaches custom states, xml:lang, an input's type and the text that decides direction") {
    Page page("<style>#host:has(:state(--on)) { width: 1px }:lang(en) > #lang { width: 2px }"
        "input:checked + #after { width: 3px }#auto:dir(rtl) + #ruled { width: 4px }</style>"
        "<div id=host><x-custom id=custom></x-custom></div><div id=outer><div id=lang></div></div>"
        "<input id=input type=checkbox checked><div id=after></div>"
        "<div id=auto dir=auto><span id=text>abc</span></div><div id=ruled></div>");
    auto element = [&](const char* id) {
        return prDomNodeGetElementById(prDomDocumentAsNode(page.document), id);
    };
    auto update = [&]() { prHtmlPresentationUpdate(page.presentation, prLayoutVecFromPx(PrVec2{800, 600})); };
    CHECK(px(page.box("after").width) == 3.0f);

    prDomElementSetCustomState(element("custom"), "--on", true);
    update();
    CHECK(px(page.box("host").width) == 1.0f);
    CHECK(verified(page.presentation));

    REQUIRE(prDomElementSetAttributeNs(element("outer"), "http://www.w3.org/XML/1998/namespace", "xml:lang", "en") ==
        PrStatus_Ok);
    update();
    CHECK(px(page.box("lang").width) == 2.0f);
    CHECK(verified(page.presentation));

    prDomElementSetAttribute(element("input"), "type", "text");
    update();
    CHECK(px(page.box("after").width) != 3.0f);
    CHECK(verified(page.presentation));

    prDomNodeSetTextContent(prDomElementAsNode(element("text")), "\xd7\xa2\xd7\x91\xd7\xa8\xd7\x99\xd7\xaa");
    update();
    CHECK(px(page.box("ruled").width) == 4.0f);
    CHECK(verified(page.presentation));
    // Text that keeps the direction restyles nothing that reads it, and text that changes it does.
    prDomNodeSetTextContent(prDomElementAsNode(element("text")), "\xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d");
    update();
    CHECK(prCssPresentationStats(page.presentation).elementsStyled == 0);
    CHECK(verified(page.presentation));
    prDomNodeSetTextContent(prDomElementAsNode(element("text")), "abc");
    update();
    CHECK(px(page.box("ruled").width) != 4.0f);
    CHECK(verified(page.presentation));
}

TEST_CASE("an element's locale is that of the tag it declares, or its parent's, and a change restyles its subtree") {
    Page page("<html id=root lang=ja><div id=a></div><div id=b lang=en-GB><span id=c>text</span></div>"
        "<div id=d lang=''></div><div id=e lang='not a tag'></div>");
    auto element = [&](const char* id) {
        return prDomNodeGetElementById(prDomDocumentAsNode(page.document), id);
    };
    auto update = [&]() { prHtmlPresentationUpdate(page.presentation, prLayoutVecFromPx(PrVec2{800, 600})); };
    CHECK(page.locale("a") == prLocaleFromTag("ja"));
    CHECK(page.locale("c") == prLocaleFromTag("en-GB"));
    // An empty tag is an unknown language, and so is one that does not resolve.
    CHECK(page.locale("d") == 0);
    CHECK(page.locale("e") == 0);

    // No selector reads the language, and still a change of it reaches the subtree.
    prDomElementSetAttribute(element("root"), "lang", "zh-TW");
    update();
    CHECK(page.locale("a") == prLocaleFromTag("zh-Hant"));
    CHECK(page.locale("c") == prLocaleFromTag("en-GB"));
    CHECK(verified(page.presentation));

    prDomElementRemoveAttribute(element("b"), "lang");
    update();
    CHECK(page.locale("c") == prLocaleFromTag("zh-TW"));
    CHECK(verified(page.presentation));

    // xml:lang is the language over lang.
    prDomElementSetAttribute(element("b"), "lang", "fr");
    REQUIRE(prDomElementSetAttributeNs(element("b"), "http://www.w3.org/XML/1998/namespace", "xml:lang", "ko") ==
        PrStatus_Ok);
    update();
    CHECK(page.locale("c") == prLocaleFromTag("ko"));
    CHECK(verified(page.presentation));
}

TEST_CASE("a document's default language is its content-language pragma's, or else its Content-Language header's") {
    Page page("");
    PrHtmlHost host = prHtmlHostDefault();
    host.font = page.defaultFont;
    host.shaper = page.shaper;
    PrHtmlPage* htmlPage = prHtmlPageCreate(&host);
    // The locale of the element `a` of `source`, loaded with `language` as its Content-Language header, and whether the
    // element matches :lang(ko).
    struct Answer {
        PrLocale locale;
        bool korean;
    };
    auto answer = [&]() {
        PrCssPresentation* presentation = prHtmlPagePresentation(htmlPage);
        prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
        PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(htmlPage));
        PrDomElement* a = prDomNodeGetElementById(prDomDocumentAsNode(document), "a");
        REQUIRE(a != nullptr);
        PrLayoutNode node = prCssPresentationLayoutNode(presentation, prDomElementAsNode(a));
        PrLayoutTree* tree = prCssPresentationTree(presentation);
        PrLayoutStyle style = prLayoutStyleRecord(tree, prLayoutNodeStyle(tree, node));
        PrLayoutFontStyle font = prLayoutFontStyleRecord(tree, style.groups[PrLayoutGroup_Font]);
        PrLayoutBoxStyle box = prLayoutBoxStyleRecord(tree, style.groups[PrLayoutGroup_Box]);
        CHECK(verified(presentation));
        PrTextFont* text = font.font != nullptr ? font.font : prLayoutTreeDefaultFont(tree);
        return Answer{prTextFontLocale(text), px(box.width) == 1.0f};
    };
    auto load = [&](const char* source, const char* language) {
        std::string text = std::string("<style>#a:lang(ko) { width: 1px }</style>") + source;
        PrHtmlResponseHeaders headers{};
        headers.contentLanguage = language != nullptr ? language : PrStringView{nullptr, 0};
        REQUIRE(prHtmlPageLoad(htmlPage, text, "about:blank", &headers) == PrStatus_Ok);
        return answer();
    };
    PrLocale ko = prLocaleFromTag("ko");
    PrLocale ja = prLocaleFromTag("ja");

    // The header's first language, if several are given.
    Answer header = load("<div id=a></div>", "ko, ja");
    CHECK(header.locale == ko);
    CHECK(header.korean);
    CHECK(load("<div id=a></div>", nullptr).locale == 0);
    // The pragma takes precedence over the header, and the element's own language over both.
    CHECK(load("<meta http-equiv=Content-Language content=' ko other'><div id=a></div>", "ja").locale == ko);
    CHECK(load("<meta http-equiv=content-language content=ko><div id=a lang=ja></div>", "zh").locale == ja);
    // A pragma of several languages sets none, and an empty lang stops the walk at unknown.
    CHECK(load("<meta http-equiv=content-language content='ko,zh'><div id=a></div>", "ja").locale == ja);
    Answer empty = load("<html lang=''><meta http-equiv=content-language content=ko><div id=a></div>", nullptr);
    CHECK(empty.locale == 0);
    CHECK(!empty.korean);

    // A pragma inserted later restyles the document.
    load("<div id=a></div>", "ja");
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(htmlPage));
    PrDomElement* meta = nullptr;
    REQUIRE(prDomElementCreate(document, "meta", nullptr, &meta) == PrStatus_Ok);
    prDomElementSetAttribute(meta, "http-equiv", "content-language");
    prDomElementSetAttribute(meta, "content", "ko");
    REQUIRE(prDomNodeAppendChild(prDomElementAsNode(prDomDocumentHead(document)), prDomElementAsNode(meta)) ==
        PrStatus_Ok);
    prDomNodeRelease(prDomElementAsNode(meta));
    Answer inserted = answer();
    CHECK(inserted.locale == ko);
    CHECK(inserted.korean);
    prHtmlPageDestroy(htmlPage);
}

TEST_CASE("an element with display none generates no box") {
    Page page("<style>#gone { display: none }</style><div id=gone><div id=child></div></div><div id=there></div>");
    CHECK(page.node("gone") == kPrLayoutNodeNone);
    CHECK(page.node("child") == kPrLayoutNodeNone);
    CHECK(page.node("there") != kPrLayoutNodeNone);
}

TEST_CASE("a border width is zero where the side has no border style") {
    Page page("<style>#a { border-width: 5px } #b { border: 5px solid }</style><div id=a></div><div id=b></div>");
    CHECK(prLayoutUnitToPx(page.box("a").border[PrRectSide_Top]) == 0.0f);
    CHECK(prLayoutUnitToPx(page.box("b").border[PrRectSide_Top]) == 5.0f);
    CHECK(page.decoration("b").borderStyle[PrRectSide_Top] == PrDrawBorderStyle_Solid);
}

TEST_CASE("the body's background is the canvas's when the root has none") {
    Page page("<html id=r><body id=b style='background-color: green'></body></html>");
    CHECK(sameColor(page.decoration("r").background, PrColor{0, 128.0f / 255, 0, 1}));
    CHECK(prColorFromColor16(page.decoration("b").background).a == 0.0f);
}

TEST_CASE("the canvas loses the body's background with the body") {
    Page page("<html id=r><body id=b style='background: red'></body></html>");
    CHECK(sameColor(page.decoration("r").background, PrColor{1, 0, 0, 1}));
    PrDomElement* body = prDomDocumentBody(page.document);
    prDomNodeRemove(prDomElementAsNode(body));
    prHtmlPresentationUpdate(page.presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(prColorFromColor16(page.decoration("r").background).a == 0.0f);
    CHECK(verified(page.presentation));
}

TEST_CASE("the body's overflow is the viewport's when the root's is visible") {
    Page page("<body id=b style='overflow: hidden; height: 50px'><div style='height: 900px'></div></body>");
    CHECK(page.box("b").overflowY == PrLayoutOverflow_Visible);
    PrLayoutScrollInfo info{};
    REQUIRE(prLayoutNodeScrollInfo(prCssPresentationTree(page.presentation), kPrLayoutNodeNone, &info));
    CHECK(info.userAxes == 0);
    CHECK(prLayoutUnitToPx(info.range.bottom) > 0.0f);
    Page scrolling("<html style='overflow: scroll'><body id=b style='overflow: hidden'></body></html>");
    CHECK(scrolling.box("b").overflowY == PrLayoutOverflow_Hidden);
}

TEST_CASE("an element with no border style takes no room for a border") {
    Page page("<style>body{margin:0}</style><div id=d><span id=s>x</span></div>");
    CHECK(prLayoutUnitToPx(page.box("s").border[PrRectSide_Left]) == 0.0f);
    CHECK(prLayoutUnitToPx(page.box("d").border[PrRectSide_Top]) == 0.0f);
    CHECK(double(page.bounds("d").left) == doctest::Approx(0.0));
}

TEST_CASE("line-height is of the element's own font size, a calculation computes, and the resolved value is a length") {
    Page page("<style>body { font: 20px/40px Arimo }</style><div id=pct style='font-size: 40px; line-height: 150%'>x</div>"
        "<div id=em style='font-size: 40px; line-height: 1.5em'>x</div>"
        "<div id=short style='font: 40px/150% Arimo'>x</div><div id=mixed style='line-height: calc(10% + 30px)'>x</div>"
        "<div id=num style='line-height: calc(1.5 * 2)'>x<div id=child style='font-size: 10px'>x</div></div>"
        "<div id=neg style='line-height: calc(10px - 1em)'>x</div>"
        "<div id=two style='line-height: 2'>x</div><div id=zero style='line-height: 0'>x</div>"
        "<div id=normal style='line-height: normal'>x</div>");
    CHECK(resolved(page, "pct", PrCssProperty_LineHeight) == "60px");
    CHECK(resolved(page, "em", PrCssProperty_LineHeight) == "60px");
    CHECK(resolved(page, "short", PrCssProperty_LineHeight) == "60px");
    CHECK(resolved(page, "mixed", PrCssProperty_LineHeight) == "32px");
    CHECK(page.font("num").lineHeight.bits == prLayoutLineHeightNumber(3).bits);
    CHECK(resolved(page, "num", PrCssProperty_LineHeight) == "60px");
    CHECK(resolved(page, "child", PrCssProperty_LineHeight) == "30px");
    CHECK(resolved(page, "neg", PrCssProperty_LineHeight) == "0px");
    CHECK(resolved(page, "two", PrCssProperty_LineHeight) == "40px");
    CHECK(resolved(page, "zero", PrCssProperty_LineHeight) == "0px");
    CHECK(resolved(page, "normal", PrCssProperty_LineHeight) == "normal");
    CHECK(verified(page.presentation));
}

TEST_CASE("lh and rlh are of the computed line height, the parent's in font-size and line-height, and zero is one") {
    Page page("<style>html { font: 16px/25px Arimo } body { font: 20px/40px Arimo }</style>"
        "<div style='line-height: 30px'><div id=px style='height: 2lh'></div>"
        "<div id=lhlh style='line-height: 2lh; height: 1lh'></div></div>"
        "<div style='line-height: 1.5'><div id=own style='font-size: 40px; height: 1lh'></div></div>"
        "<div style='font-size: 10px; line-height: 3'><div id=fs style='font-size: 1lh'>x</div></div>"
        "<div style='line-height: 0'><div id=zero style='height: 1lh'></div>"
        "<div id=sum style='height: calc(1lh + 10px)'></div><div id=zfs style='font-size: 1lh'>x</div></div>"
        "<div id=rlh style='height: 2rlh'></div>");
    CHECK(resolved(page, "px", PrCssProperty_Height) == "60px");
    CHECK(resolved(page, "lhlh", PrCssProperty_LineHeight) == "60px");
    CHECK(resolved(page, "lhlh", PrCssProperty_Height) == "60px");
    CHECK(resolved(page, "own", PrCssProperty_Height) == "60px");
    CHECK(resolved(page, "fs", PrCssProperty_FontSize) == "30px");
    CHECK(resolved(page, "zero", PrCssProperty_Height) == "0px");
    CHECK(resolved(page, "sum", PrCssProperty_Height) == "10px");
    CHECK(resolved(page, "zfs", PrCssProperty_FontSize) == "0px");
    CHECK(resolved(page, "rlh", PrCssProperty_Height) == "50px");
    // The root's line height reaches every rlh, in the same way that its font size reaches every rem.
    prDomElementSetAttribute(prDomDocumentElement(page.document), "style", "line-height: 10px");
    update(page);
    CHECK(resolved(page, "rlh", PrCssProperty_Height) == "20px");
    // A line height of zero, and back, recomputes the lengths in lh.
    prDomElementSetAttribute(byId(page, "zero"), "style", "height: 1lh; line-height: 5px");
    update(page);
    CHECK(resolved(page, "zero", PrCssProperty_Height) == "5px");
}

TEST_CASE("the root's own rem and rlh are of its font size and line height past them, and of the initial font in them") {
    Page page("<style>@keyframes indent { from, to { text-indent: 2rem } }"
        "html { font-size: calc(1rem + 24px); line-height: 2rlh; text-indent: 1rem }"
        "html.moving { font-size: 30px; animation: indent 1s paused }</style>"
        "<div id=child></div><div id=rlh style='text-indent: 1rlh'></div>");
    PrDomElement* root = prDomDocumentElement(page.document);
    prDomElementSetAttribute(root, "id", "root");
    update(page);
    // The initial font's 16px and its normal line height, 1.2em, in font-size and line-height.
    CHECK(resolved(page, "root", PrCssProperty_FontSize) == "40px");
    CHECK(resolved(page, "root", PrCssProperty_LineHeight) == "38.4px");
    // The root's own, past them, and inherited as computed.
    CHECK(resolved(page, "root", PrCssProperty_TextIndent) == "40px");
    CHECK(resolved(page, "child", PrCssProperty_TextIndent) == "40px");
    CHECK(resolved(page, "rlh", PrCssProperty_TextIndent) == "38.4px");
    // A keyframe's, alike, of the font size it changes to.
    prDomElementSetAttribute(root, "class", "moving");
    update(page);
    CHECK(resolved(page, "root", PrCssProperty_TextIndent) == "60px");
}

TEST_CASE("text-align is a shorthand of text-align-all and text-align-last, and match-parent reads the parent") {
    Page page("<style>.rtl { direction: rtl; text-align-all: start; text-align-last: center }</style>"
        "<div id=reset style='text-align-last: right; text-align: center'></div>"
        "<div id=all style='text-align: justify-all'></div>"
        "<div class=rtl><div id=match style='direction: ltr; text-align: match-parent'></div>"
        "<div id=last style='direction: ltr; text-align-last: match-parent'></div></div>"
        "<div style='text-align: center'><div id=center style='text-align: match-parent'></div></div>"
        "<div style='direction: rtl; text-align-last: end'><div id=end style='text-align-last: match-parent'>"
        "</div></div>");
    CHECK(resolved(page, "reset", PrCssProperty_TextAlignLast) == "auto");
    CHECK(resolved(page, "reset", PrCssProperty_TextAlign) == "center");
    CHECK(resolved(page, "all", PrCssProperty_TextAlignAll) == "justify");
    CHECK(resolved(page, "all", PrCssProperty_TextAlign) == "justify-all");
    CHECK(resolved(page, "match", PrCssProperty_TextAlignAll) == "right");
    CHECK(resolved(page, "match", PrCssProperty_TextAlignLast) == "center");
    CHECK(resolved(page, "last", PrCssProperty_TextAlignLast) == "center");
    CHECK(resolved(page, "center", PrCssProperty_TextAlignAll) == "center");
    CHECK(resolved(page, "end", PrCssProperty_TextAlignLast) == "left");
    CHECK(page.font("match").textAlign == PrLayoutTextAlign_Right);
    CHECK(page.font("all").textAlignLast == PrLayoutTextAlign_Justify);
    // The root's match-parent is start, for each longhand.
    Page root("<html id=r style='text-align-all: center; text-align-last: match-parent'><body></body></html>");
    CHECK(resolved(root, "r", PrCssProperty_TextAlignLast) == "start");
    CHECK(verified(page.presentation));
}

TEST_CASE("the root's used writing mode and direction are the body's, the principal writing mode") {
    Page page("<html id=r style='writing-mode: horizontal-tb'><body id=b style='writing-mode: vertical-rl; "
        "direction: rtl'><p id=p>x</p></body></html>");
    CHECK(page.font("r").writingMode == PrLayoutWritingMode_VerticalRl);
    CHECK(page.font("r").direction == PrTextDirection_Rtl);
    CHECK(page.font("b").writingMode == PrLayoutWritingMode_VerticalRl);
    // The root's computed values are its own, which its children would inherit.
    CHECK(resolved(page, "r", PrCssProperty_WritingMode) == "horizontal-tb");
    CHECK(verified(page.presentation));
}

TEST_CASE("containment on the root or the body keeps the body's writing mode and overflow from the root") {
    for (const char* on : {"r", "b"}) {
        CAPTURE(on);
        std::string root = std::string("<html id=r") + (on[0] == 'r' ? " style='contain: layout'" : "") + ">";
        std::string body = std::string("<body id=b style='writing-mode: vertical-rl; overflow: hidden") +
            (on[0] == 'b' ? "; contain: paint'" : "'") + "><p>x</p></body></html>";
        Page page((root + body).c_str());
        CHECK(page.font("r").writingMode == PrLayoutWritingMode_HorizontalTb);
        CHECK(page.box("b").overflowX == PrLayoutOverflow_Hidden);
        CHECK(verified(page.presentation));
    }
    // content-visibility and container-type contain too.
    Page visibility("<html id=r><body id=b style='writing-mode: vertical-rl; content-visibility: auto'></body></html>");
    CHECK(visibility.font("r").writingMode == PrLayoutWritingMode_HorizontalTb);
    Page container("<html id=r><body id=b style='writing-mode: vertical-rl; container-type: size'></body></html>");
    CHECK(container.font("r").writingMode == PrLayoutWritingMode_HorizontalTb);
}

TEST_CASE("writing-mode, text-orientation, dominant-baseline and unicode-bidi reach layout") {
    Page page("<div id=s style='writing-mode: sideways-lr'></div>"
        "<div id=u style='writing-mode: vertical-lr; text-orientation: upright; dominant-baseline: central'>"
        "</div><span id=o style='unicode-bidi: bidi-override'></span>");
    CHECK(page.font("s").writingMode == PrLayoutWritingMode_SidewaysLr);
    CHECK(page.font("u").textOrientation == PrLayoutTextOrientation_Upright);
    CHECK(page.font("u").dominantBaseline == PrLayoutDominantBaseline_Central);
    CHECK(page.box("o").unicodeBidi == PrTextBidi_BidiOverride);
}

TEST_CASE("glyph-orientation-vertical is a shorthand of text-orientation") {
    // Its angles and numbers are values, not keywords: 0.0 is 0, and 100grad is 90deg.
    Page page("<div id=a style='glyph-orientation-vertical: 0deg'></div><div id=b style='glyph-orientation-vertical: 90'></div>"
        "<div id=c style='text-orientation: upright; glyph-orientation-vertical: auto'></div>"
        "<div id=d style='glyph-orientation-vertical: 45deg'></div><div id=e style='glyph-orientation-vertical: 0.0'></div>"
        "<div id=f style='glyph-orientation-vertical: 100grad'></div><div id=g style='glyph-orientation-vertical: 0px'></div>");
    CHECK(resolved(page, "a", PrCssProperty_TextOrientation) == "upright");
    CHECK(resolved(page, "a", PrCssProperty_GlyphOrientationVertical) == "0deg");
    CHECK(resolved(page, "b", PrCssProperty_TextOrientation) == "sideways");
    CHECK(resolved(page, "b", PrCssProperty_GlyphOrientationVertical) == "90deg");
    CHECK(resolved(page, "c", PrCssProperty_TextOrientation) == "mixed");
    CHECK(resolved(page, "d", PrCssProperty_TextOrientation) == "mixed");
    CHECK(resolved(page, "e", PrCssProperty_TextOrientation) == "upright");
    CHECK(resolved(page, "f", PrCssProperty_TextOrientation) == "sideways");
    CHECK(resolved(page, "g", PrCssProperty_TextOrientation) == "mixed");
}

TEST_CASE("the user agent sheet isolates blocks, bdi and dir, overrides bdo, and sets pre dir=auto as plaintext") {
    Page page("<div id=d></div><p id=p></p><span id=s></span><bdi id=i></bdi><span id=dir dir=rtl></span>"
        "<bdo id=o dir=rtl></bdo><pre id=pre dir=auto></pre><pre id=plain></pre>");
    CHECK(page.box("d").unicodeBidi == PrTextBidi_Isolate);
    CHECK(page.box("p").unicodeBidi == PrTextBidi_Isolate);
    CHECK(page.box("s").unicodeBidi == PrTextBidi_Normal);
    CHECK(page.box("i").unicodeBidi == PrTextBidi_Isolate);
    CHECK(page.box("dir").unicodeBidi == PrTextBidi_Isolate);
    CHECK(page.box("o").unicodeBidi == PrTextBidi_IsolateOverride);
    CHECK(page.box("pre").unicodeBidi == PrTextBidi_Plaintext);
    CHECK(page.box("plain").unicodeBidi == PrTextBidi_Isolate);
}

TEST_CASE("scrollLeft and scrollTop are below zero from a right or bottom scroll origin") {
    Page page("<style>.s { width: 100px; height: 50px; overflow: scroll } .w { width: 300px; height: 10px }"
        " .t { width: 10px; height: 300px }</style><div id=rtl class=s style='direction: rtl'><div class=w></div></div>"
        "<div id=vrl class=s style='writing-mode: vertical-rl'><div class=w></div></div>"
        "<div id=up class=s style='writing-mode: vertical-rl; direction: rtl'><div class=t></div></div>");
    for (const char* id : {"rtl", "vrl"}) {
        CAPTURE(id);
        PrDomElement* element = byId(page, id);
        CHECK(prCssElementScrollLeft(element) == 0);
        CHECK(prCssElementScrollWidth(element) == 300);
        prCssElementSetScrollLeft(element, -1e6);
        CHECK(prCssElementScrollLeft(element) == -200);
        prCssElementSetScrollLeft(element, 50);
        CHECK(prCssElementScrollLeft(element) == 0);
    }
    PrDomElement* up = byId(page, "up");
    CHECK(prCssElementScrollHeight(up) == 300);
    prCssElementSetScrollTop(up, -1e6);
    CHECK(prCssElementScrollTop(up) == -250);
    CHECK(verified(page.presentation));
}

TEST_CASE("scrollWidth and scrollHeight are a box's scrolling area, whether or not it scrolls") {
    Page page("<style>body { margin: 0 } .w { width: 90px; border: 10px solid } .i { margin: -10px; width: 100px;"
        " height: 100px } .c { width: 100px; height: 101px } .k { width: 200px; height: 201px }"
        " .t { height: 200px; overflow: hidden; font-size: 100px } .t div { height: 200px } .t p { height: 1px }"
        "</style><div id=visible class=w><div class=i></div></div>"
        "<div id=hidden class=w style='overflow: hidden'><div class=i></div></div>"
        "<div id=clip class=c style='overflow: clip'><div class=k></div></div>"
        "<div id=scroller class=t><div><div></div><p></p></div></div>");
    // A child that reaches the padding box's far edges adds nothing, and a scroll container's own border is not
    // scrolled to.
    for (const char* id : {"visible", "hidden"}) {
        CAPTURE(id);
        CHECK(prCssElementScrollWidth(byId(page, id)) == 90);
        CHECK(prCssElementScrollHeight(byId(page, id)) == 90);
    }
    // A box that clips reports what it clips.
    CHECK(prCssElementScrollWidth(byId(page, "clip")) == 200);
    CHECK(prCssElementScrollHeight(byId(page, "clip")) == 201);
    // A paragraph's block-end margin past the box that holds it is scrolled to: 200 of filler, 100 of margin, 1 of
    // paragraph and 100 of margin.
    CHECK(prCssElementScrollHeight(byId(page, "scroller")) == 401);
    CHECK(verified(page.presentation));
}

TEST_CASE("elements that match the same rules under the same inherited style take one computed style") {
    Page page("<style>.card { color: green; --gap: 4px } .card p { margin-top: var(--gap) }</style>"
        "<div id=a class=card><p id=pa>a</p></div><div id=b class=card><p id=pb>b</p></div>");
    CHECK(verified(page.presentation));
    // The second card and its paragraph reuse the styles computed for the first card and its paragraph.
    CHECK(prCssPresentationStats(page.presentation).elementsStyledCached >= 2);
    CHECK(resolved(page, "pb", PrCssProperty_MarginTop) == "4px");
    CHECK(resolved(page, "pb", PrCssProperty_Color) == "rgb(0, 128, 0)");

    // A class toggled back and forth takes the style it had.
    prDomElementSetAttribute(byId(page, "b"), "class", "other");
    update(page);
    CHECK(resolved(page, "pb", PrCssProperty_MarginTop) == "16px");
    prDomElementSetAttribute(byId(page, "b"), "class", "card");
    CHECK(update(page).elementsStyledCached == 2);
    CHECK(resolved(page, "pb", PrCssProperty_MarginTop) == "4px");
}

TEST_CASE("an element restyled from the same inputs keeps its style, however many styles came between") {
    // The root declares custom properties, which are costly to compute again. Hundreds of distinct styles follow it,
    // more than the matched properties cache holds.
    std::string markup = "<style>:root { --a: 1px; --b: 2px } :root[data-mode=dark] { --b: 3px } p { margin-top: var(--a) }";
    for (uint32_t i = 0; i < 400; ++i)
        markup += ".c" + std::to_string(i) + " { padding-top: " + std::to_string(i) + "px }";
    markup += "</style>";
    for (uint32_t i = 0; i < 400; ++i)
        markup += "<p class=c" + std::to_string(i) + ">p</p>";
    markup += "<p id=last>p</p>";
    Page page(markup.c_str());
    CHECK(verified(page.presentation));
    // An attribute that a selector reads, set to a value that it does not match, restyles the root alone, which takes
    // the style it had.
    PrDomElement* root = prDomDocumentElement(page.document);
    prDomElementSetAttribute(root, "data-mode", "light");
    PrCssPresentationStats stats = update(page);
    CHECK(stats.elementsStyled == 1);
    CHECK(stats.elementsStyledCached == 1);
    CHECK(resolved(page, "last", PrCssProperty_MarginTop) == "1px");
    CHECK(verified(page.presentation));
    // A change to what it declares computes it again.
    prDomElementSetAttribute(root, "style", "--a: 7px");
    update(page);
    CHECK(resolved(page, "last", PrCssProperty_MarginTop) == "7px");
    CHECK(verified(page.presentation));
}

TEST_CASE("a kept style is not taken where what it was computed from differs") {
    Page page("<style>.big { font-size: 20px; --gap: 1px } .c { margin-top: 1em; padding-top: var(--gap, 3px) }</style>"
        "<div id=a class=big><p id=pa class=c>a</p></div><div id=b><p id=pb class=c>b</p></div>"
        "<p id=s class=c style='color: red'>s</p><p id=t class=c>t</p>");
    CHECK(verified(page.presentation));
    // Under a parent of another font size and other custom properties.
    CHECK(resolved(page, "pa", PrCssProperty_MarginTop) == "20px");
    CHECK(resolved(page, "pb", PrCssProperty_MarginTop) == "16px");
    CHECK(resolved(page, "pa", PrCssProperty_PaddingTop) == "1px");
    CHECK(resolved(page, "pb", PrCssProperty_PaddingTop) == "3px");
    // With declarations of its own, which change in place.
    CHECK(resolved(page, "s", PrCssProperty_Color) == "rgb(255, 0, 0)");
    CHECK(resolved(page, "t", PrCssProperty_Color) == "rgb(0, 0, 0)");
    prDomElementSetAttribute(byId(page, "s"), "style", "color: blue");
    update(page);
    CHECK(resolved(page, "s", PrCssProperty_Color) == "rgb(0, 0, 255)");
}

TEST_CASE("a change to a rule's declarations reaches the elements whose styles were kept") {
    Page page("<style>.c { color: green }</style><p id=a class=c>a</p><p id=b class=c>b</p>");
    CHECK(verified(page.presentation));
    PrCssStyleSheet* sheet = prCssStyleSheetListStyleSheetAt(prCssDocumentStyleSheets(page.document), 0);
    REQUIRE(sheet != nullptr);
    PrCssRule* rule = prCssRuleListRuleAt(prCssStyleSheetCssRules(sheet), 0);
    PrCssDeclarationName color = prCssDeclarationNameFromProperty(PrCssProperty_Color);
    prCssStyleDeclarationSetProperty(prCssStylePropertiesAsCssStyleDeclaration(
        prCssStyleRuleStyle(prCssStyleRuleQuery(rule))), &color, "blue", false);
    update(page);
    CHECK(resolved(page, "a", PrCssProperty_Color) == "rgb(0, 0, 255)");
    CHECK(resolved(page, "b", PrCssProperty_Color) == "rgb(0, 0, 255)");
}

TEST_CASE("a change only its parent's style makes restyles an element from what it matched, and one of its own matches") {
    Page page("<style>:root { --c: red } p { color: var(--c) } .x p { margin-top: 3px } .x p.y { margin-top: 5px }"
        "</style><div class=x><p id=p>p</p><p id=q>q</p></div>");
    CHECK(verified(page.presentation));
    PrDomElement* root = prDomDocumentElement(page.document);
    prDomElementSetAttribute(root, "style", "--c: blue");
    update(page);
    CHECK(resolved(page, "p", PrCssProperty_Color) == "rgb(0, 0, 255)");
    CHECK(resolved(page, "p", PrCssProperty_MarginTop) == "3px");
    // The same update restyles one paragraph for its parent and for its own class.
    prDomElementSetAttribute(root, "style", "--c: green");
    prDomElementSetAttribute(byId(page, "q"), "class", "y");
    update(page);
    CHECK(resolved(page, "p", PrCssProperty_Color) == "rgb(0, 128, 0)");
    CHECK(resolved(page, "q", PrCssProperty_Color) == "rgb(0, 128, 0)");
    CHECK(resolved(page, "p", PrCssProperty_MarginTop) == "3px");
    CHECK(resolved(page, "q", PrCssProperty_MarginTop) == "5px");
    // A class on an ancestor that changes what its descendants match.
    prDomElementSetAttribute(root, "style", "--c: blue");
    prDomElementSetAttribute(prDomNodeParentElement(prDomElementAsNode(byId(page, "p"))), "class", "");
    update(page);
    CHECK(resolved(page, "p", PrCssProperty_MarginTop) == "16px");
    CHECK(resolved(page, "q", PrCssProperty_Color) == "rgb(0, 0, 255)");
}

TEST_CASE("a selector's ancestors are matched through their ids, classes and types, and only those that carry them") {
    Page page("<style>#r .a > section.b em { color: green } .x span { color: blue } .y + div em { color: red }</style>"
        "<div id=r><div class=a><section class=b><p><em id=yes>e</em></p></section></div>"
        "<div class=a><div class=b><em id=type>e</em></div></div><section class=b><em id=child>e</em></section></div>"
        "<div id=other class=a><section class=b><em id=root>e</em></section></div>"
        "<div id=late><span id=s>s</span></div><p class=y></p><div><em id=sibling>e</em></div>");
    CHECK(verified(page.presentation));
    CHECK(resolved(page, "yes", PrCssProperty_Color) == "rgb(0, 128, 0)");
    CHECK(resolved(page, "type", PrCssProperty_Color) == "rgb(0, 0, 0)");
    CHECK(resolved(page, "child", PrCssProperty_Color) == "rgb(0, 0, 0)");
    CHECK(resolved(page, "root", PrCssProperty_Color) == "rgb(0, 0, 0)");
    // A sibling combinator leaves the ancestors beyond it to the match.
    CHECK(resolved(page, "sibling", PrCssProperty_Color) == "rgb(255, 0, 0)");
    // An ancestor that gains a class.
    CHECK(resolved(page, "s", PrCssProperty_Color) == "rgb(0, 0, 0)");
    prDomElementSetAttribute(byId(page, "late"), "class", "x");
    update(page);
    CHECK(resolved(page, "s", PrCssProperty_Color) == "rgb(0, 0, 255)");
}

TEST_CASE("flow-root and inline-block reach layout in their legacy and two-keyword forms, with used sizes") {
    Page page("<style>body { margin: 0 } #fr { display: flow-root } #fr2 { display: block flow-root }"
        "#ib { display: inline-block; width: 30px; height: 10px } #ib2 { display: inline flow-root }</style>"
        "<div id=fr><p>x</p></div><div id=fr2></div><span id=ib></span><span id=ib2>x</span><span id=s>x</span>");
    CHECK(page.box("fr").display == PrLayoutDisplay_FlowRoot);
    CHECK(page.box("fr2").display == PrLayoutDisplay_FlowRoot);
    CHECK(page.box("ib").display == PrLayoutDisplay_InlineBlock);
    CHECK(page.box("ib2").display == PrLayoutDisplay_InlineBlock);
    CHECK(resolved(page, "fr2", PrCssProperty_Display) == "flow-root");
    CHECK(resolved(page, "ib2", PrCssProperty_Display) == "inline-block");
    // The flow-root holds its paragraph's margins: 16px above and below a line.
    CHECK(page.bounds("fr").top == 0.0f);
    CHECK(resolved(page, "fr", PrCssProperty_Width) == "800px");
    CHECK(page.bounds("fr").bottom - page.bounds("fr").top > 32.0f);
    CHECK(resolved(page, "ib", PrCssProperty_Width) == "30px");
    CHECK(resolved(page, "ib", PrCssProperty_Height) == "10px");
    // An inline box's sizes are its computed values.
    CHECK(resolved(page, "s", PrCssProperty_Width) == "auto");
    CHECK(verified(page.presentation));
}

TEST_CASE("resolved sizes take the padding layout resolved against the containing block, through inline wrappers") {
    // The containing block is 200px wide inside its own padding and border, which percentages do not read, and the
    // inline wrappers around each box are no containing blocks: 10% is 20px on every side, and 5% is 10px.
    Page page("<style>body { margin: 0 } #cb { width: 200px; padding: 0 30px; border-left: 7px solid }"
        "#ib, #img { width: 20px; height: 10px; padding: 10% 5% }"
        "#bb { display: inline-block; box-sizing: border-box; width: 60px; height: 50px; padding: 10% }"
        "#ib { display: inline-block }</style>"
        "<div id=cb><span><span id=ib></span></span><span><img id=img></span><span><span id=bb></span></span></div>");
    CHECK(page.bounds("ib").right - page.bounds("ib").left == 40.0f);
    CHECK(page.bounds("ib").bottom - page.bounds("ib").top == 50.0f);
    for (const char* id : {"ib", "img"}) {
        CAPTURE(id);
        CHECK(resolved(page, id, PrCssProperty_Width) == "20px");
        CHECK(resolved(page, id, PrCssProperty_Height) == "10px");
    }
    CHECK(resolved(page, "bb", PrCssProperty_Width) == "60px");
    CHECK(resolved(page, "bb", PrCssProperty_Height) == "50px");
    CHECK(verified(page.presentation));
}

TEST_CASE("a flow-relative property computes onto its physical counterpart by the element's own writing mode") {
    Page page("<div id=ltr style='margin-inline-start: 1px; margin-inline-end: 2px; margin-block-start: 3px'>x</div>"
        "<div id=rtl style='direction: rtl; margin-inline-start: 1px; padding-inline-end: 4px'>x</div>"
        "<div id=vrl style='writing-mode: vertical-rl; margin-block-start: 5px; inset-inline-start: 6px;"
        " position: relative'>x</div><div id=vlr style='writing-mode: vertical-lr; direction: rtl; margin-block-end: 7px;"
        " margin-inline-start: 8px'>x</div><div id=slr style='writing-mode: sideways-lr; margin-inline-start: 9px'>x</div>"
        "<div id=sizes style='writing-mode: vertical-rl; inline-size: 40px; block-size: 30px;"
        " min-inline-size: 10px; max-block-size: 50px'>x</div>");
    CHECK(resolved(page, "ltr", PrCssProperty_MarginLeft) == "1px");
    CHECK(resolved(page, "ltr", PrCssProperty_MarginRight) == "2px");
    CHECK(resolved(page, "ltr", PrCssProperty_MarginTop) == "3px");
    CHECK(resolved(page, "rtl", PrCssProperty_MarginRight) == "1px");
    CHECK(resolved(page, "rtl", PrCssProperty_PaddingLeft) == "4px");
    // vertical-rl's block-start is its right, and its inline-start its top.
    CHECK(resolved(page, "vrl", PrCssProperty_MarginRight) == "5px");
    CHECK(resolved(page, "vrl", PrCssProperty_Top) == "6px");
    // vertical-lr's block-end is its right, and in rtl its inline-start is its bottom.
    CHECK(resolved(page, "vlr", PrCssProperty_MarginRight) == "7px");
    CHECK(resolved(page, "vlr", PrCssProperty_MarginBottom) == "8px");
    // sideways-lr's lines read from the bottom.
    CHECK(resolved(page, "slr", PrCssProperty_MarginBottom) == "9px");
    CHECK(resolved(page, "sizes", PrCssProperty_Height) == "40px");
    CHECK(resolved(page, "sizes", PrCssProperty_Width) == "30px");
    CHECK(resolved(page, "sizes", PrCssProperty_MinHeight) == "10px");
    CHECK(resolved(page, "sizes", PrCssProperty_MaxWidth) == "50px");
    // A flow-relative property's value, as a script reads it, is the value of its physical counterpart.
    CHECK(resolved(page, "rtl", PrCssProperty_MarginInlineStart) == "1px");
    CHECK(resolved(page, "sizes", PrCssProperty_InlineSize) == "40px");
    // Layout receives the physical values.
    CHECK(page.box("rtl").padding[PrRectSide_Left].bits == prLayoutLengthPx(4).bits);
    CHECK(page.box("vrl").margin[PrRectSide_Right].bits == prLayoutLengthPx(5).bits);
    CHECK(page.box("sizes").height.bits == prLayoutLengthPx(40).bits);
    CHECK(verified(page.presentation));
}

TEST_CASE("a flow-relative declaration and its physical counterpart cascade together, the later winning") {
    Page page("<style>#sheet { margin-left: 5px } #sheet { margin-inline-start: 7px }"
        "#important { margin-inline-start: 7px !important }</style>"
        "<div id=logical style='margin-left: 5px; margin-inline-start: 7px'>x</div>"
        "<div id=physical style='margin-inline-start: 7px; margin-left: 5px'>x</div><div id=sheet>x</div>"
        "<div id=important style='margin-left: 5px'>x</div><div id=shorthand style='margin: 1px; margin-inline: 2px 3px'>x</div>"
        "<div id=custom style='--m: 4px; margin-inline: var(--m) 6px'>x</div>");
    CHECK(resolved(page, "logical", PrCssProperty_MarginLeft) == "7px");
    CHECK(resolved(page, "physical", PrCssProperty_MarginLeft) == "5px");
    CHECK(resolved(page, "sheet", PrCssProperty_MarginLeft) == "7px");
    CHECK(resolved(page, "important", PrCssProperty_MarginLeft) == "7px");
    CHECK(resolved(page, "shorthand", PrCssProperty_MarginLeft) == "2px");
    CHECK(resolved(page, "shorthand", PrCssProperty_MarginRight) == "3px");
    CHECK(resolved(page, "shorthand", PrCssProperty_MarginTop) == "1px");
    CHECK(resolved(page, "custom", PrCssProperty_MarginLeft) == "4px");
    CHECK(resolved(page, "custom", PrCssProperty_MarginRight) == "6px");
    CHECK(verified(page.presentation));
}

TEST_CASE("a flow-relative property inherits its parent's physical counterpart's value") {
    // csswg-drafts#3029: the child's inline-start is its left, so it inherits the parent's margin-left, though the
    // parent's own inline-start is its right.
    Page page("<div id=parent style='direction: rtl; margin-left: 11px; margin-right: 22px'>"
        "<div id=child style='direction: ltr; margin-inline-start: inherit'>x</div></div>");
    CHECK(resolved(page, "child", PrCssProperty_MarginLeft) == "11px");
    CHECK(resolved(page, "child", PrCssProperty_MarginRight) == "0px");
    CHECK(verified(page.presentation));
}

TEST_CASE("a border with no style keeps its computed width, which a child inherits, and its used width is 0") {
    Page page("<style>#p { border-width: 20px; border-style: none } #c { border-style: solid; border-width: inherit }"
        "#m { border-style: solid }</style><div id=p><div id=c></div></div><div id=m></div>");
    CHECK(prLayoutUnitToPx(page.box("p").border[PrRectSide_Top]) == 0.0f);
    CHECK(prLayoutUnitToPx(page.box("c").border[PrRectSide_Top]) == 20.0f);
    CHECK(resolved(page, "p", PrCssProperty_BorderTopWidth) == "0px");
    CHECK(resolved(page, "c", PrCssProperty_BorderTopWidth) == "20px");
    // A style with no width uses the initial medium width.
    CHECK(prLayoutUnitToPx(page.box("m").border[PrRectSide_Left]) == 3.0f);
    CHECK(resolved(page, "m", PrCssProperty_BorderLeftWidth) == "3px");
}

TEST_CASE("z-index computes to auto or an integer, clamped into 32 bits, and projects as a bit and its integer") {
    Page page("<div id=auto></div><div id=one style='z-index: 1'></div><div id=neg style='z-index: -7'></div>"
        "<div id=huge style='z-index: 99999999999'></div><div id=low style='z-index: -99999999999'></div>"
        "<div id=calc style='z-index: calc(5 / 2)'></div><div id=tie style='z-index: calc(-5 / 2)'></div>");
    CHECK(resolved(page, "auto", PrCssProperty_ZIndex) == "auto");
    CHECK(resolved(page, "one", PrCssProperty_ZIndex) == "1");
    CHECK(resolved(page, "neg", PrCssProperty_ZIndex) == "-7");
    CHECK(resolved(page, "huge", PrCssProperty_ZIndex) == "2147483647");
    CHECK(resolved(page, "low", PrCssProperty_ZIndex) == "-2147483648");
    // A calculation rounds to the nearest integer, with a tie rounding toward positive infinity.
    CHECK(resolved(page, "calc", PrCssProperty_ZIndex) == "3");
    CHECK(resolved(page, "tie", PrCssProperty_ZIndex) == "-2");
    CHECK(page.box("auto").zIndexed == 0);
    CHECK(page.box("one").zIndexed == 1);
    CHECK(page.effects("one").zIndex == 1);
    CHECK(page.effects("neg").zIndex == -7);
    CHECK(page.effects("huge").zIndex == INT32_MAX);
    CHECK(page.effects("low").zIndex == INT32_MIN);
}

TEST_CASE("border widths resolve snapped to whole target pixels, and one under a target pixel is one") {
    PrHtmlHost host = prHtmlHostDefault();
    Page page("");
    host.font = page.defaultFont;
    host.shaper = page.shaper;
    PrHtmlPage* htmlPage = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(htmlPage, "<style>div { border-style: solid }</style><div id=hair style='border-width:"
        " 0.1px'></div><div id=part style='border-width: 1.7px'></div><div id=m>"
        "</div><div id=o style='outline-offset: -0.1px'></div>", "about:blank", nullptr) == PrStatus_Ok);
    PrCssPresentation* presentation = prHtmlPagePresentation(htmlPage);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(htmlPage));
    auto width = [&](const char* id, PrCssProperty property) {
        PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(document), id);
        REQUIRE(element != nullptr);
        PrCssDeclarationName name = prCssDeclarationNameFromProperty(property);
        PrRawString read = prCssStyleDeclarationGetPropertyValue(
            prCssStylePropertiesAsCssStyleDeclaration(prCssElementComputedStyle(element, PrStringView{})), &name);
        std::string value(prRawStringView(&read).data, prRawStringView(&read).size);
        prRawStringReset(&read);
        return value;
    };
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(width("hair", PrCssProperty_BorderTopWidth) == "1px");
    CHECK(width("part", PrCssProperty_BorderTopWidth) == "1px");
    CHECK(width("m", PrCssProperty_BorderTopWidth) == "3px");
    // An outline's offset snaps as its width does, its sign kept (w3c/csswg-drafts#12906).
    CHECK(width("o", PrCssProperty_OutlineOffset) == "-1px");
    PrAppDisplay display = prAppDisplayDefault();
    display.pixelRatio = 2;
    prHtmlPageSetDisplay(htmlPage, &display);
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(width("hair", PrCssProperty_BorderTopWidth) == "0.5px");
    CHECK(width("part", PrCssProperty_BorderTopWidth) == "1.5px");
    CHECK(width("m", PrCssProperty_BorderTopWidth) == "3px");
    // The initial medium snaps at a target pixel ratio that is not whole.
    display.pixelRatio = 1.5f;
    prHtmlPageSetDisplay(htmlPage, &display);
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(width("m", PrCssProperty_BorderTopWidth) == "2.6666666666666665px");
    prHtmlPageDestroy(htmlPage);
}

TEST_CASE("color-hdr() resolves for the display's headroom, which projects again only the styles that read it") {
    Page page("");
    PrHtmlHost host = prHtmlHostDefault();
    host.font = page.defaultFont;
    host.shaper = page.shaper;
    PrHtmlPage* htmlPage = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(htmlPage, "<div id=hdr style='background: color-hdr(color(rec2100-linear 1 1 1) 0, "
        "color(rec2100-linear 4 4 4) 2)'></div><div id=sdr style='background: red'></div>", "about:blank", nullptr) ==
        PrStatus_Ok);
    PrCssPresentation* presentation = prHtmlPagePresentation(htmlPage);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(htmlPage));
    auto background = [&](const char* id) {
        PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(document), id);
        REQUIRE(element != nullptr);
        PrLayoutTree* tree = prCssPresentationTree(presentation);
        PrLayoutNode node = prCssPresentationLayoutNode(presentation, prDomElementAsNode(element));
        PrLayoutStyle record = prLayoutStyleRecord(tree, prLayoutNodeStyle(tree, node));
        PrLayoutDecorationStyle decoration =
            prLayoutDecorationStyleRecord(tree, record.groups[PrLayoutGroup_Decoration]);
        return prColorFromColor16(decoration.background);
    };
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    // SDR shows the first color, two stops of headroom the second, and one stop their mean in absolute luminance.
    CHECK(background("hdr").g == doctest::Approx(1.0f).epsilon(0.01));
    // Only the element whose record read the headroom is visited again.
    setHeadroom(htmlPage, 4);
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(prCssPresentationStats(presentation).elementsStyled == 1);
    float encodedFour = 1.055f * std::pow(4.0f, 1 / 2.4f) - 0.055f;
    CHECK(background("hdr").g == doctest::Approx(encodedFour).epsilon(0.01));
    setHeadroom(htmlPage, 2);
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    float encodedTwo = 1.055f * std::pow(2.0f, 1 / 2.4f) - 0.055f;
    CHECK(background("hdr").g == doctest::Approx(encodedTwo).epsilon(0.01));
    CHECK(sameColor(background("sdr"), PrColor{1, 0, 0, 1}));
    CHECK(verified(presentation));
    // A document that reads no headroom does no work when it changes.
    REQUIRE(prHtmlPageLoad(htmlPage, "<div style='background: red'></div>", "about:blank", nullptr) == PrStatus_Ok);
    presentation = prHtmlPagePresentation(htmlPage);
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    setHeadroom(htmlPage, 8);
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(prCssPresentationStats(presentation).elementsStyled == 0);
    prHtmlPageDestroy(htmlPage);
}

TEST_CASE("an inherited currentcolor and an animated color resolve for the display's headroom, as it changes") {
    Page page("");
    PrHtmlHost host = prHtmlHostDefault();
    host.font = page.defaultFont;
    host.shaper = page.shaper;
    PrHtmlPage* htmlPage = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(htmlPage, "<style>@keyframes glow { from, to { color: color-hdr(color(rec2100-linear 1 1 "
        "1) 0, color(rec2100-linear 4 4 4) 2) } }</style><div style='color: color-hdr(color(rec2100-linear 1 1 1) 0, "
        "color(rec2100-linear 4 4 4) 2)'><div id=inherits style='color: currentcolor'>"
        "</div></div><div id=animated style='animation: glow 100s'></div>", "about:blank", nullptr) == PrStatus_Ok);
    PrCssPresentation* presentation = prHtmlPagePresentation(htmlPage);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(htmlPage));
    auto color = [&](const char* id) {
        PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(document), id);
        REQUIRE(element != nullptr);
        PrLayoutTree* tree = prCssPresentationTree(presentation);
        PrLayoutNode node = prCssPresentationLayoutNode(presentation, prDomElementAsNode(element));
        PrLayoutStyle record = prLayoutStyleRecord(tree, prLayoutNodeStyle(tree, node));
        return prColorFromColor16(prLayoutColorStyleRecord(tree, record.groups[PrLayoutGroup_Color]).color);
    };
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(color("inherits").g == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(color("animated").g == doctest::Approx(1.0f).epsilon(0.01));
    setHeadroom(htmlPage, 4);
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    float encodedFour = 1.055f * std::pow(4.0f, 1 / 2.4f) - 0.055f;
    CHECK(color("inherits").g == doctest::Approx(encodedFour).epsilon(0.01));
    CHECK(color("animated").g == doctest::Approx(encodedFour).epsilon(0.01));
    CHECK(verified(presentation));
    prHtmlPageDestroy(htmlPage);
}

TEST_CASE("the color-gamut and dynamic-range media features describe the display, and a wider one matches narrower") {
    Page page("");
    PrHtmlHost host = prHtmlHostDefault();
    host.font = page.defaultFont;
    host.shaper = page.shaper;
    PrHtmlPage* htmlPage = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(htmlPage, "<style>div { width: 1px }@media (color-gamut: srgb) { #srgb { width: 2px } }"
        "@media (color-gamut: p3) { #p3 { width: 2px } }@media (color-gamut: rec2020) { #rec2020 { width: 2px } }"
        "@media (dynamic-range: standard) { #standard { width: 2px } }@media (dynamic-range: high) { #high { width: 2px } }</style>"
        "<div id=srgb></div><div id=p3></div><div id=rec2020></div>"
        "<div id=standard></div><div id=high></div>", "about:blank", nullptr) == PrStatus_Ok);
    PrCssPresentation* presentation = prHtmlPagePresentation(htmlPage);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(htmlPage));
    // Returns true if the query that the element's id names matches.
    auto matches = [&](const char* id) {
        PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(document), id);
        REQUIRE(element != nullptr);
        PrLayoutTree* tree = prCssPresentationTree(presentation);
        PrLayoutNode node = prCssPresentationLayoutNode(presentation, prDomElementAsNode(element));
        PrRect bounds = prLayoutRectToPx(prLayoutNodeBounds(tree, node, PrLayoutCoordinates_Viewport));
        return bounds.right - bounds.left == 2.0f;
    };
    auto show = [&](float headroom, PrColorSpace gamut) {
        PrAppDisplay display = prAppDisplayDefault();
        display.headroom = headroom;
        display.gamut = gamut;
        prHtmlPageSetDisplay(htmlPage, &display);
        prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    };
    show(1, PrColorSpace_Srgb);
    CHECK(matches("srgb"));
    CHECK_FALSE(matches("p3"));
    CHECK_FALSE(matches("rec2020"));
    CHECK(matches("standard"));
    CHECK_FALSE(matches("high"));
    show(4, PrColorSpace_DisplayP3);
    CHECK(matches("srgb"));
    CHECK(matches("p3"));
    CHECK_FALSE(matches("rec2020"));
    CHECK(matches("standard"));
    CHECK(matches("high"));
    show(4, PrColorSpace_Rec2020);
    CHECK(matches("rec2020"));
    CHECK(matches("p3"));
    // The same display again changes nothing.
    show(4, PrColorSpace_Rec2020);
    CHECK(prCssPresentationStats(presentation).elementsStyled == 0);
    CHECK(verified(presentation));
    prHtmlPageDestroy(htmlPage);
}

TEST_CASE("the used color scheme follows the user's preference where an element allows both, and the page's schemes") {
    Page page("");
    PrHtmlHost host = prHtmlHostDefault();
    host.font = page.defaultFont;
    host.shaper = page.shaper;
    PrHtmlPage* htmlPage = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(htmlPage, "<style>div { background: light-dark(rgb(255, 0, 0), rgb(0, 0, 255)) }</style>"
        "<div id=both style='color-scheme: light dark'></div><div id=light style='color-scheme: light'></div>"
        "<div id=dark style='color-scheme: dark'></div><div id=normal></div>"
        "<div id=unknown style='color-scheme: sepia'></div>", "about:blank", nullptr) == PrStatus_Ok);
    PrCssPresentation* presentation = prHtmlPagePresentation(htmlPage);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(htmlPage));
    // Returns true if the element's background resolved for a dark scheme.
    auto dark = [&](const char* id) {
        PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(document), id);
        REQUIRE(element != nullptr);
        PrLayoutTree* tree = prCssPresentationTree(presentation);
        PrLayoutNode node = prCssPresentationLayoutNode(presentation, prDomElementAsNode(element));
        PrLayoutStyle record = prLayoutStyleRecord(tree, prLayoutNodeStyle(tree, node));
        PrLayoutGroupIndex decoration = record.groups[PrLayoutGroup_Decoration];
        return prColorFromColor16(prLayoutDecorationStyleRecord(tree, decoration).background).b == 1.0f;
    };
    auto prefer = [&](PrAppColorScheme scheme) {
        PrAppUserPreferences preferences{.colorScheme = scheme};
        prHtmlPageSetUserPreferences(htmlPage, &preferences);
        prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    };
    prefer(PrAppColorScheme_Light);
    CHECK_FALSE(dark("both"));
    CHECK_FALSE(dark("light"));
    CHECK(dark("dark"));
    CHECK_FALSE(dark("normal"));
    CHECK(sameColor(prColorFromColor16(prLayoutTreeCanvasColor(prCssPresentationTree(presentation))), PrColor{1, 1, 1, 1}));
    // A dark preference reaches only the element that allows both schemes, which alone projects again.
    CHECK_FALSE(dark("unknown"));
    prefer(PrAppColorScheme_Dark);
    CHECK(prCssPresentationStats(presentation).elementsStyled == 1);
    CHECK(dark("both"));
    CHECK_FALSE(dark("light"));
    CHECK_FALSE(dark("normal"));
    CHECK(verified(presentation));

    // The page's supported schemes apply where color-scheme is normal, and the canvas follows the root's scheme.
    REQUIRE(prHtmlPageLoad(htmlPage, "<meta name=color-scheme content='light dark'>"
        "<style>div { background: light-dark(rgb(255, 0, 0), rgb(0, 0, 255)) }</style>"
        "<div id=normal></div><div id=light style='color-scheme: light'></div>"
        "<div id=unknown style='color-scheme: sepia'></div>", "about:blank", nullptr) == PrStatus_Ok);
    presentation = prHtmlPagePresentation(htmlPage);
    document = prHtmlWindowDocument(prHtmlPageWindow(htmlPage));
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(dark("normal"));
    CHECK_FALSE(dark("light"));
    // A scheme that the engine does not know gives the default, light, not the page's schemes.
    CHECK_FALSE(dark("unknown"));
    PrColor canvas = prColorFromColor16(prLayoutTreeCanvasColor(prCssPresentationTree(presentation)));
    CHECK(canvas.r < 0.2f);
    CHECK(canvas.a == 1.0f);
    prefer(PrAppColorScheme_Light);
    CHECK_FALSE(dark("normal"));
    CHECK(sameColor(prColorFromColor16(prLayoutTreeCanvasColor(prCssPresentationTree(presentation))), PrColor{1, 1, 1, 1}));
    // A meta that changes its schemes reaches every box.
    PrDomElement* meta = prDomNodeFirstElementChild(prDomElementAsNode(prDomDocumentHead(document)));
    REQUIRE(meta != nullptr);
    prDomElementSetAttribute(meta, "content", "dark");
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(dark("normal"));
    CHECK(verified(presentation));
    // A content that is not a valid color-scheme value is passed over for the next meta, which may be in the body.
    prDomElementSetAttribute(meta, "content", "light,dark");
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK_FALSE(dark("normal"));
    PrDomElement* later = nullptr;
    REQUIRE(prDomElementCreate(document, "meta", nullptr, &later) == PrStatus_Ok);
    prDomElementSetAttribute(later, "name", "color-scheme");
    prDomElementSetAttribute(later, "content", "dark");
    REQUIRE(prDomNodeAppendChild(prDomElementAsNode(prDomDocumentBody(document)), prDomElementAsNode(later)) ==
        PrStatus_Ok);
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(dark("normal"));
    CHECK(verified(presentation));
    prDomNodeRemove(prDomElementAsNode(later));
    prDomNodeRelease(prDomElementAsNode(later));
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK_FALSE(dark("normal"));
    prHtmlPageDestroy(htmlPage);
}

TEST_CASE("a frame's document sees its parent's display and user preferences, before and after they change") {
    Page page("");
    PrHtmlHost host = prHtmlHostDefault();
    host.font = page.defaultFont;
    host.shaper = page.shaper;
    PrHtmlPage* htmlPage = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(htmlPage, "<iframe id=early srcdoc='x'></iframe>", "about:blank", nullptr) == PrStatus_Ok);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(htmlPage));
    // Returns true if the query matches in the window of the iframe whose id is `id`.
    auto matches = [&](const char* id, const char* query) {
        PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(document), id);
        REQUIRE(element != nullptr);
        PrHtmlWindow* window = prHtmlIFrameElementContentWindow(
            prHtmlIFrameElementQuery(prDomNodeAsEventTarget(prDomElementAsNode(element))));
        REQUIRE(window != nullptr);
        PrCssMediaQueryList* list = prHtmlWindowMatchMedia(window, query);
        bool matched = prCssMediaQueryListMatches(list);
        prCssMediaQueryListRelease(list);
        return matched;
    };
    CHECK(matches("early", "(resolution: 1dppx)"));
    CHECK(matches("early", "(dynamic-range: standard) and (not (dynamic-range: high))"));

    PrAppDisplay display{.pixelRatio = 2, .headroom = 4, .potentialHeadroom = 8, .gamut = PrColorSpace_DisplayP3};
    prHtmlPageSetDisplay(htmlPage, &display);
    PrAppUserPreferences preferences{
        .colorScheme = PrAppColorScheme_Dark,
        .contrast = PrAppContrast_More,
        .reducedMotion = true,
        .reducedTransparency = true,
    };
    prHtmlPageSetUserPreferences(htmlPage, &preferences);
    const char* const shared[] = {
        "(resolution: 2dppx)", "(dynamic-range: high)", "(color-gamut: p3)",
        "(prefers-contrast: more)", "(prefers-reduced-motion: reduce)",
        "(prefers-reduced-transparency: reduce)",
    };
    for (const char* query : shared) {
        CAPTURE(query);
        CHECK(matches("early", query));
    }
    // A frame keeps the color scheme that its iframe element gives it, which a view carries over from the iframe.
    CHECK(matches("early", "(prefers-color-scheme: light)"));

    // A frame attached after the change sees the same.
    PrDomElement* late = nullptr;
    REQUIRE(prDomElementCreate(document, "iframe", nullptr, &late) == PrStatus_Ok);
    prDomElementSetAttribute(late, "id", "late");
    prDomElementSetAttribute(late, "srcdoc", "x");
    prDomNodeAppendChild(prDomElementAsNode(prDomDocumentBody(document)), prDomElementAsNode(late));
    for (const char* query : shared) {
        CAPTURE(query);
        CHECK(matches("late", query));
    }
    prDomNodeRelease(prDomElementAsNode(late));
    prHtmlPageDestroy(htmlPage);
}

TEST_CASE("dynamic-range-limit caps an element's colors, and a mix follows the display's headroom") {
    Page page("");
    PrHtmlHost host = prHtmlHostDefault();
    host.font = page.defaultFont;
    host.shaper = page.shaper;
    PrHtmlPage* htmlPage = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(htmlPage, "<style>div { background: color(rec2100-linear 4 4 4) }</style>"
        "<div id=standard style='dynamic-range-limit: standard'></div>"
        "<div id=constrained style='dynamic-range-limit: constrained'></div><div id=open></div>"
        "<div id=mix style='dynamic-range-limit: dynamic-range-limit-mix(standard 50%,"
        " no-limit 50%)'></div>", "about:blank", nullptr) == PrStatus_Ok);
    PrCssPresentation* presentation = prHtmlPagePresentation(htmlPage);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(htmlPage));
    auto decoration = [&](const char* id) {
        PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(document), id);
        REQUIRE(element != nullptr);
        PrLayoutTree* tree = prCssPresentationTree(presentation);
        PrLayoutNode node = prCssPresentationLayoutNode(presentation, prDomElementAsNode(element));
        PrLayoutStyle record = prLayoutStyleRecord(tree, prLayoutNodeStyle(tree, node));
        return prLayoutDecorationStyleRecord(tree, record.groups[PrLayoutGroup_Decoration]);
    };
    auto encoded = [](float linear) { return 1.055f * std::pow(linear, 1 / 2.4f) - 0.055f; };
    setHeadroom(htmlPage, 4);
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(prColorFromColor16(decoration("standard").background).g == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(prColorFromColor16(decoration("constrained").background).g == doctest::Approx(encoded(2)).epsilon(0.01));
    CHECK(prColorFromColor16(decoration("open").background).g == doctest::Approx(encoded(4)).epsilon(0.01));
    // Half of no-limit is half the display's two stops.
    CHECK(prColorFromColor16(decoration("mix").background).g == doctest::Approx(encoded(2)).epsilon(0.01));
    setHeadroom(htmlPage, 16);
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(prColorFromColor16(decoration("mix").background).g == doctest::Approx(encoded(4)).epsilon(0.01));
    CHECK(verified(presentation));
    prHtmlPageDestroy(htmlPage);
}

TEST_CASE("a br breaks its line whatever its own white space, and what it holds has no boxes") {
    Page page("<style>div { width: 400px; line-height: 20px }</style><div id=d>a<br id=b style='white-space: normal'>b"
        "</div><pre id=p style='margin: 0; line-height: 20px'>a\n  b</pre>");
    CHECK(double(page.bounds("d").bottom - page.bounds("d").top) == doctest::Approx(40.0));
    CHECK(double(page.bounds("p").bottom - page.bounds("p").top) == doctest::Approx(40.0));
    CHECK(resolved(page, "p", PrCssProperty_WhiteSpaceCollapse) == "preserve");
    CHECK(resolved(page, "p", PrCssProperty_TextWrapMode) == "nowrap");
    // A script may give a br children, which render nothing: its box is a line break.
    PrDomElement* br = byId(page, "b");
    prDomNodeAppendChild(prDomElementAsNode(br), prDomTextAsNode(prDomTextCreate(page.document, "x")));
    update(page);
    CHECK(double(page.bounds("d").bottom - page.bounds("d").top) == doctest::Approx(40.0));
    CHECK(prLayoutNodeKind(prCssPresentationTree(page.presentation), page.node("b")) == PrLayoutNodeKind_LineBreak);
    CHECK(prLayoutNodeFirstChild(prCssPresentationTree(page.presentation), page.node("b")) == kPrLayoutNodeNone);

    // display: contents removes a br's box entirely, in the same way as none, including its line feed
    // (css-display-3/#unbox-html).
    Page contents("<div id=d style='width: 400px; line-height: 20px'>a<br id=b style='display: contents'>b</div>");
    CHECK(contents.node("b") == kPrLayoutNodeNone);
    CHECK(double(contents.bounds("d").bottom - contents.bounds("d").top) == doctest::Approx(20.0));
}

TEST_CASE("a br's own font and alignment take no part in its line, but its clear does") {
    // A br of a larger font, or of taller lines, leaves its line as tall as the text around it. Alone in a block, the
    // br gives the block the strut's height.
    Page page("<style>body { margin: 0; font: 5px/1 Arimo } div { width: 200px }</style>"
        "<div id=fs>XXX<br style='font-size: 25px'>XX</div><div id=lh>XXX<br style='line-height: 5'>XX</div>"
        "<div id=only><br style='font-size: 40px'></div>"
        "<div id=va style='font: 20px/40px Arimo'>a<br style='vertical-align: 30px'>b</div>"
        "<div id=span>XXX<span style='font-size: 25px'><br></span>XX</div>");
    auto height = [&](const char* id) { return double(page.bounds(id).bottom - page.bounds(id).top); };
    CHECK(height("fs") == doctest::Approx(10.0));
    CHECK(height("lh") == doctest::Approx(10.0));
    CHECK(height("only") == doctest::Approx(5.0));
    CHECK(height("va") == doctest::Approx(80.0));
    // A span around the br is an inline box of its own, which does make the line taller.
    CHECK(height("span") == doctest::Approx(30.0));

    // Clearing puts the line after it below the float, and at the end of the content its block ends there too.
    Page clears("<style>body { margin: 0; font: 20px/40px Arimo } .f { float: left; width: 10px; height: 60px }"
        "</style><div id=c><span class=f></span>a<br style='clear: left'>b</div>"
        "<div id=e><span class=f></span>a<br style='clear: left'></div>");
    CHECK(double(clears.bounds("c").bottom - clears.bounds("c").top) == doctest::Approx(100.0));
    CHECK(double(clears.bounds("e").bottom - clears.bounds("e").top) == doctest::Approx(60.0));
    CHECK(verified(clears.presentation));
}

TEST_CASE("the root's display is blockified, and its block box fills the viewport") {
    struct Case {
        const char* display;
        const char* computed;
        const char* width;
    };
    // A table shrinks to fit its content even as the root (css-tables-3/#layout-principles). Its width is the body's
    // margins around an x.
    for (Case c : {Case{"inline", "block", "800px"}, Case{"inline-block", "block", "800px"},
        Case{"inline flow-root", "block", "800px"}, Case{"inline-table", "table", "24px"},
                Case{"inline list-item", "list-item", "800px"}, Case{"initial", "block", "800px"},
                Case{"inherit", "block", "800px"}}) {
        CAPTURE(c.display);
        std::string html = std::string("<html id=r style='display: ") + c.display + "'><body>x</body></html>";
        Page page(html.c_str());
        CHECK(resolved(page, "r", PrCssProperty_Display) == c.computed);
        CHECK(!prLayoutDisplayIsInlineLevel(page.box("r").display));
        CHECK(resolved(page, "r", PrCssProperty_Width) == c.width);
        CHECK(verified(page.presentation));
    }
}

TEST_CASE("float and clear reach layout, and a floated element's display is blockified") {
    Page page("<style>body { margin: 0; width: 200px } .f { width: 60px; height: 20px }</style>"
        "<span id=l class=f style='float: left'></span><div id=r class=f style='float: right'></div>"
        "<div id=s class=f style='float: inline-start'></div><div id=e class=f style='float: inline-end'></div>"
        "<div id=a class=f style='float: left; position: absolute'></div>"
        "<div id=c style='clear: both; height: 5px'></div><div id=cs style='clear: inline-start'></div>"
        "<span id=i style='float: left; display: inline-block'></span>"
        "<span id=n style='float: inherit'><span id=inherit style='float: inherit; display: inherit'></span></span>");
    CHECK(page.box("l").floating == PrLayoutFloat_Left);
    CHECK(page.box("r").floating == PrLayoutFloat_Right);
    CHECK(page.box("s").floating == PrLayoutFloat_InlineStart);
    CHECK(page.box("e").floating == PrLayoutFloat_InlineEnd);
    CHECK(page.box("a").floating == PrLayoutFloat_None);
    CHECK(page.box("c").clear == PrLayoutClear_Both);
    CHECK(page.box("cs").clear == PrLayoutClear_InlineStart);
    CHECK(resolved(page, "l", PrCssProperty_Display) == "block");
    CHECK(resolved(page, "i", PrCssProperty_Display) == "block");
    CHECK(resolved(page, "l", PrCssProperty_Float) == "left");
    CHECK(resolved(page, "n", PrCssProperty_Display) == "inline");
    CHECK(page.box("l").display == PrLayoutDisplay_Block);
    // The left float is at the left, the right one at the right, and the cleared block below both rows of floats.
    CHECK(page.bounds("l").left == 0.0f);
    CHECK(page.bounds("r").right == 200.0f);
    CHECK(page.bounds("c").top >= 20.0f);
    CHECK(resolved(page, "l", PrCssProperty_Width) == "60px");
    CHECK(verified(page.presentation));
}

TEST_CASE("a sticky element sticks as its scroll container scrolls, where it is measured, hit and scrolled to") {
    Page page("<style>body { margin: 0 } #s { overflow: auto; height: 100px } "
        "#h { position: sticky; top: 10px; height: 20px }</style>"
        "<div id=s><div style='height: 50px'></div><div id=h></div><div style='height: 500px'></div></div>");
    PrHtmlElement* header = prHtmlElementQuery(prDomNodeAsEventTarget(prDomElementAsNode(byId(page, "h"))));
    CHECK(prCssElementGetBoundingClientRect(byId(page, "h")).y == 50.0);
    prCssElementSetScrollTop(byId(page, "s"), 100);
    // It stays 10px below the scrollport's top, and offsetTop measures it there, in its scroll container's content.
    CHECK(prCssElementGetBoundingClientRect(byId(page, "h")).y == 10.0);
    CHECK(prHtmlElementOffsetTop(header) == 110);
    CHECK(prCssDocumentElementFromPoint(page.document, 5, 15) == byId(page, "h"));
    // Scrolled into view, its place in flow comes to the top.
    PrCssScrollIntoViewOptions options = prCssScrollIntoViewOptionsDefault();
    prCssElementScrollIntoView(byId(page, "h"), &options);
    CHECK(prCssElementScrollTop(byId(page, "s")) == 50.0);
}

TEST_CASE("absolute and fixed positions reach layout, blockified, their float none, keeping whether they were inline") {
    Page page("<style>body { margin: 0 } .a { position: absolute }</style>"
        "<span id=a class=a>a</span><span id=ib class=a style='display: inline-block'>b</span>"
        "<div id=d class=a></div><div id=x style='position: fixed'></div><div id=f class=a style='float: left'>"
        "</div><div id=s style='position: sticky'></div><div id=r style='position: relative'></div>"
        "<span id=t style='transform: translateX(0)'>t</span><div id=n></div>");
    CHECK(page.box("a").position == PrLayoutPosition_Absolute);
    CHECK(page.box("x").position == PrLayoutPosition_Fixed);
    CHECK(page.box("s").position == PrLayoutPosition_Sticky);
    CHECK(page.box("r").position == PrLayoutPosition_Relative);
    CHECK(page.box("n").position == PrLayoutPosition_Static);
    CHECK(resolved(page, "a", PrCssProperty_Display) == "block");
    CHECK(resolved(page, "ib", PrCssProperty_Display) == "block");
    CHECK(resolved(page, "f", PrCssProperty_Float) == "none");
    CHECK(page.box("a").display == PrLayoutDisplay_Block);
    CHECK(page.box("a").staticInline == 1);
    CHECK(page.box("ib").staticInline == 1);
    CHECK(page.box("d").staticInline == 0);
    CHECK(page.box("x").staticInline == 0);
    // Layout records whether a box has a transform, whether or not it maps to the identity. Compositing receives its
    // matrix.
    CHECK(page.box("t").transformed == 1);
    CHECK(page.box("n").transformed == 0);
    // An absolute span turned from inline to block moves its static position to the next line, absolute throughout.
    prDomElementSetAttribute(byId(page, "a"), "style", "display: block");
    update(page);
    CHECK(page.box("a").staticInline == 0);
    CHECK(verified(page.presentation));
}

TEST_CASE("flex containers and the flex longhands reach layout, and flex items are blockified") {
    Page page("<style>body { margin: 0; font: 20px/20px Arimo } span { width: 50px }</style>"
        "<div id=c style='display: flex; flex-direction: column-reverse; flex-wrap: wrap; row-gap: 5px; "
        "justify-content: safe center; align-items: last baseline; align-content: space-evenly'>"
        "<span id=i style='flex: 2 0 10px; order: 3; align-self: flex-end'>a</span><span id=j>b</span>"
        "<div style='display: contents'><span id=k style='display: inline-block'>c</span></div></div>"
        "<span id=n style='display: inline-flex; flex-basis: content'><b id=b>d</b></span>");
    CHECK(page.box("c").display == PrLayoutDisplay_Flex);
    CHECK(page.box("n").display == PrLayoutDisplay_InlineFlex);
    PrLayoutFlexStyle c = page.flex("c");
    CHECK(c.direction == PrLayoutFlexDirection_ColumnReverse);
    CHECK(c.wrap == PrLayoutFlexWrap_Wrap);
    CHECK(c.rowGap.bits == prLayoutLengthPx(5).bits);
    CHECK(c.columnGap.bits == prLayoutLengthNormal().bits);
    CHECK(c.justifyContent == (PrLayoutAlignment_Center | PrLayoutAlignmentFlag_Safe));
    CHECK(c.alignItems == PrLayoutAlignment_LastBaseline);
    CHECK(c.alignContent == PrLayoutAlignment_SpaceEvenly);
    PrLayoutFlexStyle i = page.flex("i");
    CHECK(i.grow == 2.0f);
    CHECK(i.shrink == 0.0f);
    CHECK(i.basis.bits == prLayoutLengthPx(10).bits);
    CHECK(i.order == 3);
    CHECK(i.alignSelf == PrLayoutAlignment_FlexEnd);
    CHECK(page.flex("j").alignSelf == PrLayoutAlignment_Auto);
    CHECK(page.flex("n").basis.bits == prLayoutLengthContent().bits);
    // Each item's display is blockified, through an element of display contents too, and nothing else's.
    CHECK(resolved(page, "i", PrCssProperty_Display) == "block");
    CHECK(resolved(page, "j", PrCssProperty_Display) == "block");
    CHECK(resolved(page, "k", PrCssProperty_Display) == "block");
    CHECK(resolved(page, "b", PrCssProperty_Display) == "block");
    CHECK(resolved(page, "n", PrCssProperty_Display) == "inline-flex");
    CHECK(page.box("j").display == PrLayoutDisplay_Block);
    CHECK(verified(page.presentation));

    // With the container turned into a block, its children are inline again; and then back.
    prDomElementSetAttribute(byId(page, "c"), "style", "display: block");
    update(page);
    CHECK(resolved(page, "j", PrCssProperty_Display) == "inline");
    CHECK(resolved(page, "k", PrCssProperty_Display) == "inline-block");
    CHECK(page.box("j").display == PrLayoutDisplay_Inline);
    CHECK(verified(page.presentation));
    prDomElementSetAttribute(byId(page, "c"), "style", "display: flex");
    update(page);
    CHECK(resolved(page, "j", PrCssProperty_Display) == "block");
    CHECK(resolved(page, "k", PrCssProperty_Display) == "block");
    CHECK(verified(page.presentation));
}

TEST_CASE("grid containers and the grid longhands reach layout, and the track lists resolve to their used sizes") {
    Page page("<style>body { margin: 0; font: 20px/20px Arimo }</style>"
        "<div id=c style='display: grid; width: 300px; grid-template-columns: [a] 100px repeat(2, [b] 1fr); "
        "grid-auto-rows: 30px; grid-auto-flow: column dense; column-gap: 10px'>"
        "<i id=i style='grid-column: b 2 / span 1; grid-row: 2'></i><i id=j></i></div>"
        "<span id=n style='display: inline-grid'></span>");
    CHECK(page.box("c").display == PrLayoutDisplay_Grid);
    CHECK(page.box("n").display == PrLayoutDisplay_InlineGrid);
    PrLayoutGridStyle c = page.grid("c");
    CHECK(c.autoFlow == (PrLayoutGridAutoFlowFlag_Column | PrLayoutGridAutoFlowFlag_Dense));
    CHECK(c.templateColumns != kPrLayoutGridTracksNone);
    PrLayoutGridStyle i = page.grid("i");
    CHECK(i.columnStart.kind == PrLayoutGridLineKind_Line);
    CHECK(i.columnStart.integer == 2);
    CHECK(i.columnEnd.kind == PrLayoutGridLineKind_Span);
    CHECK(i.rowStart.integer == 2);
    // The repetition by a count is expanded, with its names merged at the lines between repetitions. The rows are
    // implicit, two of them, at their auto size.
    CHECK(resolved(page, "c", PrCssProperty_GridTemplateColumns) == "[a] 100px [b] 90px [b] 90px");
    CHECK(resolved(page, "c", PrCssProperty_GridTemplateRows) == "30px 30px");
    // An element that is not a grid container resolves to its computed value.
    CHECK(resolved(page, "i", PrCssProperty_GridTemplateColumns) == "none");
    CHECK(resolved(page, "n", PrCssProperty_GridTemplateRows) == "none");
    CHECK(verified(page.presentation));
}

TEST_CASE("grid-template-areas folds its strings, and strings that form no template are invalid") {
    Page page("<div id=a style='grid-template-areas: \"a   a ...\" \"b b  .\"'></div>"
        "<div id=b style='grid-template-areas: \"a b\" \"b a\"'></div><div id=c style='grid-template-areas: \"a\" \"b c\"'></div>"
        "<div id=d style='grid-template-areas: \"a !\"'></div>");
    CHECK(resolved(page, "a", PrCssProperty_GridTemplateAreas) == "\"a a .\" \"b b .\"");
    CHECK(resolved(page, "b", PrCssProperty_GridTemplateAreas) == "none");
    CHECK(resolved(page, "c", PrCssProperty_GridTemplateAreas) == "none");
    CHECK(resolved(page, "d", PrCssProperty_GridTemplateAreas) == "none");
}

TEST_CASE("percentage margins and padding resolve to their used lengths") {
    Page page("<style>body { margin: 0 }</style>"
        "<div style='width: 200px'><div id=a style='padding-left: 10%; margin-top: 25%; margin-left: auto'>"
        "</div></div><span id=s style='padding-left: 10%'></span>");
    CHECK(resolved(page, "a", PrCssProperty_PaddingLeft) == "20px");
    CHECK(resolved(page, "a", PrCssProperty_MarginTop) == "50px");
    CHECK(resolved(page, "a", PrCssProperty_MarginLeft) == "auto");
    // An inline box has no single box whose containing block the percentage resolved against.
    CHECK(resolved(page, "s", PrCssProperty_PaddingLeft) == "10%");
}

TEST_CASE("an element of display contents gives its parent's box its children's boxes") {
    Page page("<style>body { margin: 0 } i { width: 10px; height: 10px } .w { width: 20px }</style>"
        "<div id=c style='display: flex'><i id=a></i><div id=w style='display: contents'><i id=b class=w></i>"
        "t<span id=s>x</span><i id=d></i></div><i id=e></i></div>");
    PrLayoutTree* tree = prCssPresentationTree(page.presentation);
    auto count = [&](PrLayoutNode box) {
        uint32_t n = 0;
        for (PrLayoutNode c = prLayoutNodeFirstChild(tree, box); c != kPrLayoutNodeNone;
                c = prLayoutNodeNextSibling(tree, c))
            ++n;
        return n;
    };
    // The wrapper has no box, and its children are the container's items, in order between A and E. Its text is an
    // anonymous item of its own.
    CHECK(page.node("w") == kPrLayoutNodeNone);
    CHECK(count(page.node("c")) == 6);
    CHECK(prLayoutNodeParent(tree, page.node("b")) == page.node("c"));
    CHECK(page.bounds("a").left == 0.0f);
    CHECK(page.bounds("b").left == 10.0f);
    CHECK(page.bounds("s").left > 30.0f);
    CHECK(page.bounds("d").left == page.bounds("s").right);
    CHECK(page.bounds("e").left == page.bounds("d").right);
    CHECK(verified(page.presentation));

    // A box of its own holds its children as one item; display: contents again gives them back.
    prDomElementSetAttribute(byId(page, "w"), "style", "display: block");
    update(page);
    CHECK(page.node("w") != kPrLayoutNodeNone);
    CHECK(count(page.node("c")) == 3);
    CHECK(prLayoutNodeParent(tree, page.node("b")) == page.node("w"));
    CHECK(page.bounds("w").left == 10.0f);
    CHECK(verified(page.presentation));
    prDomElementSetAttribute(byId(page, "w"), "style", "display: contents");
    update(page);
    CHECK(page.node("w") == kPrLayoutNodeNone);
    CHECK(count(page.node("c")) == 6);
    CHECK(verified(page.presentation));

    // A child added to the wrapper is an item of the container.
    PrDomElement* added = nullptr;
    REQUIRE(prDomElementCreate(page.document, "i", nullptr, &added) == PrStatus_Ok);
    prDomElementSetAttribute(added, "id", "f");
    prDomNodeAppendChild(prDomElementAsNode(byId(page, "w")), prDomElementAsNode(added));
    update(page);
    CHECK(count(page.node("c")) == 7);
    CHECK(page.bounds("e").left == page.bounds("f").right);
    CHECK(verified(page.presentation));

    // The wrapper leaves, and its children's boxes with it.
    prDomNodeRemove(prDomElementAsNode(byId(page, "w")));
    update(page);
    CHECK(count(page.node("c")) == 2);
    CHECK(page.bounds("e").left == 10.0f);
    CHECK(verified(page.presentation));
}

TEST_CASE("text in an element of display contents inherits from it") {
    Page page("<style>body { margin: 0; font: 20px/20px Arimo } .w { display: contents; white-space: pre; "
        "font-size: 40px; line-height: 40px }</style><div id=c><div id=w class=w>a\nb</div></div>");
    // The text is the container's, set in the wrapper's record: two lines of 40px.
    CHECK(page.node("w") == kPrLayoutNodeNone);
    CHECK(double(page.bounds("c").bottom) == doctest::Approx(80.0));
    CHECK(verified(page.presentation));
    // The wrapper's record changes, and its text's with it: 30px lines of 40px text on the container's 20px strut reach
    // 32px. A box of its own takes its text, on its own strut.
    prDomElementSetAttribute(byId(page, "w"), "style", "line-height: 30px");
    update(page);
    CHECK(double(page.bounds("c").bottom) == doctest::Approx(64.0));
    CHECK(verified(page.presentation));
    prDomElementSetAttribute(byId(page, "w"), "style", "display: block; line-height: 30px");
    update(page);
    CHECK(page.node("w") != kPrLayoutNodeNone);
    CHECK(double(page.bounds("c").bottom) == doctest::Approx(60.0));
    CHECK(verified(page.presentation));
}

TEST_CASE("offsetParent is the nearest containing block of a box out of flow's kind, or of absolute boxes") {
    Page page("<style>body { margin: 0 } .a { position: absolute }</style>"
        "<div id=ops style='position: relative; padding-top: 5px'><div style='margin-top: 10px'>"
        "<i id=opa class=a style='top: 20px'></i></div></div>"
        "<div id=tr style='transform: translateX(0)'><div id=rel style='position: relative; margin-top: 10px'>"
        "<i id=held style='position: fixed; top: 20px'></i></div></div><i id=free style='position: fixed'></i>"
        "<div>x<span id=opis style='position: relative; padding-left: 10px'>yy<i id=opi class=a style='left: 0'>"
        "</i></span></div>");
    auto html = [&](const char* id) {
        return prHtmlElementQuery(prDomNodeAsEventTarget(prDomElementAsNode(byId(page, id))));
    };
    CHECK(prHtmlElementOffsetParent(html("opa")) == byId(page, "ops"));
    CHECK(prHtmlElementOffsetTop(html("opa")) == 20);
    // A fixed box's offset parent is the nearest containing block of fixed boxes, past the positioned boxes in between.
    CHECK(prHtmlElementOffsetParent(html("held")) == byId(page, "tr"));
    CHECK(prHtmlElementOffsetTop(html("held")) == 20);
    CHECK(prHtmlElementOffsetParent(html("free")) == nullptr);
    CHECK(prHtmlElementOffsetParent(html("opi")) == byId(page, "opis"));
    CHECK(prHtmlElementOffsetLeft(html("opi")) == 0);
    CHECK(verified(page.presentation));
}

TEST_CASE("a float that inherits its display is restyled when its parent's display alone changes") {
    // The float is explicit, so that only the display's inheritance makes the child depend on its parent.
    Page page("<div id=p style='display: block'><div id=c style='float: left; display: inherit'></div></div>");
    CHECK(resolved(page, "c", PrCssProperty_Display) == "block");
    prDomElementSetAttribute(byId(page, "p"), "style", "display: flow-root");
    update(page);
    CHECK(resolved(page, "c", PrCssProperty_Display) == "flow-root");
    prDomElementSetAttribute(byId(page, "p"), "style", "display: inline-block");
    update(page);
    CHECK(resolved(page, "c", PrCssProperty_Display) == "block");
    CHECK(verified(page.presentation));
}

TEST_CASE("the overflow pair computes together: visible and clip beside a scrolling value become auto and hidden") {
    struct Case {
        const char* x;
        const char* y;
        const char* computedX;
        const char* computedY;
    };
    for (Case c : {Case{"visible", "hidden", "auto", "hidden"}, Case{"clip", "scroll", "hidden", "scroll"},
        Case{"auto", "visible", "auto", "auto"}, Case{"clip", "visible", "clip", "visible"},
                Case{"visible", "clip", "visible", "clip"}, Case{"hidden", "clip", "hidden", "hidden"},
                Case{"inherit", "visible", "scroll", "auto"}}) {
        CAPTURE(c.x);
        CAPTURE(c.y);
        std::string html = std::string("<div style='overflow-x: scroll; overflow-y: hidden'><div id=e style='overflow-x: ") +
            c.x + "; overflow-y: " + c.y + "'></div></div>";
        Page page(html.c_str());
        CHECK(resolved(page, "e", PrCssProperty_OverflowX) == c.computedX);
        CHECK(resolved(page, "e", PrCssProperty_OverflowY) == c.computedY);
        CHECK(verified(page.presentation));
    }
}

TEST_CASE("a scroll container with visible in one axis clips both, and is hit in neither past its edge") {
    for (const char* axis : {"overflow-y", "overflow-x"}) {
        CAPTURE(axis);
        std::string html = std::string("<style>body { margin: 0 }</style><div id=p style='width: 20px; height: 20px; ") +
            axis + ": hidden'><div id=c style='width: 40px; height: 40px'></div></div>";
        Page page(html.c_str());
        PrLayoutTree* tree = prCssPresentationTree(page.presentation);
        PrLayoutNode hit = kPrLayoutNodeNone;
        prLayoutTreeHitTest(tree, prLayoutVecFromPx(PrVec2{30, 10}), &hit, 1);
        CHECK(hit != page.node("c"));
        prLayoutTreeHitTest(tree, prLayoutVecFromPx(PrVec2{10, 30}), &hit, 1);
        CHECK(hit != page.node("c"));
        prLayoutTreeHitTest(tree, prLayoutVecFromPx(PrVec2{10, 10}), &hit, 1);
        CHECK(hit == page.node("c"));
        CHECK(verified(page.presentation));
    }
}

TEST_CASE("overflow clip clips without making a scroll container or a formatting context") {
    Page page("<style>body { margin: 0 } p { margin: 20px 0 }</style>"
        "<div id=clip style='overflow: clip; height: 50px'><p id=p>x</p><div style='height: 200px'></div></div>"
        "<div id=hidden style='overflow: hidden; height: 50px'><p id=q>x</p></div>");
    CHECK(page.box("clip").overflowY == PrLayoutOverflow_Clip);
    // Its child's margin collapses through it, as through a block whose overflow is visible.
    CHECK(page.bounds("clip").top == 20.0f);
    CHECK(page.bounds("p").top == 20.0f);
    PrLayoutScrollInfo info{};
    REQUIRE(prLayoutNodeScrollInfo(prCssPresentationTree(page.presentation), page.node("clip"), &info));
    CHECK(!info.scrollContainer);
    REQUIRE(prLayoutNodeScrollInfo(prCssPresentationTree(page.presentation), page.node("hidden"), &info));
    CHECK(info.scrollContainer);
    CHECK(verified(page.presentation));
}

TEST_CASE("an update restyles what changed, and keeps the rest") {
    Page page("<style>.x { color: red } .wide { width: 50px } .gone { display: none }</style>"
        "<div id=outer><div id=a>a</div><div id=b>b</div><p id=c>c</p></div>");
    CHECK(verified(page.presentation));

    // Nothing changed.
    CHECK(update(page).elementsStyled == 0);

    // A class on a leaf restyles the leaf alone.
    prDomElementSetAttribute(byId(page, "a"), "class", "x");
    CHECK(update(page).elementsStyled == 1);
    CHECK(prLayoutTreeStats(prCssPresentationTree(page.presentation)).blocksLaidOut == 0);

    // An attribute that no selector reads restyles nothing.
    prDomElementSetAttribute(byId(page, "a"), "data-anything", "1");
    CHECK(update(page).elementsStyled == 0);

    // An inherited value restyles the children that take it; a value that does not inherit restyles none.
    prDomElementSetAttribute(byId(page, "outer"), "style", "color: blue");
    CHECK(update(page).elementsStyled == 4);
    prDomElementSetAttribute(byId(page, "outer"), "style", "color: blue; background-color: green");
    CHECK(update(page).elementsStyled == 1);

    // A size lays out the box and its ancestors, and keeps its siblings.
    prDomElementSetAttribute(byId(page, "b"), "class", "wide");
    PrCssPresentationStats stats = update(page);
    CHECK(stats.elementsStyled == 1);
    CHECK(prLayoutTreeStats(prCssPresentationTree(page.presentation)).blocksReused == 2);

    // Text changes no style.
    prDomNodeSetTextContent(prDomElementAsNode(byId(page, "c")), "more text");
    CHECK(update(page).elementsStyled == 0);

    // A new element is styled and given a box; a removed one's box is erased.
    PrDomElement* added = nullptr;
    REQUIRE(prDomElementCreate(page.document, "div", nullptr, &added) == PrStatus_Ok);
    prDomNodeAppendChild(prDomElementAsNode(byId(page, "outer")), prDomElementAsNode(added));
    stats = update(page);
    CHECK(stats.elementsStyled == 1);
    CHECK(stats.boxesCreated == 1);
    prDomNodeRemoveChild(prDomElementAsNode(byId(page, "outer")), prDomElementAsNode(byId(page, "a")));
    stats = update(page);
    CHECK(stats.elementsStyled == 0);
    CHECK(stats.boxesErased == 1);

    // display: none erases a box, with the boxes of its subtree, and removing it creates them again.
    prDomElementSetAttribute(byId(page, "b"), "class", "gone");
    stats = update(page);
    CHECK(stats.boxesErased == 1);
    prDomElementSetAttribute(byId(page, "b"), "class", "");
    stats = update(page);
    CHECK(stats.boxesCreated == 2);
}

TEST_CASE("a change to the root's font size restyles what reads rem, whichever size it changes from") {
    // #r's parent computes the same values whatever the root's size, so only rem reaches #r.
    Page page("<style>html { font-size: 2rem } #fixed { font-size: 10px } #r { padding-left: 1rem }</style>"
        "<div id=fixed><div id=r>text</div></div>");
    // rem in the root's own font-size is relative to the initial size.
    CHECK(px(page.box("r").padding[PrRectSide_Left]) == 32.0f);
    PrDomElement* root = prDomDocumentElement(page.document);
    prDomElementSetAttribute(root, "style", "font-size: 20px");
    update(page);
    CHECK(px(page.box("r").padding[PrRectSide_Left]) == 20.0f);
    prDomElementSetAttribute(root, "style", "font-size: 16px");
    update(page);
    CHECK(px(page.box("r").padding[PrRectSide_Left]) == 16.0f);
    // The root's rem stays relative to the initial size as the root's size changes.
    prDomElementRemoveAttribute(root, "style");
    update(page);
    CHECK(px(page.box("r").padding[PrRectSide_Left]) == 32.0f);
}

TEST_CASE("a rule inserted, removed or edited restyles the elements its selectors file under") {
    Page page("<style>.x { color: red }</style><style id=own></style>"
        "<div id=a class=card><span id=s>s</span><p>p</p></div><div id=b><span>t</span></div><em>e</em>");
    PrCssStyleSheetList* sheets = prCssDocumentStyleSheets(page.document);
    REQUIRE(prCssStyleSheetListLength(sheets) == 2);
    PrCssStyleSheet* own = prCssStyleSheetListStyleSheetAt(sheets, 1);
    CHECK(update(page).elementsStyled == 0);

    // A rule whose subjects are spans restyles the two spans, and so does its removal.
    REQUIRE(prCssStyleSheetInsertRule(own, ".card span { color: rgb(0, 120, 0) }", 0, nullptr) == PrStatus_Ok);
    CHECK(update(page).elementsStyled == 2);
    REQUIRE(prCssStyleSheetDeleteRule(own, 0) == PrStatus_Ok);
    CHECK(update(page).elementsStyled == 2);

    // A rule in a @media rule restyles by its own selectors; an edit of a rule's declarations restyles what it matches.
    REQUIRE(prCssStyleSheetInsertRule(own, "@media all { #b { width: 10px } }", 0, nullptr) == PrStatus_Ok);
    CHECK(update(page).elementsStyled == 1);
    REQUIRE(prCssStyleSheetInsertRule(own, "em { color: blue }", 0, nullptr) == PrStatus_Ok);
    CHECK(update(page).elementsStyled == 1);
    PrCssStyleRule* rule = prCssStyleRuleQuery(prCssRuleListRuleAt(prCssStyleSheetCssRules(own), 0));
    REQUIRE(rule != nullptr);
    PrCssDeclarationName color = prCssDeclarationNameFromProperty(PrCssProperty_Color);
    prCssStyleDeclarationSetProperty(prCssStylePropertiesAsCssStyleDeclaration(prCssStyleRuleStyle(rule)), &color,
        "green", false);
    CHECK(update(page).elementsStyled == 1);

    // A rule that a selector with no feature can match restyles every element.
    REQUIRE(prCssStyleSheetInsertRule(own, ":first-child { color: blue }", 0, nullptr) == PrStatus_Ok);
    CHECK(update(page).elementsStyled == 11);
}

TEST_CASE("selectors that combine elements restyle what a change reaches") {
    Page page("<style>.on .inner { color: red } .on + p { color: blue } li:first-child { color: green }</style>"
        "<div id=d><span class=inner id=i>x</span></div><p id=p>p</p><ul id=u><li id=l1>1</li></ul>");
    // A class on an ancestor restyles its descendants, and on a sibling its later siblings.
    prDomElementSetAttribute(byId(page, "d"), "class", "on");
    update(page);
    CHECK(prColorFromColor16(page.color("i").color).r == 1.0f);
    CHECK(prColorFromColor16(page.color("p").color).b == 1.0f);
    // A child added before another changes which is first.
    PrDomElement* first = nullptr;
    REQUIRE(prDomElementCreate(page.document, "li", nullptr, &first) == PrStatus_Ok);
    prDomNodeInsertBefore(prDomElementAsNode(byId(page, "u")), prDomElementAsNode(first), prDomElementAsNode(byId(page, "l1")));
    update(page);
    CHECK(prColorFromColor16(page.color("l1").color).g == 0.0f);
}

TEST_CASE("a scroll set while the layout thread lays out applies as the presentation is waited for") {
    Page page("<div style='height: 5000px'></div>");
    prHtmlPresentationStart(page.presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    prCssPresentationSetScroll(page.presentation, kPrLayoutNodeNone, prLayoutVecFromPx(PrVec2{0, 100}));
    prCssPresentationWait(page.presentation);
    PrLayoutVec scroll = prLayoutNodeScroll(prCssPresentationTree(page.presentation), kPrLayoutNodeNone);
    CHECK(prLayoutUnitToPx(scroll.y) == 100.0f);
    prLayoutSceneRelease(prCssPresentationTakeScene(page.presentation));
}

TEST_CASE("the presentations of an agent's documents lay out in turn on the agent's one layout thread") {
    Page page("<style>body { margin: 0 }</style><div id=a style='height: 10px'></div>");
    PrDomDocument* other = prHtmlDomParserParseFromString("<div style='height: 20px'>other</div>",
        PrHtmlDomParserSupportedType_TextHtml);
    uint32_t sheetCount = 0;
    const PrCssUserAgentSheet* sheets = prHtmlUserAgentSheets(&sheetCount);
    PrCssPresentationDesc desc{
        .document = other,
        .font = page.defaultFont,
        .shaper = page.shaper,
        .agent = page.agent,
        .userAgentSheets = sheets,
        .userAgentSheetCount = sheetCount,
    };
    PrCssPresentation* presentation = prCssPresentationCreate(&desc);
    std::atomic<uint32_t> done{0};
    prDomAgentSetLayoutDone(page.agent, [](void* context) { static_cast<std::atomic<uint32_t>*>(context)->fetch_add(1); }, &done);
    for (uint32_t frame = 0; frame < 10; ++frame) {
        prDomElementSetAttribute(byId(page, "a"), "style", (frame % 2 == 0 ? "height: 30px" : "height: 10px"));
        prHtmlPresentationStart(page.presentation, prLayoutVecFromPx(PrVec2{800, 600}));
        // The other presentation waits for this one's job before it takes its changes in, as the two share the layout
        // thread and its context.
        prHtmlPresentationStart(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
        const PrLayoutScene* first = prCssPresentationTakeScene(page.presentation);
        const PrLayoutScene* second = prCssPresentationTakeScene(presentation);
        CHECK(first != nullptr);
        CHECK(second != nullptr);
        CHECK(first != second);
        prLayoutSceneRelease(first);
        prLayoutSceneRelease(second);
        CHECK(page.bounds("a").bottom == (frame % 2 == 0 ? 30.0f : 10.0f));
    }
    CHECK(done.load() >= 10);
    CHECK(verified(page.presentation));
    CHECK(verified(presentation));
    prDomAgentSetLayoutDone(page.agent, nullptr, nullptr);
    prCssPresentationDestroy(presentation);
    prDomDocumentRelease(other);
}

TEST_CASE("a job budget takes new content in in steps, and changes to the content taken in at once") {
    Page page("<style>.hot { background-color: red }</style><div id=first>x</div><div id=list></div>");
    PrDomNode* list = prDomElementAsNode(byId(page, "list"));
    for (uint32_t i = 0; i < 1000; ++i) {
        PrDomElement* item = nullptr;
        REQUIRE(prDomElementCreate(page.document, "p", nullptr, &item) == PrStatus_Ok);
        prDomNodeAppendChild(list, prDomElementAsNode(item));
    }
    std::atomic<uint32_t> done{0};
    prDomAgentSetLayoutDone(page.agent, [](void* context) { static_cast<std::atomic<uint32_t>*>(context)->fetch_add(1); }, &done);
    // The smallest budget takes in the fewest elements that a job takes.
    prCssPresentationSetJobBudget(page.presentation, 1e-6);
    auto job = [&] {
        prHtmlPresentationStart(page.presentation, prLayoutVecFromPx(PrVec2{800, 600}));
        prCssPresentationWait(page.presentation);
        prLayoutSceneRelease(prCssPresentationTakeScene(page.presentation));
        return prCssPresentationStats(page.presentation);
    };
    CHECK(job().elementsStyled < 1000);
    CHECK(!prCssPresentationIsCurrent(page.presentation));

    // A class on an element already taken in is in the next job, while new content still waits.
    prDomElementSetAttribute(byId(page, "first"), "class", "hot");
    job();
    PrLayoutNode first = prCssPresentationLayoutNode(page.presentation, prDomElementAsNode(byId(page, "first")));
    PrLayoutTree* tree = prCssPresentationTree(page.presentation);
    PrLayoutStyle record = prLayoutStyleRecord(tree, prLayoutNodeStyle(tree, first));
    PrLayoutDecorationStyle decoration = prLayoutDecorationStyleRecord(tree, record.groups[PrLayoutGroup_Decoration]);
    CHECK(prColorFromColor16(decoration.background).r == 1.0f);
    CHECK(!prCssPresentationIsCurrent(page.presentation));

    // Jobs continue until the rest is in, and each tells the host that it is done.
    uint32_t jobs = 2;
    while (!prCssPresentationIsCurrent(page.presentation) && jobs < 1000) {
        job();
        ++jobs;
    }
    CHECK(prCssPresentationIsCurrent(page.presentation));
    CHECK(jobs > 4);
    CHECK(done.load() == jobs);
    CHECK(verified(page.presentation));
}

TEST_CASE("a presentation lays out and records on its own thread while the document changes") {
    Page page("<style>.big { height: 50px }</style><div id=outer><div id=a>a</div><div id=b>b</div></div>");
    PrDrawList* list = prDrawListCreate();
    for (uint32_t frame = 0; frame < 30; ++frame) {
        prHtmlPresentationStart(page.presentation, prLayoutVecFromPx(PrVec2{800, 600}));
        // The document changes while the layout thread lays out its previous state.
        prDomElementSetAttribute(byId(page, "a"), "class", (frame % 2 == 0 ? "big" : ""));
        if (frame % 5 == 0) {
            PrDomElement* added = nullptr;
            REQUIRE(prDomElementCreate(page.document, "p", nullptr, &added) == PrStatus_Ok);
            prDomNodeAppendChild(prDomElementAsNode(byId(page, "outer")), prDomElementAsNode(added));
        }
        // The paragraphs added at frames 0, 5, 10... are taken out again two frames later.
        if (frame % 5 == 2)
            prDomNodeRemoveChild(prDomElementAsNode(byId(page, "outer")),
                prDomNodeLastChild(prDomElementAsNode(byId(page, "outer"))));
        const PrLayoutScene* scene = prCssPresentationTakeScene(page.presentation);
        REQUIRE(scene != nullptr);
        prDrawListClear(list);
        prLayoutScenePlay(scene, list, nullptr);
        prLayoutSceneRelease(scene);
    }
    CHECK(update(page).elementsStyled >= 1);
    CHECK(verified(page.presentation));
    prDrawListRelease(list);
}

TEST_CASE("queries between a removal and the next update answer no node that left the document") {
    Page page("<style>body { margin: 0 } div { height: 20px }</style><div id=a></div><div id=b></div>");
    PrDomNode* a = prDomElementAsNode(byId(page, "a"));
    CHECK(prCssPresentationHitTest(page.presentation, prLayoutVecFromPx(PrVec2{5, 5})) == a);
    // The test's reference keeps the removed node alive to be asked about.
    prDomNodeRetain(a);
    prDomNodeRemoveChild(prDomNodeParentNode(a), a);
    // The tree is as the last update laid it out, but the node that it presents is gone.
    CHECK(prCssPresentationHitTest(page.presentation, prLayoutVecFromPx(PrVec2{5, 5})) != a);
    CHECK(prCssPresentationLayoutNode(page.presentation, a) == kPrLayoutNodeNone);
    update(page);
    PrDomNode* hit = prCssPresentationHitTest(page.presentation, prLayoutVecFromPx(PrVec2{5, 5}));
    CHECK(hit == prDomElementAsNode(byId(page, "b")));
    prDomNodeRelease(a);
}

TEST_CASE("a child added restyles a sibling's subtree only where a selector reads the sibling's place from below it") {
    Page page("<style>.s:last-child { background-color: blue } .t:last-child p { color: red }</style>"
        "<div id=a><div class=s id=s1><p>1</p><p>2</p><p>3</p></div></div>"
        "<div id=b><div class=t id=t1><p id=p1>1</p><p>2</p><p>3</p></div></div>");
    CHECK(prColorFromColor16(page.color("p1").color).r > 0.0f);

    // The section that was last is restyled, but not its paragraphs, which no selector reaches through the section's
    // position.
    PrDomElement* added = nullptr;
    REQUIRE(prDomElementCreate(page.document, "div", nullptr, &added) == PrStatus_Ok);
    prDomElementSetAttribute(added, "class", "s");
    prDomNodeAppendChild(prDomElementAsNode(byId(page, "a")), prDomElementAsNode(added));
    CHECK(update(page).elementsStyled == 2);

    // The paragraphs of the section that was last are restyled, because a selector reaches them through the section's
    // position.
    REQUIRE(prDomElementCreate(page.document, "div", nullptr, &added) == PrStatus_Ok);
    prDomElementSetAttribute(added, "class", "t");
    prDomNodeAppendChild(prDomElementAsNode(byId(page, "b")), prDomElementAsNode(added));
    CHECK(update(page).elementsStyled == 5);
    CHECK(prColorFromColor16(page.color("p1").color).r == 0.0f);
}

TEST_CASE("a child added restyles its siblings only where a selector read their places") {
    Page page("<style>li:last-child { color: green } .c { --a: 1 } .c.z { background-color: blue } p:empty { color: red }"
        "h1 + p { color: red } h2 ~ i { color: blue } b:nth-last-child(3) { color: green }</style><p id=e></p>"
        "<div id=s><p id=s1>1</p><p id=s2>2</p></div><div id=t><i id=t1>1</i><i id=t2>2</i></div>"
        "<div id=n><b id=n1>1</b><b id=n2>2</b></div><div id=d><span>a</span><span>b</span><span>c</span></div>"
        "<ul id=u><li id=l1>1</li><li id=l2>2</li></ul><div id=c class=c><b>x</b><b>y</b></div>");
    PrDomElement* added = nullptr;
    REQUIRE(prDomElementCreate(page.document, "span", nullptr, &added) == PrStatus_Ok);
    prDomNodeAppendChild(prDomElementAsNode(byId(page, "d")), prDomElementAsNode(added));
    CHECK(update(page).elementsStyled == 1);

    // The list's last child changes: the child added and the one that was last are restyled, and not the first.
    REQUIRE(prDomElementCreate(page.document, "li", nullptr, &added) == PrStatus_Ok);
    prDomElementSetAttribute(added, "id", "l3");
    prDomNodeAppendChild(prDomElementAsNode(byId(page, "u")), prDomElementAsNode(added));
    CHECK(update(page).elementsStyled == 2);
    CHECK(prColorFromColor16(page.color("l2").color).g == 0.0f);
    CHECK(prColorFromColor16(page.color("l3").color).g > 0.0f);
    prDomNodeRemove(prDomElementAsNode(added));
    CHECK(update(page).elementsStyled == 1);
    CHECK(prColorFromColor16(page.color("l2").color).g > 0.0f);

    // A lone `+` restyles the element after the change, and `~` and :nth-last-child() every element after or before.
    PrDomElement* heading = nullptr;
    REQUIRE(prDomElementCreate(page.document, "h1", nullptr, &heading) == PrStatus_Ok);
    prDomNodeInsertBefore(prDomElementAsNode(byId(page, "s")), prDomElementAsNode(heading),
        prDomElementAsNode(byId(page, "s1")));
    CHECK(update(page).elementsStyled == 2);
    CHECK(prColorFromColor16(page.color("s1").color).r > 0.0f);
    CHECK(prColorFromColor16(page.color("s2").color).r == 0.0f);
    REQUIRE(prDomElementCreate(page.document, "h2", nullptr, &heading) == PrStatus_Ok);
    prDomNodeInsertBefore(prDomElementAsNode(byId(page, "t")), prDomElementAsNode(heading),
        prDomElementAsNode(byId(page, "t1")));
    CHECK(update(page).elementsStyled == 3);
    CHECK(prColorFromColor16(page.color("t2").color).b > 0.0f);
    REQUIRE(prDomElementCreate(page.document, "b", nullptr, &added) == PrStatus_Ok);
    prDomNodeAppendChild(prDomElementAsNode(byId(page, "n")), prDomElementAsNode(added));
    CHECK(update(page).elementsStyled == 3);
    CHECK(prColorFromColor16(page.color("n1").color).g > 0.0f);
    CHECK(prColorFromColor16(page.color("n2").color).g == 0.0f);

    // A child of an element that :empty tested restyles the element.
    prDomNodeSetTextContent(prDomElementAsNode(byId(page, "e")), "x");
    CHECK(update(page).elementsStyled == 1);
    CHECK(prColorFromColor16(page.color("e").color).r == 0.0f);

    // Custom properties computed again to the same values restyle no descendant.
    prDomElementSetAttribute(byId(page, "c"), "class", "c z");
    CHECK(update(page).elementsStyled == 1);
}

TEST_CASE("a new viewport restyles only where a style reads it, and new media only where a query answers otherwise") {
    Page page("<style>p { margin: 1em }</style><p>a</p><p>b</p>");
    prHtmlPresentationUpdate(page.presentation, prLayoutVecFromPx(PrVec2{700, 600}));
    CHECK(prCssPresentationStats(page.presentation).elementsStyled == 0);
    Page units("<style>p { width: 10vw }</style><p id=p>a</p>");
    prHtmlPresentationUpdate(units.presentation, prLayoutVecFromPx(PrVec2{700, 600}));
    CHECK(prCssPresentationStats(units.presentation).elementsStyled == 5);
    CHECK(units.bounds("p").right - units.bounds("p").left == 70.0f);

    PrHtmlHost host = prHtmlHostDefault();
    host.font = page.defaultFont;
    host.shaper = page.shaper;
    PrHtmlPage* htmlPage = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(htmlPage, "<style>@media (min-width: 500px) { p { color: red } }</style><p>a</p>",
        "about:blank", nullptr) == PrStatus_Ok);
    PrCssPresentation* presentation = prHtmlPagePresentation(htmlPage);
    prHtmlPageSetMediaFeature(htmlPage, PrCssDescriptor_MediaWidth, "800px");
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    prHtmlPageSetMediaFeature(htmlPage, PrCssDescriptor_MediaWidth, "700px");
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(prCssPresentationStats(presentation).elementsStyled == 0);
    prHtmlPageSetMediaFeature(htmlPage, PrCssDescriptor_MediaWidth, "400px");
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(prCssPresentationStats(presentation).elementsStyled > 0);
    prHtmlPageDestroy(htmlPage);
}

TEST_CASE("a change restyles the elements whose selectors read the feature that changed") {
    Page page("<style>.on .inner { color: red } .on > b { color: blue } .lead ~ p { color: green }"
        "#key { width: 5px } #key[data-x] { width: 6px } .menu:hover .item { color: red }"
        ".self { color: blue } li:nth-child(odd of .pick) { color: red }</style>"
        "<div id=d><span class=inner id=i>x</span><span>y</span><b id=b>z</b><i><span class=inner>w</span></i>"
        "</div><p id=p1>p</p><p id=p2>p</p><p id=p3>p</p>"
        "<div id=m class=menu><span class=item id=item>a</span><span>b</span></div>"
        "<ul id=u><li id=l1>1</li><li id=l2>2</li><li id=l3>3</li></ul>");

    // A class on an ancestor restyles the descendants that its selectors name, and not the ancestor or the rest.
    prDomElementSetAttribute(byId(page, "d"), "class", "on");
    CHECK(update(page).elementsStyled == 3);
    CHECK(prColorFromColor16(page.color("i").color).r == 1.0f);
    CHECK(prColorFromColor16(page.color("b").color).b == 1.0f);
    // The same classes in another order change nothing, and a class that no selector reads restyles nothing.
    prDomElementSetAttribute(byId(page, "d"), "class", " on  unread ");
    CHECK(update(page).elementsStyled == 0);
    // A class of the subject restyles the element alone.
    prDomElementSetAttribute(byId(page, "p2"), "class", "self");
    CHECK(update(page).elementsStyled == 1);

    // A class left of a sibling combinator restyles the later siblings, with their subtrees: two paragraphs, the menu
    // and the list.
    prDomElementSetAttribute(byId(page, "p1"), "class", "lead");
    CHECK(update(page).elementsStyled == 9);
    CHECK(prColorFromColor16(page.color("p3").color).g > 0.0f);

    // An ID and an attribute that the selectors name restyle the element.
    prDomElementSetAttribute(byId(page, "p3"), "id", "key");
    CHECK(update(page).elementsStyled == 1);
    CHECK(px(page.box("key").width) == 5.0f);
    prDomElementSetAttribute(byId(page, "key"), "data-x", "");
    CHECK(update(page).elementsStyled == 1);
    CHECK(px(page.box("key").width) == 6.0f);

    // A state restyles the descendants that its selectors name.
    prHtmlDocumentSetHover(page.document, byId(page, "m"));
    CHECK(update(page).elementsStyled == 1);
    CHECK(prColorFromColor16(page.color("item").color).r == 1.0f);
    prHtmlDocumentSetHover(page.document, nullptr);
    CHECK(update(page).elementsStyled == 1);
    // A state changes what the compound reads only on an element that carries the compound's feature.
    prHtmlDocumentSetHover(page.document, byId(page, "d"));
    CHECK(update(page).elementsStyled == 0);
    prHtmlDocumentSetHover(page.document, nullptr);
    CHECK(update(page).elementsStyled == 0);

    // A class in an `of` selector changes which siblings count.
    prDomElementSetAttribute(byId(page, "l2"), "class", "pick");
    update(page);
    CHECK(prColorFromColor16(page.color("l2").color).r == 1.0f);
    prDomElementSetAttribute(byId(page, "l1"), "class", "pick");
    update(page);
    CHECK(prColorFromColor16(page.color("l1").color).r == 1.0f);
    CHECK(prColorFromColor16(page.color("l2").color).r == 0.0f);
}

TEST_CASE("a restyle of whole subtrees divides its work among a pool's threads, and gives the styles one thread does") {
    std::string html = "<style>.row:nth-child(odd) { color: blue } .row .cell { padding-left: 2px }"
        ".dark * { color: white; background-color: black } :root { --gap: 3px }"
        ".cell { margin-top: var(--gap) } :disabled { color: gray } input:default { width: 3px }"
        "</style><form id=f></form><div id=top>";
    for (uint32_t row = 0; row < 100; ++row) {
        // States that the selectors read, one of which looks up the form by ID.
        html += "<div class=row><input type=submit form=f>";
        for (uint32_t cell = 0; cell < 8; ++cell)
            html += "<span class=cell style='width: " + std::to_string(cell) + "px'>x</span>";
        html += "</div>";
    }
    html += "</div>";
    HostPool pool(4);
    Page parallel(html.c_str(), PrVec2{800, 600}, withPool(pool));
    Page serial(html.c_str());
    PrCssPresentationStats stats = prCssPresentationStats(parallel.presentation);
    CHECK(pool.widest > 1);
    CHECK(stats.elementsStyledAhead > 900);
    CHECK(prCssPresentationStats(serial.presentation).elementsStyledAhead == 0);
    CHECK(verified(parallel.presentation));
    CHECK(stats.elementsStyled == prCssPresentationStats(serial.presentation).elementsStyled);

    // A class on an ancestor that restyles its whole subtree: the rows, their inputs and their cells.
    for (Page* page : {&parallel, &serial}) {
        prDomElementSetAttribute(byId(*page, "top"), "class", "dark");
        stats = update(*page);
        CHECK(stats.elementsStyled == 1000);
    }
    CHECK(prCssPresentationStats(parallel.presentation).elementsStyledAhead == 1000);
    CHECK(verified(parallel.presentation));
}

TEST_CASE("a parallel restyle under an element with a running transition inherits what the transition presents") {
    // The threads compute each style under its parent's style from the cascade, which a running transition is not part
    // of.
    std::string html = "<style>.x { color: black; transition: color 1s linear } .x.on { color: red }"
        " .dark * { padding-left: 1px }</style><div id=top><div id=a class=x>";
    for (uint32_t i = 0; i < 600; ++i)
        html += "<span>x</span>";
    html += "</div></div>";
    HostPool pool(4);
    Page page(html.c_str(), PrVec2{800, 600}, withPool(pool));
    prCssPresentationSetTime(page.presentation, 0);
    update(page);
    prDomElementSetAttribute(byId(page, "a"), "class", "x on");
    update(page);
    prCssPresentationSetTime(page.presentation, 500);
    update(page);
    prDomElementSetAttribute(byId(page, "top"), "class", "dark");
    uint32_t loops = pool.loops;
    PrCssPresentationStats stats = update(page);
    CHECK(pool.loops > loops);
    CHECK(stats.elementsStyledAhead > 0);
    CHECK(verified(page.presentation));
}

TEST_CASE("a transition repaints, relayouts or composites as its property says, frame by frame") {
    Page page("<style>body { margin: 0 } div { height: 10px; transition: all 1s linear }"
        "#paint { background-color: rgb(255, 0, 0) } #paint.on { background-color: rgb(0, 0, 255) }"
        "#size { width: 100px } #size.on { width: 200px }#fade { opacity: 0.8 } #fade.on { opacity: 0.2 }</style>"
        "<div id=paint></div><div id=size></div><div id=fade>text</div>");
    frame(page, 0);

    // A background color repaints its box's stacking context, and lays out nothing.
    prDomElementSetAttribute(byId(page, "paint"), "class", "on");
    frame(page, 0);
    CHECK(prCssPresentationIsAnimating(page.presentation));
    PrLayoutTreeStats stats = frame(page, 500);
    CHECK(prCssPresentationStats(page.presentation).elementsStyled == 0);
    CHECK(prCssPresentationStats(page.presentation).elementsAnimated == 1);
    CHECK(stats.blocksLaidOut == 0);
    CHECK(stats.layersRecorded == 1);
    CHECK(prColorFromColor16(page.decoration("paint").background).r == doctest::Approx(0.5).epsilon(0.01));
    frame(page, 1000);
    CHECK(prColorFromColor16(page.decoration("paint").background).b == 1.0f);
    CHECK_FALSE(prCssPresentationIsAnimating(page.presentation));

    // A width lays out the box and its parent.
    prDomElementSetAttribute(byId(page, "size"), "class", "on");
    frame(page, 1000);
    stats = frame(page, 1250);
    CHECK(stats.blocksLaidOut >= 1);
    CHECK(px(page.box("size").width) == doctest::Approx(125));

    // Opacity is composited: the box is a layer that keeps its base opacity, and a compositor runs the transition's
    // track. Its frames style, lay out and record nothing.
    frame(page, 2000);
    prDomElementSetAttribute(byId(page, "fade"), "class", "on");
    frame(page, 2000);
    stats = frame(page, 2500);
    CHECK(prCssPresentationStats(page.presentation).elementsStyled == 0);
    CHECK(prCssPresentationStats(page.presentation).elementsAnimated == 0);
    CHECK(stats.blocksLaidOut == 0);
    CHECK(stats.layersRecorded == 0);
    PrLayoutEffectStyle effects = page.effects("fade");
    CHECK(effects.layer);
    CHECK(effects.opacity == doctest::Approx(0.2));
    PrLayoutTrack track{};
    REQUIRE(prCssPresentationTracks(page.presentation, &track, 1) == 1);
    CHECK(track.node == page.node("fade"));
    float opacity = 0;
    PrMatrix4 matrix = prMatrix4Identity();
    REQUIRE(prLayoutTrackSample(&track, 2500, PrVec2{0, 0}, &opacity, &matrix));
    CHECK(double(opacity) == doctest::Approx(0.5));
    // A script reads the value that the compositor shows.
    PrCssDeclarationName name = prCssDeclarationNameFromProperty(PrCssProperty_Opacity);
    PrCssStyleDeclaration* computed =
        prCssStylePropertiesAsCssStyleDeclaration(prCssElementComputedStyle(byId(page, "fade"), PrStringView{}));
    PrRawString read = prCssStyleDeclarationGetPropertyValue(computed, &name);
    CHECK(std::string(prRawStringView(&read).data, prRawStringView(&read).size) == "0.5");
    prRawStringReset(&read);
    // A change midway transitions from the current value.
    prDomElementSetAttribute(byId(page, "fade"), "class", "");
    frame(page, 2500);
    REQUIRE(prCssPresentationTracks(page.presentation, &track, 1) == 1);
    REQUIRE(prLayoutTrackSample(&track, 3000, PrVec2{0, 0}, &opacity, &matrix));
    CHECK(double(opacity) == doctest::Approx(0.65));
    // Once it ends, the box is no longer a layer.
    frame(page, 4000);
    CHECK(prCssPresentationTracks(page.presentation, nullptr, 0) == 0);
    effects = page.effects("fade");
    CHECK_FALSE(effects.layer);
    CHECK(effects.opacity == doctest::Approx(0.8));
}

TEST_CASE("a CSS animation runs its keyframes, iterations and direction") {
    Page page("<style>@keyframes grow { from { width: 10px } 50% { width: 60px } to { width: 110px } }"
        "#a { height: 5px; animation: grow 1s linear 2 alternate } #b { width: 7px }</style><div id=a></div><div id=b></div>");
    CHECK(prCssPresentationIsAnimating(page.presentation));
    frame(page, 250);
    CHECK(px(page.box("a").width) == doctest::Approx(35));
    frame(page, 1250);
    CHECK(px(page.box("a").width) == doctest::Approx(85));
    // After its iterations it shows the base value, which is auto.
    frame(page, 2500);
    CHECK(page.box("a").width.bits == prLayoutLengthAuto().bits);
    CHECK_FALSE(prCssPresentationIsAnimating(page.presentation));
    // Naming the animation on another element starts it there, at the time at which it is named.
    prDomElementSetAttribute(byId(page, "b"), "style", "animation: grow 2s linear forwards");
    frame(page, 3000);
    frame(page, 3500);
    CHECK(px(page.box("b").width) == doctest::Approx(35));
    frame(page, 6000);
    CHECK(px(page.box("b").width) == doctest::Approx(110));
}

TEST_CASE("a keyframe of initial, inherit or unset takes the value that the keyword gives in a declaration") {
    Page page("<style>@keyframes k { from { width: inherit; padding-left: initial; margin-left: unset } "
        "to { width: 110px; padding-left: 40px; margin-left: 40px } }"
        "#p { width: 50px } #a { padding-left: 20px; margin-left: 20px; animation: k 1s linear }</style>"
        "<div id=p><div id=a></div></div>");
    frame(page, 250);
    PrLayoutBoxStyle box = page.box("a");
    CHECK(px(box.width) == doctest::Approx(65));
    CHECK(px(box.padding[PrRectSide_Left]) == doctest::Approx(10));
    // margin-left is not inherited, so unset is its initial value.
    CHECK(px(box.margin[PrRectSide_Left]) == doctest::Approx(10));
    // The inherited keyframe follows the parent's value, although width does not inherit.
    prDomElementSetAttribute(byId(page, "p"), "style", "width: 10px");
    frame(page, 500);
    CHECK(px(page.box("a").width) == doctest::Approx(60));
}

TEST_CASE("a keyframe that adds or accumulates is composited onto the underlying value before interpolating") {
    Page page("<style>@keyframes k { from { width: 100px; animation-composition: replace } to { width: 200px } "
        "to { scale: 2 } }#a { width: 100px; scale: 3; animation: k 1s linear; animation-composition: add }"
        "#b { scale: 3; animation: k 1s linear; animation-composition: accumulate }</style><div id=a></div><div id=b></div>");
    frame(page, 250);
    // From 100px, which replaces, to 100px plus 200px.
    CHECK(px(page.box("a").width) == doctest::Approx(150));
    // A scale that adds multiplies, and one that accumulates adds its factors less one.
    CHECK(double(page.effects("a").transform.matrix.m[0][0]) == doctest::Approx(3 + (6 - 3) * 0.25));
    CHECK(double(page.effects("b").transform.matrix.m[0][0]) == doctest::Approx(3 + (4 - 3) * 0.25));
}

TEST_CASE("revert in a keyframe takes the user agent's value, and revert-layer the element's base value") {
    Page page("<style>@keyframes r { from, to { margin-top: 0px } 50% { margin-top: revert } }"
        "@keyframes l { from, to { width: 0px } 50% { width: revert-layer } }"
        "#r { margin-top: 0px; animation: r 1s linear } #l { width: 100px; animation: l 1s linear }</style>"
        "<h1 id=r></h1><h1 id=ref></h1><div id=l></div>");
    frame(page, 500);
    CHECK(resolved(page, "r", PrCssProperty_MarginTop) == resolved(page, "ref", PrCssProperty_MarginTop));
    CHECK(resolved(page, "r", PrCssProperty_MarginTop) != "0px");
    CHECK(resolved(page, "l", PrCssProperty_Width) == "100px");
    frame(page, 750);
    CHECK(resolved(page, "l", PrCssProperty_Width) == "50px");
}

TEST_CASE("path data interpolates argument by argument between paths of the same commands") {
    Page page("<style>@keyframes rise { from { d: path('M 20 50 L 80 50') } to { d: path('M 20 50 L 80 10') } }"
        "@keyframes mixed { from { d: path('m 10 20 l 40 50 z l 40 60') } to { d: path('M 210 220 L 170 190 Z L 90 120') } }"
        "@keyframes sweep { from { d: path('M 0 0 A 5 5 0 0 0 10 0') } to { d: path('M 0 0 A 5 5 0 0 1 10 0') } }"
        "@keyframes close { from { d: path('M 0 0 L 10 0') } to { d: path('M 0 0 L 10 0 Z') } }"
        "@keyframes down { from { d: path('M 0 10') } to { d: path('M 0 -10') } }"
        "@keyframes add { from { d: path('M 1 2 L 3 4'); animation-composition: add } "
        "to { d: path('M 1 2 L 3 4'); animation-composition: add } }path { animation: 1s linear }</style>"
        "<svg><path id=rise style='animation-name: rise'/><path id=mixed style='animation-name: mixed'/>"
        "<path id=sweep style='animation-name: sweep'/><path id=close style='animation-name: close'/>"
        "<path id=add d='M 10 20 L 30 40' style='animation-name: add'/><path id=down style='animation-name: down'/></svg>");
    frame(page, 250);
    CHECK(resolved(page, "rise", PrCssProperty_D) == "path(\"M 20 50 L 80 40\")");
    // Paths interpolate in absolute coordinates, whether their commands are relative or not.
    CHECK(resolved(page, "mixed", PrCssProperty_D) == "path(\"M 60 70 L 80 100 Z L 60 90\")");
    // A flag above zero is set.
    CHECK(resolved(page, "sweep", PrCssProperty_D) == "path(\"M 0 0 A 5 5 0 0 1 10 0\")");
    // Paths of other commands switch halfway.
    CHECK(resolved(page, "close", PrCssProperty_D) == "path(\"M 0 0 L 10 0\")");
    // A keyframe that adds sums its arguments with the underlying path's.
    CHECK(resolved(page, "add", PrCssProperty_D) == "path(\"M 11 22 L 33 44\")");
    frame(page, 750);
    CHECK(resolved(page, "close", PrCssProperty_D) == "path(\"M 0 0 L 10 0 Z\")");
    // Path data has no range: a coordinate passes below zero, whatever the property's numbers allow.
    CHECK(resolved(page, "down", PrCssProperty_D) == "path(\"M 0 -5\")");
}

TEST_CASE("filter lists interpolate function by function, padding the shorter list with initial values") {
    Page page("<style>@keyframes pad { from { filter: blur(10px) } to { filter: blur(20px) sepia(1) } }"
        "@keyframes grow { from { filter: none } to { filter: opacity(0.5) drop-shadow(red 10px 0px) } }"
        "@keyframes other { from { filter: blur(10px) } to { filter: sepia(1) } }"
        "@keyframes add { to { filter: sepia(1); animation-composition: add } }"
        "@keyframes acc { from, to { filter: blur(4px); animation-composition: accumulate } }div { animation: 1s linear }"
        "#pad { animation-name: pad } #grow { animation-name: grow } #other { animation-name: other }"
        "#add { filter: blur(4px); animation-name: add } #acc { filter: blur(6px); animation-name: acc }"
        "</style><div id=pad></div><div id=grow></div><div id=other></div><div id=add></div><div id=acc></div>");
    frame(page, 250);
    CHECK(resolved(page, "pad", PrCssProperty_Filter) == "blur(12.5px) sepia(0.25)");
    // none stands for the initial values of the other list's functions. A shadow's initial color is transparent.
    CHECK(resolved(page, "grow", PrCssProperty_Filter) == "opacity(0.875) drop-shadow(rgba(255, 0, 0, 0.25) 2.5px 0px 0px)");
    // Lists of other functions switch halfway.
    CHECK(resolved(page, "other", PrCssProperty_Filter) == "blur(10px)");
    // Addition appends the keyframe's list to the underlying list. Accumulation sums functions of the same name.
    CHECK(resolved(page, "add", PrCssProperty_Filter) == "blur(4px) sepia(0.25)");
    CHECK(resolved(page, "acc", PrCssProperty_Filter) == "blur(10px)");
}

TEST_CASE("filter() images of one input interpolate by their filter lists, and other images switch halfway") {
    Page page("<style>@keyframes gray { from { background-image: filter(linear-gradient(red, blue), grayscale(0)), "
        "none } to { background-image: filter(linear-gradient(red, blue), grayscale(1)), none } }"
        "@keyframes other { from { background-image: filter(linear-gradient(red, blue), grayscale(0)) } "
        "to { background-image: filter(linear-gradient(red, lime), grayscale(1)) } }"
        "@keyframes mask { from { mask-image: filter(url(a.png), blur(0px)) } to { mask-image: filter(url(a.png), blur(8px)) } }"
        "div { animation: 1s linear } #gray { animation-name: gray } #other { animation-name: other }"
        "#mask { animation-name: mask }</style><div id=gray></div><div id=other></div><div id=mask></div>");
    frame(page, 250);
    CHECK(resolved(page, "gray", PrCssProperty_BackgroundImage) == "filter(linear-gradient(red, blue), grayscale(0.25)), none");
    CHECK(resolved(page, "other", PrCssProperty_BackgroundImage) == "filter(linear-gradient(red, blue), grayscale(0))");
    CHECK(resolved(page, "mask", PrCssProperty_MaskImage) == "filter(url(\"a.png\"), blur(2px))");
}

TEST_CASE("basic shapes interpolate where their kinds agree, and clip interpolates each edge") {
    Page page("<style>@keyframes circle { from { clip-path: circle(10px at 0px 0px) } to { clip-path: circle(50px at 100% 40px) } }"
        "@keyframes inset { from { clip-path: inset(0px) } to { clip-path: xywh(10px 20px 50% 40px) } }"
        "@keyframes box { from { clip-path: circle(10px) } to { clip-path: circle(10px) padding-box } }"
        "@keyframes rect { from { clip: rect(0px, 100px, 50px, 0px) } to { clip: rect(20px, 140px, 90px, 40px) } }"
        "div { position: absolute; animation: 1s linear }"
        "#circle { animation-name: circle } #inset { animation-name: inset } #box { animation-name: box }"
        "#rect { animation-name: rect }</style>"
        "<div id=circle></div><div id=inset></div><div id=box></div><div id=rect></div>");
    frame(page, 250);
    CHECK(resolved(page, "circle", PrCssProperty_ClipPath) == "circle(20px at 25% 10px)");
    CHECK(resolved(page, "inset", PrCssProperty_ClipPath) == "inset(5px calc(12.5% - 2.5px) calc(25% - 15px) 2.5px)");
    // Shapes in other reference boxes switch halfway.
    CHECK(resolved(page, "box", PrCssProperty_ClipPath) == "circle(10px)");
    CHECK(resolved(page, "rect", PrCssProperty_Clip) == "rect(5px 110px 60px 10px)");
}

TEST_CASE("xywh() and rect() compute to inset(), and a shadow's current color to the element's color") {
    Page page("<div id=xywh style='clip-path: xywh(10px 20px 30% 40px round 5px)'></div>"
        "<div id=rect style='clip-path: rect(10px auto 30px 5px)'></div>"
        "<div id=shadow style='color: blue; filter: drop-shadow(1px 2px)'></div>");
    CHECK(resolved(page, "xywh", PrCssProperty_ClipPath) == "inset(20px calc(70% - 10px) calc(100% - 60px) 10px round 5px)");
    CHECK(resolved(page, "rect", PrCssProperty_ClipPath) == "inset(10px 0% calc(100% - 30px) 5px)");
    CHECK(resolved(page, "shadow", PrCssProperty_Filter) == "drop-shadow(rgb(0, 0, 255) 1px 2px 0px)");
}

TEST_CASE("the filter attribute takes lengths and angles without units, as SVG numbers") {
    Page page("<svg><rect id=shadow filter='drop-shadow(4 5 6) blur(2)' /><rect id=turn filter='hue-rotate(90)' />"
        "<rect id=css filter='blur(2px)' /></svg>");
    CHECK(resolved(page, "shadow", PrCssProperty_Filter) == "drop-shadow(rgb(0, 0, 0) 4px 5px 6px) blur(2px)");
    CHECK(resolved(page, "turn", PrCssProperty_Filter) == "hue-rotate(90deg)");
    CHECK(resolved(page, "css", PrCssProperty_Filter) == "blur(2px)");
}

TEST_CASE("a mask border's slices, widths and outsets interpolate side by side, and keep fill") {
    Page page("<style>@keyframes k { from { mask-border-slice: 10 fill; mask-border-width: 2px } "
        "to { mask-border-slice: 20 30 fill; mask-border-width: 6px 10px 2px } }"
        "#a { animation: k 1s linear }</style><div id=a></div>");
    frame(page, 250);
    CHECK(resolved(page, "a", PrCssProperty_MaskBorderSlice) == "12.5 15 fill");
    CHECK(resolved(page, "a", PrCssProperty_MaskBorderWidth) == "3px 4px 2px");
}

TEST_CASE("lists of a repeatable list repeat to a common length before they add or interpolate") {
    Page page("<style>@keyframes k { from { background-size: 60px 60px, 260px 260px; animation-composition: add } "
        "to { background-size: 160px 160px; animation-composition: add } }"
        "@keyframes w { from { background-size: 10px 10px } to { background-size: 20px 20px, 0px 0px } }"
        "@keyframes n { from { background-size: 100px 150px; animation-composition: add } "
        "to { background-size: 200px 250px } }div { background-image: url(), url() }"
        "#a { background-size: 40px 40px; animation: k 1s linear }#b { animation: w 1s linear }"
        "#c { background-size: auto, contain; animation: n 1s linear }</style><div id=a></div><div id=b></div><div id=c></div>");
    frame(page, 0);
    // The underlying value and the last keyframe repeat to two items each.
    CHECK(resolved(page, "a", PrCssProperty_BackgroundSize) == "100px 100px, 300px 300px");
    // An item that does not add leaves the keyframe's value as it is written.
    CHECK(resolved(page, "c", PrCssProperty_BackgroundSize) == "100px 150px");
    frame(page, 500);
    CHECK(resolved(page, "a", PrCssProperty_BackgroundSize) == "150px 150px, 250px 250px");
    CHECK(resolved(page, "b", PrCssProperty_BackgroundSize) == "15px 15px, 5px 5px");
}

TEST_CASE("pseudo-elements run transitions and CSS animations as elements do") {
    Page page("<style>@keyframes blue { from { color: rgb(0, 0, 0) } to { color: rgb(0, 0, 255) } }"
        "@keyframes gone { from, to { content: none } } @keyframes fade { to { opacity: 0 } }"
        "#a::before { content: 'B'; display: inline-block; width: 10px; transition: width 1s linear }"
        "#a.on::before { width: 110px } #a.none::before { content: none }"
        "#a::after { content: 'A' } #a.gone::after { animation: gone 1s }"
        "#a.fade::after { animation: fade 1s linear } li::marker { animation: blue 1s linear }</style>"
        "<div id=a>t</div><ul><li id=li>i</li></ul>");
    frame(page, 0);
    CHECK(resolved(page, "li", PrCssProperty_Color, "::marker") == "rgb(0, 0, 0)");
    prDomElementSetAttribute(byId(page, "a"), "class", "on");
    frame(page, 0);
    frame(page, 250);
    CHECK(resolved(page, "a", PrCssProperty_Width, "::before") == "35px");
    CHECK(resolved(page, "li", PrCssProperty_Color, "::marker") == "rgb(0, 0, 64)");
    frame(page, 500);
    CHECK(resolved(page, "a", PrCssProperty_Width, "::before") == "60px");

    // An animation that takes the box away keeps the pseudo-element, which has its box again once the animation ends.
    prDomElementSetAttribute(byId(page, "a"), "class", "on gone");
    frame(page, 500);
    CHECK(childBoxes(page, "a") == "[B] t");
    frame(page, 1600);
    CHECK(childBoxes(page, "a") == "[B] t [A]");

    // A composited animation of a pseudo-element runs on a track of its box.
    prDomElementSetAttribute(byId(page, "a"), "class", "on fade");
    frame(page, 1600);
    PrLayoutTrack track{};
    REQUIRE(prCssPresentationTracks(page.presentation, &track, 1) == 1);
    CHECK(track.node != page.node("a"));
    CHECK(prLayoutNodeParent(prCssPresentationTree(page.presentation), track.node) == page.node("a"));
    frame(page, 2100);
    CHECK(resolved(page, "a", PrCssProperty_Opacity, "::after") == "0.5");

    // A pseudo-element that stops generating a box drops its animations.
    prDomElementSetAttribute(byId(page, "a"), "class", "none");
    frame(page, 2100);
    CHECK(childBoxes(page, "a") == "t [A]");
    frame(page, 3000);
    CHECK_FALSE(prCssPresentationIsAnimating(page.presentation));
}

TEST_CASE("a host that steps its own clock runs transitions frame by frame") {
    Page fonts("");
    SteppedClock clock;
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &clock;
    host.now = steppedNow;
    host.font = fonts.defaultFont;
    host.shaper = fonts.shaper;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(page, "<style>#a { width: 100px; transition: width 1s linear }"
        "#a.wide { width: 300px }</style><div id=a></div>", "about:blank", nullptr) == PrStatus_Ok);
    PrCssPresentation* presentation = prHtmlPagePresentation(page);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    PrDomElement* a = prDomNodeGetElementById(prDomDocumentAsNode(document), "a");
    REQUIRE(a != nullptr);
    auto width = [&] {
        prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
        PrLayoutTree* tree = prCssPresentationTree(presentation);
        PrLayoutNode node = prCssPresentationLayoutNode(presentation, prDomElementAsNode(a));
        PrLayoutStyle style = prLayoutStyleRecord(tree, prLayoutNodeStyle(tree, node));
        return px(prLayoutBoxStyleRecord(tree, style.groups[PrLayoutGroup_Box]).width);
    };
    // The first rendering update reveals the document; then nothing is due.
    clock.now = 20;
    runDue(page);
    CHECK(width() == 100.0f);
    CHECK(prDomAgentNextTaskDelay(prHtmlPageAgent(page)) == -1);

    // A change starts a transition, which wants a frame at each rendering opportunity until it ends.
    prDomElementSetAttribute(a, "class", "wide");
    CHECK(width() == 100.0f);
    CHECK(prDomAgentNextTaskDelay(prHtmlPageAgent(page)) >= 0);
    clock.now = 520;
    runDue(page);
    CHECK(width() == doctest::Approx(200).epsilon(0.01));
    clock.now = 1100;
    runDue(page);
    CHECK(width() == 300.0f);
    CHECK(prDomAgentNextTaskDelay(prHtmlPageAgent(page)) == -1);
    prHtmlPageDestroy(page);
}

TEST_CASE("a host that observes its animations through events alone runs the frames they are seen in") {
    Observed all = observeAnimation(PrHtmlAnimationFrames_All);
    Observed observed = observeAnimation(PrHtmlAnimationFrames_Observed);
    auto joined = [](const Observed& o) {
        std::string text;
        for (const std::string& event : o.events)
            text += event + "; ";
        return text;
    };
    REQUIRE(all.events.size() == 3);
    CHECK(joined(observed) == joined(all));
    CHECK(observed.timerSaw == all.timerSaw);
    CHECK(all.timerSaw > 12000);
    // Every frame of 20 seconds of animation, compared with the frames at its events, the timer, and the frames before
    // them.
    CHECK(all.frames > 1000);
    CHECK(observed.frames < 20);
}

TEST_CASE("a click on a link to a fragment scrolls to the element it names, which matches :target") {
    Page fonts("");
    SteppedClock clock;
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &clock;
    host.now = steppedNow;
    host.font = fonts.defaultFont;
    host.shaper = fonts.shaper;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(page, "<!doctype html><style>body { margin: 0 } div { height: 1000px }</style>"
        "<a id=link href='#x'>x</a><a id=held href='#y'>y</a><div></div>"
        "<p id=x>x</p><div></div><a name=y>y</a><div></div>", "about:blank", nullptr) == PrStatus_Ok);
    PrHtmlWindow* window = prHtmlPageWindow(page);
    PrDomDocument* document = prHtmlWindowDocument(window);
    auto byId = [&](const char* id) {
        PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(document), id);
        REQUIRE(element != nullptr);
        return element;
    };
    auto click = [&](PrDomElement* element, PrDomEventKind kind) {
        PrDomMouseEventInit init = prDomMouseEventInitDefault();
        init.ui.event.bubbles = true;
        init.ui.event.cancelable = true;
        PrDomEvent* event;
        if (kind == PrDomEventKind_Mouse)
            event = prDomMouseEventAsEvent(prDomMouseEventCreate("click", &init));
        else
            event = prDomEventCreate("click", &init.ui.event);
        prDomEventTargetDispatchEvent(prDomNodeAsEventTarget(prDomElementAsNode(element)), event, nullptr);
        prDomEventRelease(event);
    };
    auto target = [&](PrDomElement* element) {
        bool matches = false;
        REQUIRE(prCssElementMatches(element, ":target", &matches) == PrStatus_Ok);
        return matches;
    };
    clock.now = 20;
    runDue(page);

    // A click that is not a MouseEvent activates nothing.
    click(byId("link"), PrDomEventKind_Event);
    CHECK(prHtmlWindowScrollY(window) == 0.0);

    // The link's fragment names an element by its ID, which scrolls to the top of the viewport and becomes the target.
    click(byId("link"), PrDomEventKind_Mouse);
    double scrolled = prHtmlWindowScrollY(window);
    CHECK(scrolled > 1000.0);
    CHECK(prCssElementGetBoundingClientRect(byId("x")).y == 0.0);
    CHECK(target(byId("x")));
    PrRawString hash = prHtmlWindowLocation(window, PrUrlComponent_Hash);
    CHECK(std::string(prRawStringView(&hash).data, prRawStringView(&hash).size) == "#x");
    prRawStringReset(&hash);

    // A click that a listener cancels follows nothing.
    PrDomEventListener cancel{
        [](void* PR_NULLABLE, PrDomEvent* event) { prDomEventPreventDefault(event); },
        nullptr,
        1,
        nullptr,
        nullptr,
    };
    PrDomEventTarget* held = prDomNodeAsEventTarget(prDomElementAsNode(byId("held")));
    prDomEventTargetAddEventListener(held, "click", &cancel, nullptr);
    click(byId("held"), PrDomEventKind_Mouse);
    CHECK(prHtmlWindowScrollY(window) == scrolled);
    prDomEventTargetRemoveEventListener(held, "click", &cancel, nullptr);

    // A fragment can name an a element by its name, and the top of the document.
    click(byId("held"), PrDomEventKind_Mouse);
    CHECK(prHtmlWindowScrollY(window) > 2000.0);
    CHECK_FALSE(target(byId("x")));
    CHECK(prHtmlWindowSetLocation(window, window, PrUrlComponent_Hash, "top", PrHtmlHistoryHandling_Push) ==
        PrStatus_Ok);
    CHECK(prHtmlWindowScrollY(window) == 0.0);
    prHtmlPageDestroy(page);
}

TEST_CASE("geometry reads lay out what changed, and a scroll's events fire at the next rendering update") {
    Page fonts("");
    SteppedClock clock;
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &clock;
    host.now = steppedNow;
    host.font = fonts.defaultFont;
    host.shaper = fonts.shaper;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(page, "<!doctype html><style>body { margin: 8px } div { height: 50px }"
        "#s { height: 100px; overflow: auto; border: 2px solid }#t { transform: translate(10px, 0); position: relative; top: 5px }"
        "</style><div id=a></div><div id=s><div></div><div></div><div id=in>"
        "</div></div><div id=t></div><div id=end style='height: 1000px'></div>", "about:blank", nullptr) ==
        PrStatus_Ok);
    PrHtmlWindow* window = prHtmlPageWindow(page);
    PrDomDocument* document = prHtmlWindowDocument(window);
    auto byId = [&](const char* id) {
        PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(document), id);
        REQUIRE(element != nullptr);
        return element;
    };
    auto html = [&](const char* id) {
        return prHtmlElementQuery(prDomNodeAsEventTarget(prDomElementAsNode(byId(id))));
    };
    clock.now = 20;
    runDue(page);

    // A read lays out first, so it sees a change made since the last update.
    prDomElementSetAttribute(byId("a"), "style", "height: 30px");
    PrDomRect a = prCssElementGetBoundingClientRect(byId("a"));
    CHECK(a.x == 8.0);
    CHECK(a.height == 30.0);
    CHECK(prHtmlElementOffsetTop(html("s")) == 38);
    CHECK(prCssElementClientTop(byId("s")) == 2);
    CHECK(prCssElementClientHeight(byId("s")) == 100);
    CHECK(prCssElementScrollHeight(byId("s")) == 150);
    // A transform moves the box as painted, and not as laid out.
    CHECK(prCssElementGetBoundingClientRect(byId("t")).x == 18.0);
    CHECK(prHtmlElementOffsetLeft(html("t")) == 8);
    CHECK(prHtmlElementOffsetTop(html("t")) == 147);

    // A scroll moves the content at once, within its range, and its events wait for the rendering update.
    struct Heard {
        uint32_t scrolls = 0;
        uint32_t ends = 0;
    } heard;
    PrDomEventListener scrolls{
        [](void* PR_NULLABLE c, PrDomEvent*) { ++static_cast<Heard*>(c)->scrolls; },
        &heard,
        1,
        nullptr,
        nullptr,
    };
    PrDomEventListener ends{
        [](void* PR_NULLABLE c, PrDomEvent*) { ++static_cast<Heard*>(c)->ends; },
        &heard,
        2,
        nullptr,
        nullptr,
    };
    PrDomEventTarget* scroller = prDomNodeAsEventTarget(prDomElementAsNode(byId("s")));
    prDomEventTargetAddEventListener(scroller, "scroll", &scrolls, nullptr);
    prDomEventTargetAddEventListener(scroller, "scrollend", &ends, nullptr);
    prCssElementSetScrollTop(byId("s"), 1000);
    CHECK(prCssElementScrollTop(byId("s")) == 50.0);
    CHECK(prCssElementGetBoundingClientRect(byId("in")).y == 90.0);
    PrCssScrollToOptions by{false, true, 0, -10};
    prCssElementScrollBy(byId("s"), &by);
    CHECK(heard.scrolls == 0);
    CHECK_FALSE(prDomAgentIsIdle(prHtmlPageAgent(page)));
    clock.now = 40;
    runDue(page);
    CHECK(heard.scrolls == 1);
    CHECK(heard.ends == 1);
    CHECK(prDomAgentIsIdle(prHtmlPageAgent(page)));

    // The root element's scrolling is the viewport's, which the window measures too.
    PrDomElement* root = prCssDocumentScrollingElement(document);
    CHECK(root == prDomDocumentElement(document));
    // The queries laid the document out on this thread, and no job's scene records it yet. A scroll needs no job, since
    // no scene holds an offset. The host takes the document's own offsets for its compositor, each container once, and
    // not the offsets that it set itself.
    PrCssPresentation* presentation = prHtmlPagePresentation(page);
    prHtmlPageSetViewport(page, PrVec2{200, 100});
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{200, 100}));
    CHECK_FALSE(prCssPresentationIsCurrent(presentation));
    auto job = [&] {
        prHtmlPresentationStart(presentation, prLayoutVecFromPx(PrVec2{200, 100}));
        prCssPresentationWait(presentation);
        prLayoutSceneRelease(prCssPresentationTakeScene(presentation));
    };
    job();
    CHECK(prCssPresentationIsCurrent(presentation));
    // A query that lays out nothing new leaves the scene current. One that lays out a change leaves it behind until a
    // job records it.
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{200, 100}));
    CHECK(prCssPresentationIsCurrent(presentation));
    prDomElementSetAttribute(byId("end"), "style", "height: 1200px");
    CHECK(prCssElementGetBoundingClientRect(byId("end")).height == 1200.0);
    CHECK_FALSE(prCssPresentationIsCurrent(presentation));
    job();
    CHECK(prCssPresentationIsCurrent(presentation));
    prDomElementSetAttribute(byId("end"), "style", "height: 1000px");
    job();
    CHECK(prCssPresentationIsCurrent(presentation));
    PrLayoutSceneScroll taken[2]{};
    CHECK(prCssPresentationTakeScrolls(presentation, taken, 2) == 1);
    CHECK(taken[0].node != kPrLayoutNodeNone);
    PrCssScrollToOptions to{false, true, 0, 50};
    prHtmlWindowScroll(window, &to);
    to.top = 100;
    prHtmlWindowScroll(window, &to);
    CHECK(prCssPresentationIsCurrent(presentation));
    REQUIRE(prCssPresentationTakeScrolls(presentation, taken, 2) == 1);
    CHECK(taken[0].node == kPrLayoutNodeNone);
    CHECK(taken[0].offset == prLayoutVecFromPx(PrVec2{0, 100}));
    CHECK(prCssPresentationTakeScrolls(presentation, taken, 2) == 0);
    prCssPresentationSetScroll(presentation, kPrLayoutNodeNone, prLayoutVecFromPx(PrVec2{0, 60}));
    CHECK(prCssPresentationTakeScrolls(presentation, taken, 2) == 0);
    prCssPresentationSetScroll(presentation, kPrLayoutNodeNone, prLayoutVecFromPx(PrVec2{0, 100}));
    CHECK(prHtmlWindowScrollY(window) == 100.0);
    CHECK(prCssElementScrollTop(root) == 100.0);
    CHECK(prCssElementGetBoundingClientRect(byId("a")).y == -92.0);
    CHECK(prCssDocumentElementFromPoint(document, 20, 10) == byId("in"));
    CHECK(prCssDocumentElementFromPoint(document, 20, 60) == byId("t"));

    // Scrolling into view puts the element at the start of each container around it.
    PrCssScrollIntoViewOptions start = prCssScrollIntoViewOptionsDefault();
    prCssElementScrollIntoView(byId("in"), &start);
    CHECK(prCssElementGetBoundingClientRect(byId("in")).y == 0.0);
    CHECK(prCssElementScrollTop(byId("s")) == 50.0);
    prHtmlPageDestroy(page);
}

TEST_CASE("a resize observer reports the boxes that changed, deeper ones in the same frame and the rest in the next") {
    Page fonts("");
    SteppedClock clock;
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &clock;
    host.now = steppedNow;
    host.font = fonts.defaultFont;
    host.shaper = fonts.shaper;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(page, "<!doctype html><div id=a style='width: 100px; height: 50px; padding: 5px 7px; border: 3px "
        "solid'><div id=in style='width: 50%; height: 10px'></div></div>"
        "<div id=b style='writing-mode: vertical-rl; width: 20px; height: 40px'></div>"
        "<span id=c>text</span>", "about:blank", nullptr) == PrStatus_Ok);
    PrHtmlWindow* window = prHtmlPageWindow(page);
    PrDomDocument* document = prHtmlWindowDocument(window);
    auto byId = [&](const char* id) {
        PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(document), id);
        REQUIRE(element != nullptr);
        return element;
    };
    // Each call's entries, and the box that the first call widens.
    struct Observed {
        std::vector<std::vector<PrCssResizeObserverEntry>> calls;
        PrDomElement* PR_NULLABLE widen = nullptr;
        uint32_t errors = 0;
    } observed;
    observed.widen = byId("a");
    PrCssResizeCallback callback{};
    callback.context = &observed;
    callback.invoke = [](void* PR_NULLABLE context, PrCssResizeObserver*, const PrCssResizeObserverEntry* entries,
            uint32_t count) {
        Observed& o = *static_cast<Observed*>(context);
        o.calls.emplace_back(entries, entries + count);
        if (o.widen != nullptr) {
            prDomElementSetAttribute(o.widen, "style", "width: 200px; height: 50px; padding: 5px 7px; border: 3px solid");
            o.widen = nullptr;
        }
    };
    PrDomEventListener errors{
        [](void* PR_NULLABLE c, PrDomEvent*) { ++static_cast<Observed*>(c)->errors; },
        &observed,
        1,
        nullptr,
        nullptr,
    };
    prDomEventTargetAddEventListener(prHtmlWindowAsEventTarget(window), "error", &errors, nullptr);
    PrCssResizeObserver* observer = prCssResizeObserverCreate(document, &callback);
    prCssResizeObserverObserve(observer, byId("a"), PrCssResizeObserverBox_ContentBox);
    prCssResizeObserverObserve(observer, byId("in"), PrCssResizeObserverBox_ContentBox);
    prCssResizeObserverObserve(observer, byId("b"), PrCssResizeObserverBox_BorderBox);
    prCssResizeObserverObserve(observer, byId("c"), PrCssResizeObserverBox_ContentBox);
    CHECK_FALSE(prDomAgentIsIdle(prHtmlPageAgent(page)));
    clock.now = 20;
    runDue(page);

    // The first frame reports every box, a box without one at zero. The box inside the one that the call widens is
    // deeper, and is reported again in the same frame. The widened box is not, and its loop error is reported instead.
    REQUIRE(observed.calls.size() == 2);
    REQUIRE(observed.calls[0].size() == 4);
    const PrCssResizeObserverEntry& a = observed.calls[0][0];
    CHECK(a.target == byId("a"));
    CHECK(a.contentRect.x == 7.0);
    CHECK(a.contentRect.y == 5.0);
    CHECK(a.contentRect.width == 100.0);
    CHECK(a.contentRect.height == 50.0);
    CHECK(a.contentBoxSize.inlineSize == 100.0);
    CHECK(a.contentBoxSize.blockSize == 50.0);
    CHECK(a.borderBoxSize.inlineSize == 120.0);
    CHECK(a.borderBoxSize.blockSize == 66.0);
    CHECK(a.devicePixelContentBoxSize.inlineSize == 100.0);
    CHECK(observed.calls[0][1].contentBoxSize.inlineSize == 50.0);
    // A vertical writing mode's inline axis is vertical.
    CHECK(observed.calls[0][2].borderBoxSize.inlineSize == 40.0);
    CHECK(observed.calls[0][2].borderBoxSize.blockSize == 20.0);
    CHECK(observed.calls[0][3].borderBoxSize.inlineSize == 0.0);
    REQUIRE(observed.calls[1].size() == 1);
    CHECK(observed.calls[1][0].target == byId("in"));
    CHECK(observed.calls[1][0].contentBoxSize.inlineSize == 100.0);
    CHECK(observed.errors == 1);

    // The next frame reports the box that was skipped.
    clock.now = 40;
    runDue(page);
    REQUIRE(observed.calls.size() == 3);
    REQUIRE(observed.calls[2].size() == 1);
    CHECK(observed.calls[2][0].target == byId("a"));
    CHECK(observed.calls[2][0].contentBoxSize.inlineSize == 200.0);
    CHECK(prDomAgentIsIdle(prHtmlPageAgent(page)));

    // A box that is no longer observed is not reported.
    prCssResizeObserverUnobserve(observer, byId("a"));
    prDomElementSetAttribute(byId("a"), "style", "width: 300px; height: 50px");
    clock.now = 60;
    runDue(page);
    REQUIRE(observed.calls.size() == 4);
    REQUIRE(observed.calls[3].size() == 1);
    CHECK(observed.calls[3][0].target == byId("in"));
    CHECK(observed.calls[3][0].contentBoxSize.inlineSize == 150.0);
    CHECK(observed.errors == 1);
    prCssResizeObserverDisconnect(observer);
    prCssResizeObserverRelease(observer);
    prDomEventTargetRemoveEventListener(prHtmlWindowAsEventTarget(window), "error", &errors, nullptr);
    prHtmlPageDestroy(page);
}

TEST_CASE("an intersection observer reports a target's crossings of its thresholds in a task after the update") {
    Page fonts("");
    SteppedClock clock;
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &clock;
    host.now = steppedNow;
    host.font = fonts.defaultFont;
    host.shaper = fonts.shaper;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(page, "<!doctype html><style>body { margin: 0 }</style>"
        "<div style='height: 650px'></div><div id=t style='width: 100px; height: 100px'></div>"
        "<div style='height: 2000px'></div>", "about:blank", nullptr) == PrStatus_Ok);
    PrHtmlWindow* window = prHtmlPageWindow(page);
    PrDomDocument* document = prHtmlWindowDocument(window);
    PrDomElement* target = prDomNodeGetElementById(prDomDocumentAsNode(document), "t");
    REQUIRE(target != nullptr);
    // The entries of each call.
    std::vector<std::vector<PrCssIntersectionObserverEntry>> calls;
    PrCssIntersectionCallback callback{};
    callback.context = &calls;
    callback.invoke = [](void* PR_NULLABLE context, PrCssIntersectionObserver* observer) {
        std::vector<std::vector<PrCssIntersectionObserverEntry>>& delivered =
            *static_cast<std::vector<std::vector<PrCssIntersectionObserverEntry>>*>(context);
        delivered.emplace_back();
        PrCssIntersectionObserverEntry entry{};
        while (prCssIntersectionObserverTakeRecords(observer, &entry, 1) != 0) {
            delivered.back().push_back(entry);
            prDomNodeRelease(prDomElementAsNode(entry.target));
        }
    };
    const double thresholds[] = {0.5, 0};
    PrCssIntersectionObserverInit init = prCssIntersectionObserverInitDefault();
    init.thresholds = thresholds;
    init.thresholdCount = 2;
    PrCssIntersectionObserver* observer = nullptr;
    REQUIRE(prCssIntersectionObserverCreate(document, &init, &callback, &observer) == PrStatus_Ok);
    uint32_t count = 0;
    const double* sorted = prCssIntersectionObserverThresholds(observer, &count);
    REQUIRE(count == 2);
    CHECK(sorted[0] == 0.0);
    CHECK(sorted[1] == 0.5);
    PrRawString margin = prCssIntersectionObserverRootMargin(observer);
    CHECK(std::string(prRawStringView(&margin).data, prRawStringView(&margin).size) == "0px 0px 0px 0px");
    prRawStringReset(&margin);
    prCssIntersectionObserverObserve(observer, target);
    CHECK_FALSE(prDomAgentIsIdle(prHtmlPageAgent(page)));
    clock.now = 20;
    runDue(page);

    // The first update reports the target below the viewport, which does not intersect it.
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0].size() == 1);
    const PrCssIntersectionObserverEntry& below = calls[0][0];
    CHECK_FALSE(below.isIntersecting);
    CHECK(below.intersectionRatio == 0.0);
    CHECK(below.boundingClientRect.y == 650.0);
    CHECK(below.rootBounds.height == 600.0);

    // A scroll that shows half of the target crosses both thresholds at once, which one entry reports.
    PrCssScrollToOptions half{false, true, 0, 100};
    prCssElementScroll(prDomDocumentElement(document), &half);
    clock.now = 40;
    runDue(page);
    REQUIRE(calls.size() == 2);
    REQUIRE(calls[1].size() == 1);
    CHECK(calls[1][0].isIntersecting);
    CHECK(calls[1][0].intersectionRatio == 0.5);
    CHECK(calls[1][0].intersectionRect.height == 50.0);

    // A scroll within the same thresholds reports nothing.
    PrCssScrollToOptions more{false, true, 0, 120};
    prCssElementScroll(prDomDocumentElement(document), &more);
    clock.now = 60;
    runDue(page);
    CHECK(calls.size() == 2);

    // A target observed once the page is idle keeps it from idling until its first update is delivered.
    REQUIRE(prDomAgentIsIdle(prHtmlPageAgent(page)));
    PrCssIntersectionObserver* later = nullptr;
    REQUIRE(prCssIntersectionObserverCreate(document, &init, &callback, &later) == PrStatus_Ok);
    prCssIntersectionObserverObserve(later, target);
    CHECK_FALSE(prDomAgentIsIdle(prHtmlPageAgent(page)));
    clock.now = 80;
    runDue(page);
    REQUIRE(calls.size() == 3);
    REQUIRE(calls[2].size() == 1);
    CHECK(calls[2][0].isIntersecting);
    CHECK(prDomAgentIsIdle(prHtmlPageAgent(page)));
    prCssIntersectionObserverDisconnect(later);
    prCssIntersectionObserverRelease(later);
    prCssIntersectionObserverDisconnect(observer);
    prCssIntersectionObserverRelease(observer);
    prHtmlPageDestroy(page);
}

TEST_CASE("an idle period before a requested frame leaves no more than a frame interval") {
    Page fonts("");
    SteppedClock clock;
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &clock;
    host.now = steppedNow;
    host.font = fonts.defaultFont;
    host.shaper = fonts.shaper;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(page, "<!doctype html>", "about:blank", nullptr) == PrStatus_Ok);
    PrHtmlWindow* window = prHtmlPageWindow(page);
    PrHtmlFrameRequestCallback frame{};
    frame.invoke = [](void* PR_NULLABLE, double) {};
    prHtmlWindowRequestAnimationFrame(window, &frame);
    // A frame at 50 ms is one whose time plus the frame interval rounds up, past the interval from it.
    clock.now = 50;
    runDue(page);
    prHtmlWindowRequestAnimationFrame(window, &frame);
    struct Idle {
        PrHtmlWindow* window;
        double remaining = -1;
    } idle{window};
    PrHtmlIdleRequestCallback callback{};
    callback.context = &idle;
    callback.invoke = [](void* PR_NULLABLE context, double deadline, bool) {
        Idle* idle = static_cast<Idle*>(context);
        idle->remaining = prHtmlWindowIdleTimeRemaining(idle->window, deadline);
    };
    prHtmlWindowRequestIdleCallback(window, &callback, 0);
    runDue(page);
    CHECK(idle.remaining > 0);
    CHECK(idle.remaining <= 1000.0 / 60);
    prHtmlPageDestroy(page);
}

TEST_CASE("an infinite animation keeps its page from idling until it starts") {
    Page fonts("");
    SteppedClock clock;
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &clock;
    host.now = steppedNow;
    host.font = fonts.defaultFont;
    host.shaper = fonts.shaper;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(page, "<style>@keyframes k { to { opacity: 0 } }"
        "#a { animation: k 1s linear infinite 100ms }</style><div id=a></div>", "about:blank", nullptr) == PrStatus_Ok);
    clock.now = 20;
    runDue(page);
    CHECK_FALSE(prDomAgentIsIdle(prHtmlPageAgent(page)));
    clock.now = 200;
    runDue(page);
    CHECK(prDomAgentIsIdle(prHtmlPageAgent(page)));
    prHtmlPageDestroy(page);
}

TEST_CASE("transforms compute in 3D, with the individual transforms, percentages and the reference box") {
    Page page("<style>body { margin: 0 } div { width: 100px; height: 50px }</style>"
        "<div id=p style='transform: translate(50%, 10px)'></div>"
        "<div id=i style='translate: 10px 20px; rotate: 90deg; scale: 2; transform: translateX(5px)'></div>"
        "<div id=r style='rotate: y 60deg'></div>"
        "<div id=z style='transform: perspective(100px) translateZ(10px); transform-origin: 0 0 5px;"
        " transform-box: content-box'></div><div id=q style='transform: translate(50%) perspective(100px)'></div><div id=n></div>");
    // A percentage is a fraction of the reference box, which painting applies.
    PrLayoutEffectStyle p = page.effects("p");
    CHECK(double(p.transform.perWidth[0]) == doctest::Approx(0.5));
    CHECK(double(p.transform.matrix.m[1][3]) == doctest::Approx(10));
    // The individual transforms apply before transform, in the order translate, rotate, scale.
    PrLayoutTransform individual = page.effects("i").transform;
    PrMatrix4 i = prLayoutTransformResolve(&individual, PrVec2{100, 50});
    CHECK(double(i.m[0][3]) == doctest::Approx(10));
    CHECK(double(i.m[1][3]) == doctest::Approx(30));
    CHECK(double(i.m[1][0]) == doctest::Approx(2));
    CHECK(page.box("i").transformed == 1);
    CHECK(page.box("r").transformed == 1);
    CHECK(page.box("n").transformed == 0);
    PrMatrix4 r = page.effects("r").transform.matrix;
    CHECK(double(r.m[0][0]) == doctest::Approx(0.5));
    CHECK(double(r.m[2][0]) == doctest::Approx(-0.866).epsilon(0.001));
    PrLayoutEffectStyle z = page.effects("z");
    CHECK(double(z.transform.matrix.m[3][2]) == doctest::Approx(-0.01));
    CHECK(double(z.originZ) == doctest::Approx(5));
    CHECK(z.transformBox == PrLayoutTransformBox_ContentBox);
    CHECK(page.effects("n").transformBox == PrLayoutTransformBox_ViewBox);
    // A percentage to the left of a perspective counts as zero.
    CHECK(double(page.effects("q").transform.perWidth[0]) == 0);
}

TEST_CASE("perspective, its origin and backface-visibility project onto the effects group") {
    Page page("<style>body { margin: 0 } div { width: 200px; height: 100px }</style>"
        "<div id=p style='perspective: 50px; perspective-origin: right 20% bottom 10px'></div>"
        "<div id=z style='perspective: 0; perspective-origin: top; backface-visibility: hidden;"
        " transform: rotateY(10deg)'></div><div id=b style='backface-visibility: hidden'></div>"
        "<div id=t style='transform-origin: bottom'></div><div id=n></div>");
    // A perspective makes a containing block for all descendants, as a transform does.
    CHECK(page.box("p").transformed == 1);
    CHECK(page.box("n").transformed == 0);
    PrLayoutEffectStyle p = page.effects("p");
    CHECK(double(p.perspective) == doctest::Approx(50));
    // An offset from the far edge is 100% less the offset.
    CHECK(p.perspectiveOriginX.bits == prLayoutLengthPercent(80).bits);
    CHECK(!p.backfaceHidden);
    // A perspective below 1px renders as 1px.
    PrLayoutEffectStyle z = page.effects("z");
    CHECK(double(z.perspective) == doctest::Approx(1));
    CHECK(z.perspectiveOriginX.bits == prLayoutLengthPercent(50).bits);
    CHECK(z.perspectiveOriginY.bits == prLayoutLengthPercent(0).bits);
    CHECK(z.backfaceHidden);
    // A box that no transform turns cannot show its back, and keeps the default group.
    CHECK(!page.effects("b").backfaceHidden);
    PrLayoutEffectStyle n = page.effects("n");
    CHECK(double(n.perspective) == 0);
    CHECK(n.perspectiveOriginX.bits == prLayoutLengthPercent(50).bits);
    // The resolved values of the origins are pixels from the reference box's top left.
    CHECK(resolved(page, "p", PrCssProperty_PerspectiveOrigin) == "160px 90px");
    CHECK(resolved(page, "n", PrCssProperty_PerspectiveOrigin) == "100px 50px");
    CHECK(resolved(page, "z", PrCssProperty_Perspective) == "0px");
    CHECK(resolved(page, "z", PrCssProperty_BackfaceVisibility) == "hidden");
    CHECK(resolved(page, "t", PrCssProperty_TransformOrigin) == "100px 100px");
}

TEST_CASE("a composited transform keeps the individual transforms before it") {
    Page page("<style>#m { width: 100px; height: 50px; translate: 10px; transition: transform 1s linear }"
        "#m.on { transform: translateX(50%) }</style><div id=m></div>");
    frame(page, 0);
    prDomElementSetAttribute(byId(page, "m"), "class", "on");
    frame(page, 0);
    PrLayoutTrack track{};
    REQUIRE(prCssPresentationTracks(page.presentation, &track, 1) == 1);
    float opacity = 1;
    PrMatrix4 matrix = prMatrix4Identity();
    REQUIRE(prLayoutTrackSample(&track, 500, PrVec2{100, 50}, &opacity, &matrix));
    CHECK(double(matrix.m[0][3]) == doctest::Approx(35));
}

TEST_CASE("transitions and animations fire their events at the rendering updates where their phases change") {
    Page fonts("");
    SteppedClock clock;
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &clock;
    host.now = steppedNow;
    host.font = fonts.defaultFont;
    host.shaper = fonts.shaper;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(page, "<style>#t { width: 10px; transition: width 100ms linear 50ms }"
        "#t.wide { width: 20px } @keyframes k { to { opacity: 0 } }"
        "#a { animation: k 100ms linear 2 }</style><div id=t></div><div id=a></div>", "about:blank", nullptr) ==
        PrStatus_Ok);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    std::string log;
    PrDomEventListener listener{
        [](void* data, PrDomEvent* event) {
            std::string* log = static_cast<std::string*>(data);
            PrStringView type = prDomEventType(event);
            *log += std::string(type.data, type.size);
            if (PrCssTransitionEvent* transition = prCssTransitionEventQuery(event))
                *log += "@" + std::to_string(int(prCssTransitionEventElapsedTime(transition) * 1000));
            if (PrCssAnimationEvent* animation = prCssAnimationEventQuery(event))
                *log += "@" + std::to_string(int(prCssAnimationEventElapsedTime(animation) * 1000));
            *log += " ";
        },
        &log,
        1,
        nullptr,
        nullptr,
    };
    PrDomEventTarget* target = prDomNodeAsEventTarget(prDomDocumentAsNode(document));
    for (const char* type : {"transitionrun", "transitionstart", "transitionend", "transitioncancel", "animationstart",
        "animationiteration", "animationend", "animationcancel"})
        prDomEventTargetAddEventListener(target, type, &listener, nullptr);
    PrDomElement* t = prDomNodeGetElementById(prDomDocumentAsNode(document), "t");

    // A rendering update computes the styles that start the animation and, once its class changes, the transition. Each
    // sends its events from the following update.
    clock.now = 20;
    runDue(page);
    CHECK(log.empty());
    prDomElementSetAttribute(t, "class", "wide");
    clock.now = 40;
    runDue(page);
    CHECK(log == "animationstart@0 ");
    log.clear();
    clock.now = 60;
    runDue(page);
    CHECK(log == "transitionrun@0 ");
    log.clear();
    clock.now = 100;
    runDue(page);
    CHECK(log == "transitionstart@0 ");
    log.clear();
    clock.now = 140;
    runDue(page);
    CHECK(log == "animationiteration@100 ");
    log.clear();
    clock.now = 200;
    runDue(page);
    CHECK(log == "transitionend@100 ");
    log.clear();
    clock.now = 240;
    runDue(page);
    CHECK(log == "animationend@200 ");

    // A transition whose property stops transitioning before it ends is canceled.
    log.clear();
    prDomElementSetAttribute(t, "class", "");
    clock.now = 260;
    runDue(page);
    clock.now = 280;
    runDue(page);
    CHECK(log == "transitionrun@0 ");
    log.clear();
    prDomElementSetAttribute(t, "style", "transition: none");
    clock.now = 300;
    runDue(page);
    clock.now = 320;
    runDue(page);
    CHECK(log == "transitioncancel@0 ");
    prHtmlPageDestroy(page);
}

TEST_CASE("an SVG document lays out its root svg element, a replaced box, as the root of its layout") {
    Page fonts("");
    PrHtmlHost host = prHtmlHostDefault();
    host.font = fonts.defaultFont;
    host.shaper = fonts.shaper;
    PrHtmlResponseHeaders svg = {.contentType = "image/svg+xml"};
    const char* source = "<svg xmlns='http://www.w3.org/2000/svg'><rect width='10' height='10'/></svg>";
    PrHtmlPage* whole = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(whole, source, "about:blank", &svg) == PrStatus_Ok);
    PrCssPresentation* presentation = prHtmlPagePresentation(whole);
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(verified(presentation));
    prHtmlPageDestroy(whole);

    PrHtmlPage* streamed = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageBeginLoad(streamed, "about:blank", &svg) == PrStatus_Ok);
    prHtmlPageAppendLoad(streamed, source);
    prHtmlPageEndLoad(streamed);
    runDue(streamed);
    presentation = prHtmlPagePresentation(streamed);
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(verified(presentation));
    prHtmlPageDestroy(streamed);
}

TEST_CASE("a document streamed in pieces renders as it arrives, once its head is past") {
    Page fonts("");
    SteppedClock clock;
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &clock;
    host.now = steppedNow;
    host.font = fonts.defaultFont;
    host.shaper = fonts.shaper;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    // The response names its charset, so the parse starts with the first piece instead of waiting for the bytes that
    // the prescan reads.
    PrHtmlResponseHeaders utf8 = {.contentType = "text/html; charset=utf-8"};
    REQUIRE(prHtmlPageBeginLoad(page, "about:blank", &utf8) == PrStatus_Ok);
    PrCssPresentation* presentation = prHtmlPagePresentation(page);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));

    // The head may yet add a sheet, so the document waits to render.
    prHtmlPageAppendLoad(page, "<!doctype html><html><head><style>p { margin: 0; height: 10px }");
    runDue(page);
    CHECK(prHtmlPageIsRenderBlocked(page));
    clock.now = 100;
    CHECK(prDomAgentNextTaskDelay(prHtmlPageAgent(page)) == -1);
    prHtmlPageAppendLoad(page, "</style></head><body><p id=first>one</p>");
    runDue(page);
    CHECK_FALSE(prHtmlPageIsRenderBlocked(page));
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    PrDomElement* first = prDomNodeGetElementById(prDomDocumentAsNode(document), "first");
    CHECK(prCssPresentationLayoutNode(presentation, prDomElementAsNode(first)) != kPrLayoutNodeNone);

    // A large piece parses a slice per task, and each update styles what the slices added.
    std::string many;
    for (uint32_t i = 0; i < 2000; ++i)
        many += "<p>paragraph</p>";
    prHtmlPageAppendLoad(page, many);
    CHECK(prDomAgentRunTask(prHtmlPageAgent(page)));
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    uint32_t styled = prCssPresentationStats(presentation).elementsStyled;
    CHECK(styled > 100);
    CHECK(styled < 2000);
    CHECK(prDomAgentNextTaskDelay(prHtmlPageAgent(page)) == 0);
    runDue(page);
    prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(prCssPresentationStats(presentation).elementsStyled == 2000 - styled);
    CHECK(verified(presentation));

    // The end of the source stops the parse, and the document completes.
    prHtmlPageEndLoad(page);
    runDue(page);
    CHECK(prHtmlDocumentReadyState(document) == PrHtmlDocumentReadyState_Complete);
    prHtmlPageDestroy(page);
}

TEST_CASE("text that a streamed load appends to a text node reaches the presentation") {
    Page fonts("");
    PrHtmlHost host = prHtmlHostDefault();
    host.font = fonts.defaultFont;
    host.shaper = fonts.shaper;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    // The response names its charset, so that the parse does not wait for the first kilobyte that the prescan reads.
    PrHtmlResponseHeaders headers{};
    headers.contentType = "text/html; charset=utf-8";
    REQUIRE(prHtmlPageBeginLoad(page, "about:blank", &headers) == PrStatus_Ok);
    PrCssPresentation* presentation = prHtmlPagePresentation(page);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));

    // Each piece ends inside the paragraph's text, so the parser appends each later piece to the text node that the
    // first made, and the presentation lays out between the pieces.
    std::string source = "<!doctype html><body><p id=t>The page is complete</p>";
    for (size_t at = 0; at < source.size(); at += 3) {
        prHtmlPageAppendLoad(page, source.substr(at, 3));
        runDue(page);
        prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    }
    PrDomElement* t = prDomNodeGetElementById(prDomDocumentAsNode(document), "t");
    REQUIRE(t != nullptr);
    PrLayoutNode box = prCssPresentationLayoutNode(presentation, prDomElementAsNode(t));
    REQUIRE(box != kPrLayoutNodeNone);
    CHECK(boxesUnder(prCssPresentationTree(presentation), box, " ") == "The page is complete");
    CHECK(verified(presentation));
    prHtmlPageEndLoad(page);
    runDue(page);
    prHtmlPageDestroy(page);
}

TEST_CASE("a load restyles on one thread, and a change once the load is in divides its subtrees among a pool's threads") {
    Page fonts("");
    HostPool pool(4);
    PrDomAgentOptions options = withPool(pool);
    PrDomAgent* agent = prDomAgentCreate(&options);
    PrHtmlHost host = prHtmlHostDefault();
    host.font = fonts.defaultFont;
    host.shaper = fonts.shaper;
    host.agent = agent;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    // The response names its charset, so the parse starts with the first piece instead of waiting for the bytes that
    // the prescan reads.
    PrHtmlResponseHeaders utf8 = {.contentType = "text/html; charset=utf-8"};
    REQUIRE(prHtmlPageBeginLoad(page, "about:blank", &utf8) == PrStatus_Ok);
    PrCssPresentation* presentation = prHtmlPagePresentation(page);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    std::string many = "<!doctype html><html><head><style>.dark * { color: white }</style></head><body>";
    for (uint32_t i = 0; i < 2000; ++i)
        many += "<p>paragraph</p>";
    prHtmlPageAppendLoad(page, many);
    runDue(page);
    prHtmlPresentationStart(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    prCssPresentationWait(presentation);

    // A sheet that arrives as the document loads restyles every element, on this thread alone.
    prHtmlPageAppendLoad(page, "<style>p { margin: 0 }</style><p>last</p>");
    runDue(page);
    prHtmlPresentationStart(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    prCssPresentationWait(presentation);
    CHECK(prCssPresentationStats(presentation).elementsStyled > 2000);
    CHECK(prCssPresentationStats(presentation).elementsStyledAhead == 0);
    CHECK(pool.loops == 0);

    // Once the document is in, a change that restyles a whole subtree is someone's to wait for.
    prHtmlPageEndLoad(page);
    runDue(page);
    REQUIRE(prHtmlDocumentReadyState(document) == PrHtmlDocumentReadyState_Complete);
    prHtmlPresentationStart(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    prCssPresentationWait(presentation);
    PrDomElement* body = prDomDocumentBody(document);
    REQUIRE(body != nullptr);
    prDomElementSetAttribute(body, "class", "dark");
    prHtmlPresentationStart(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    prCssPresentationWait(presentation);
    CHECK(prCssPresentationStats(presentation).elementsStyledAhead > 2000);
    CHECK(pool.widest > 1);
    CHECK(verified(presentation));
    prHtmlPageDestroy(page);
    prDomAgentDestroy(agent);
}

TEST_CASE("without a pool, a restyle runs on the agent's thread alone, whatever number of threads it asks for") {
    std::string html = "<style>.dark * { color: white }</style><div id=top>";
    for (uint32_t i = 0; i < 2000; ++i)
        html += "<span>x</span>";
    html += "</div>";
    PrDomAgentOptions options = prDomAgentOptionsDefault();
    options.restyleThreads = 4;
    Page page(html.c_str(), PrVec2{800, 600}, options);
    prDomElementSetAttribute(byId(page, "top"), "class", "dark");
    PrCssPresentationStats stats = update(page);
    CHECK(stats.elementsStyled == 2000);
    CHECK(stats.elementsStyledAhead == 0);
}

TEST_CASE("a restyle divides its work among no more threads than the agent asks for, nor than its pool has") {
    std::string html = "<style>.dark * { color: white }</style><div id=top>";
    for (uint32_t i = 0; i < 2000; ++i)
        html += "<span>x</span>";
    html += "</div>";
    HostPool wide(8);
    PrDomAgentOptions options = withPool(wide);
    options.restyleThreads = 2;
    Page limited(html.c_str(), PrVec2{800, 600}, options);
    HostPool narrow(3);
    Page pooled(html.c_str(), PrVec2{800, 600}, withPool(narrow));
    for (Page* page : {&limited, &pooled}) {
        prDomElementSetAttribute(byId(*page, "top"), "class", "dark");
        CHECK(update(*page).elementsStyledAhead == 2000);
    }
    CHECK(wide.widest == 2);
    CHECK(narrow.widest == 3);
}

TEST_CASE("a change under a :has() anchor restyles the anchor, not the document") {
    Page page("<style>.card { color: rgb(0, 0, 0) } .card:has(.flag) { color: rgb(255, 0, 0) }"
        ".row:has(+ .picked) { width: 10px }</style>"
        "<div class=card id=a><p><span id=s>x</span></p></div><div class=card id=b><p>y</p></div>"
        "<div class=row id=r1></div><div class=row id=r2></div><div id=other><p>z</p></div>");
    // A class deep in an anchor restyles the anchor, and what inherits its color.
    prDomElementSetAttribute(byId(page, "s"), "class", "flag");
    PrCssPresentationStats stats = update(page);
    CHECK(stats.elementsStyled == 3);
    CHECK(prColorFromColor16(page.color("a").color).r == 1.0f);
    CHECK(prColorFromColor16(page.color("b").color).r == 0.0f);
    // An element added under an anchor, and taken away again.
    PrDomElement* added = nullptr;
    REQUIRE(prDomElementCreate(page.document, "i", nullptr, &added) == PrStatus_Ok);
    prDomElementSetAttribute(added, "class", "flag");
    prDomNodeAppendChild(prDomElementAsNode(byId(page, "b")), prDomElementAsNode(added));
    update(page);
    CHECK(prColorFromColor16(page.color("b").color).r == 1.0f);
    prDomNodeRemove(prDomElementAsNode(added));
    update(page);
    CHECK(prColorFromColor16(page.color("b").color).r == 0.0f);
    // An argument that reaches siblings anchors at the earlier sibling.
    prDomElementSetAttribute(byId(page, "r2"), "class", "row picked");
    stats = update(page);
    CHECK(px(page.box("r1").width) == 10.0f);
    CHECK(stats.elementsStyled <= 3);
}

TEST_CASE("a width of min() over a percentage resolves against each containing block") {
    Page page("<div style='width: 300px'><div id=a style='width: min(50%, 100px); height: 1px'></div></div>"
        "<div style='width: 150px'><div id=b style='width: calc(50% + 10px); height: 1px'></div></div>"
        "<div style='width: 150px'><div id=c style='width: clamp(10px, 50%, 200px); height: 1px'></div></div>");
    CHECK(page.bounds("a").right - page.bounds("a").left == 100.0f);
    CHECK(page.bounds("b").right - page.bounds("b").left == 85.0f);
    CHECK(page.bounds("c").right - page.bounds("c").left == 75.0f);
}

TEST_CASE("visibility: collapse projects as a box that collapses, and as hidden for painting") {
    Page page("<div id=c style='visibility: collapse'><span id=inherits></span></div><div id=h style='visibility: "
        "hidden'></div><div id=v></div>");
    CHECK(page.box("c").collapsed == 1);
    CHECK(page.box("inherits").collapsed == 1);
    CHECK(page.box("h").collapsed == 0);
    CHECK(page.box("v").collapsed == 0);
    CHECK(page.color("c").visibility == PrLayoutVisibility_Hidden);
}

TEST_CASE("aspect-ratio projects as a ratio of width over height, with whether auto was given with it") {
    Page page("<div id=a style='aspect-ratio: 16 / 9'></div><div id=b style='aspect-ratio: 2 auto'></div><div id=c "
        "style='aspect-ratio: auto'></div><div id=d style='aspect-ratio: 0 / 1'></div><div id=e></div>");
    CHECK(page.box("a").aspectRatio == doctest::Approx(16.0 / 9.0));
    CHECK(page.box("a").aspectRatioAuto == 0);
    CHECK(page.box("b").aspectRatio == 2.0f);
    CHECK(page.box("b").aspectRatioAuto == 1);
    CHECK(page.box("c").aspectRatio == 0.0f);
    // A degenerate ratio is auto.
    CHECK(page.box("d").aspectRatio == 0.0f);
    CHECK(page.box("d").aspectRatioAuto == 0);
    CHECK(page.box("e").aspectRatio == 0.0f);
}

TEST_CASE("a button is a semi-replaced element, a flow root or inline-block by button layout") {
    Page page("<button id=a style='all: unset; display: block'></button><button id=b style='all: unset'></button>"
        "<button id=c style='display: flex'></button><div id=d></div>");
    CHECK(page.box("a").display == PrLayoutDisplay_FlowRoot);
    CHECK(page.box("a").width.bits == prLayoutLengthAuto().bits);
    CHECK(page.box("b").display == PrLayoutDisplay_InlineBlock);
    CHECK(page.box("c").display == PrLayoutDisplay_Flex);
    PrLayoutTree* tree = prCssPresentationTree(page.presentation);
    CHECK(prLayoutNodeKind(tree, page.node("a")) == PrLayoutNodeKind_SemiReplaced);
    CHECK(prLayoutNodeKind(tree, page.node("c")) == PrLayoutNodeKind_SemiReplaced);
    CHECK(prLayoutNodeKind(tree, page.node("d")) == PrLayoutNodeKind_Element);
}

TEST_CASE("justify-self and justify-items project onto the alignment fields") {
    Page page("<div id=p style='justify-items: legacy center'><div id=a style='justify-self: safe end'></div>"
        "<div id=b></div></div>");
    CHECK(page.flex("a").justifySelf == (PrLayoutAlignment_End | PrLayoutAlignmentFlag_Safe));
    CHECK(page.flex("p").justifyItems == PrLayoutAlignment_Center);
    CHECK(page.flex("b").justifySelf == PrLayoutAlignment_Auto);
}

TEST_CASE("a block-level button is as wide as its content, and its ratio sizes it from its minimum height") {
    // A button's auto inline size is its fit-content size, so in a stretched flex item its block size transfers from
    // the width its min-height gives it, not from the item's.
    Page page("<div style='display: flex'><div id=item><div style='height: 50px; width: 100px'></div><button id=b "
        "style='all: unset; display: block; height: 100%; aspect-ratio: 1; min-height: 50px'></button></div></div>"
        "<button id=w style='all: unset; display: block'><span style='display: inline-block; width: 40px; "
        "height: 10px'></span></button>");
    CHECK(page.bounds("item").bottom - page.bounds("item").top == 100.0f);
    CHECK(page.bounds("b").right - page.bounds("b").left == 100.0f);
    CHECK(page.bounds("w").right - page.bounds("w").left == 40.0f);
}

TEST_CASE("object-fit projects onto the decoration group") {
    Page page("<img id=a style='object-fit: contain'><img id=b style='object-fit: scale-down'><img id=c "
        "style='object-fit: cover'><img id=d>");
    CHECK(page.decoration("a").objectFit == PrLayoutObjectFit_Contain);
    CHECK(page.decoration("b").objectFit == PrLayoutObjectFit_ScaleDown);
    CHECK(page.decoration("c").objectFit == PrLayoutObjectFit_Cover);
    CHECK(page.decoration("d").objectFit == PrLayoutObjectFit_Fill);
}

TEST_CASE("background layers project in order, each taking its item of every background list property") {
    Page page("<div id=a style='background: linear-gradient(90deg, red, blue) 10px 20% / 30px auto no-repeat "
        "content-box padding-box, conic-gradient(from 90deg at 0 0, white 0 25%, black 0 50%) repeat-x, "
        "lime'></div><div id=b style='background-image: radial-gradient(circle 10px at right, red, blue)'></div><div id=c></div>");
    PrLayoutTree* tree = prCssPresentationTree(page.presentation);
    PrLayoutDecorationStyle a = page.decoration("a");
    CHECK(sameColor(a.background, PrColor{0, 1, 0, 1}));
    // The final layer, which sets the color, has no image, and keeps its place.
    REQUIRE(prLayoutBackgroundLayerCount(tree, a.backgroundLayers) == 3);
    CHECK(prLayoutBackgroundLayer(tree, a.backgroundLayers, 2).kind == PrLayoutImageKind_None);
    PrLayoutBackgroundLayer top = prLayoutBackgroundLayer(tree, a.backgroundLayers, 0);
    CHECK(top.kind == PrLayoutImageKind_Gradient);
    CHECK(top.gradient.kind == PrLayoutGradientKind_Linear);
    CHECK(top.gradient.angle == 90.0f);
    // Legacy colors interpolate in sRGB.
    CHECK(top.gradient.space == PrColorSpace_Srgb);
    CHECK(top.gradient.stopCount == 2);
    CHECK(sameColor(top.gradient.stops[1].color, PrColor{0, 0, 1, 1}));
    CHECK(top.positionX.bits == prLayoutLengthPx(10).bits);
    CHECK(top.positionY.bits == prLayoutLengthPercent(20).bits);
    CHECK(top.sizeX.bits == prLayoutLengthPx(30).bits);
    CHECK(top.sizeY.bits == prLayoutLengthAuto().bits);
    CHECK(top.repeatX == PrLayoutBackgroundRepeat_NoRepeat);
    CHECK(top.repeatY == PrLayoutBackgroundRepeat_NoRepeat);
    CHECK(top.origin == PrLayoutBackgroundBox_ContentBox);
    CHECK(top.clip == PrLayoutBackgroundBox_PaddingBox);
    PrLayoutBackgroundLayer bottom = prLayoutBackgroundLayer(tree, a.backgroundLayers, 1);
    CHECK(bottom.gradient.kind == PrLayoutGradientKind_Conic);
    CHECK(bottom.gradient.angle == 90.0f);
    CHECK(bottom.gradient.centerX.bits == prLayoutLengthPx(0).bits);
    CHECK(bottom.gradient.stopCount == 4);
    CHECK(bottom.gradient.stops[1].position.bits == prLayoutLengthPercent(25).bits);
    CHECK(bottom.repeatX == PrLayoutBackgroundRepeat_Repeat);
    CHECK(bottom.repeatY == PrLayoutBackgroundRepeat_NoRepeat);
    // The color takes the bottom layer's clip.
    CHECK(a.backgroundClip == PrLayoutBackgroundBox_BorderBox);

    PrLayoutBackgroundLayer radial = prLayoutBackgroundLayer(tree, page.decoration("b").backgroundLayers, 0);
    CHECK(radial.gradient.kind == PrLayoutGradientKind_Radial);
    CHECK(radial.gradient.shape == PrLayoutRadialShape_Circle);
    CHECK(radial.gradient.extent == PrLayoutRadialExtent_Radii);
    CHECK(radial.gradient.radiusX.bits == prLayoutLengthPx(10).bits);
    CHECK(radial.gradient.centerX.bits == prLayoutLengthPercent(100).bits);
    CHECK(page.decoration("c").backgroundLayers == kPrLayoutBackgroundNone);
}

TEST_CASE("background-blend-mode gives each layer its blend mode, repeating its list over the layers") {
    Page page("<div id=a style='background-image: linear-gradient(red, red), linear-gradient(blue, blue), "
        "linear-gradient(lime, lime); background-blend-mode: multiply, plus-lighter'></div>"
        "<div id=b style='background-image: linear-gradient(red, red)'></div>");
    PrLayoutTree* tree = prCssPresentationTree(page.presentation);
    PrLayoutBackgroundIndex a = page.decoration("a").backgroundLayers;
    REQUIRE(prLayoutBackgroundLayerCount(tree, a) == 3);
    CHECK(prLayoutBackgroundLayer(tree, a, 0).blendMode == PrDrawBlendMode_Multiply);
    // plus-lighter is an operator, and the list repeats for the third layer.
    CHECK(prLayoutBackgroundLayer(tree, a, 1).blendMode == PrDrawBlendMode_Normal);
    CHECK(prLayoutBackgroundLayer(tree, a, 1).composite == PrDrawComposite_PlusLighter);
    CHECK(prLayoutBackgroundLayer(tree, a, 2).blendMode == PrDrawBlendMode_Multiply);
    PrLayoutBackgroundLayer b = prLayoutBackgroundLayer(tree, page.decoration("b").backgroundLayers, 0);
    CHECK(b.blendMode == PrDrawBlendMode_Normal);
    CHECK(b.composite == PrDrawComposite_SourceOver);
}

TEST_CASE("filter() images project as their input filtered by the lists of each filter() around it, innermost first") {
    Page page("<div id=a style='background-image: filter(filter(linear-gradient(red, blue), sepia(1)), "
        "invert(1) blur(2px))'></div>"
        "<div id=b style='background-image: filter(linear-gradient(red, blue), url(#missing) invert(1))'></div>"
        "<div id=c style='background-image: filter(url(missing.png), invert(1))'></div>"
        "<div id=d style='background-image: filter(linear-gradient(red, blue), grayscale() drop-shadow(1px 2px))'></div>");
    PrLayoutTree* tree = prCssPresentationTree(page.presentation);
    PrLayoutBackgroundLayer a = prLayoutBackgroundLayer(tree, page.decoration("a").backgroundLayers, 0);
    CHECK(a.kind == PrLayoutImageKind_Gradient);
    REQUIRE(a.filterCount == 3);
    CHECK(a.filters[0].kind == PrLayoutFilterKind_Sepia);
    CHECK(a.filters[1].kind == PrLayoutFilterKind_Invert);
    CHECK(a.filters[2].kind == PrLayoutFilterKind_Blur);
    CHECK(a.filters[2].amount == 2.0f);
    // A URL that names no filter element leaves the list filtering nothing.
    PrLayoutBackgroundLayer b = prLayoutBackgroundLayer(tree, page.decoration("b").backgroundLayers, 0);
    CHECK(b.kind == PrLayoutImageKind_Gradient);
    CHECK(b.filterCount == 0);
    // An input that cannot be displayed draws nothing.
    PrLayoutBackgroundLayer c = prLayoutBackgroundLayer(tree, page.decoration("c").backgroundLayers, 0);
    CHECK(c.kind == PrLayoutImageKind_None);
    // The list computes as the filter property's does: omitted arguments take their defaults.
    CHECK(resolved(page, "d", PrCssProperty_BackgroundImage) ==
        "filter(linear-gradient(red, blue), grayscale(1) drop-shadow(currentcolor 1px 2px 0px))");
}

TEST_CASE("light-dark() of images picks the image or none of the element's used color scheme") {
    Page page("");
    PrHtmlHost host = prHtmlHostDefault();
    host.font = page.defaultFont;
    host.shaper = page.shaper;
    PrHtmlPage* htmlPage = prHtmlPageCreate(&host);
    REQUIRE(prHtmlPageLoad(htmlPage, "<style>div { background-image: light-dark(linear-gradient(red, blue), none), "
        "light-dark(none, radial-gradient(red, blue)) }</style><div id=light style='color-scheme: light'></div>"
        "<div id=dark style='color-scheme: dark'></div>"
        "<div id=both style='color-scheme: light dark'></div>", "about:blank", nullptr) == PrStatus_Ok);
    PrCssPresentation* presentation = prHtmlPagePresentation(htmlPage);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(htmlPage));
    // Returns the kinds of the element's two layers.
    auto kinds = [&](const char* id) {
        PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(document), id);
        REQUIRE(element != nullptr);
        PrLayoutTree* tree = prCssPresentationTree(presentation);
        PrLayoutNode node = prCssPresentationLayoutNode(presentation, prDomElementAsNode(element));
        PrLayoutStyle record = prLayoutStyleRecord(tree, prLayoutNodeStyle(tree, node));
        PrLayoutBackgroundIndex layers =
            prLayoutDecorationStyleRecord(tree, record.groups[PrLayoutGroup_Decoration]).backgroundLayers;
        REQUIRE(prLayoutBackgroundLayerCount(tree, layers) == 2);
        return std::pair<PrLayoutImageKind, PrLayoutImageKind>{
            prLayoutBackgroundLayer(tree, layers, 0).kind,
            prLayoutBackgroundLayer(tree, layers, 1).kind,
        };
    };
    auto prefer = [&](PrAppColorScheme scheme) {
        PrAppUserPreferences preferences{.colorScheme = scheme};
        prHtmlPageSetUserPreferences(htmlPage, &preferences);
        prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(PrVec2{800, 600}));
    };
    using Kinds = std::pair<PrLayoutImageKind, PrLayoutImageKind>;
    const Kinds lightKinds{PrLayoutImageKind_Gradient, PrLayoutImageKind_None};
    const Kinds darkKinds{PrLayoutImageKind_None, PrLayoutImageKind_Gradient};
    prefer(PrAppColorScheme_Light);
    CHECK(kinds("light") == lightKinds);
    CHECK(kinds("dark") == darkKinds);
    CHECK(kinds("both") == lightKinds);
    // The preference reaches the element that allows both schemes, which alone projects again.
    prefer(PrAppColorScheme_Dark);
    CHECK(prCssPresentationStats(presentation).elementsStyled == 1);
    CHECK(kinds("both") == darkKinds);
    CHECK(kinds("light") == lightKinds);
    CHECK(verified(presentation));
    prHtmlPageDestroy(htmlPage);
}

TEST_CASE("a gradient interpolates in Oklab unless its colors are all legacy, and a missing component takes its neighbor's") {
    Page page("<div id=a style='background-image: linear-gradient(red, color(srgb 0 0 1))'></div>"
        "<div id=b style='background-image: linear-gradient(in oklch longer hue, red, blue)'></div>"
        "<div id=c style='background-image: linear-gradient(in srgb, color(srgb 1 0 0), "
        "color(srgb none 0.5 0.5), color(srgb 0 0 1))'></div>");
    PrLayoutTree* tree = prCssPresentationTree(page.presentation);
    CHECK(prLayoutBackgroundLayer(tree, page.decoration("a").backgroundLayers, 0).gradient.space == PrColorSpace_Oklab);
    PrLayoutGradient b = prLayoutBackgroundLayer(tree, page.decoration("b").backgroundLayers, 0).gradient;
    CHECK(b.space == PrColorSpace_Oklch);
    CHECK(b.hueMethod == PrColorHueMethod_Longer);
    // The middle stop's red is 1 toward the first stop, and 0 toward the last: two stops at one position.
    PrLayoutGradient c = prLayoutBackgroundLayer(tree, page.decoration("c").backgroundLayers, 0).gradient;
    REQUIRE(c.stopCount == 4);
    CHECK(sameColor(c.stops[1].color, PrColor{1, 0.5f, 0.5f, 1}));
    CHECK(c.stops[2].position.bits == prLayoutLengthNone().bits);
    CHECK(sameColor(c.stops[2].color, PrColor{0, 0.5f, 0.5f, 1}));
}

TEST_CASE("a gradient's stop keeps a hue that it states in the interpolation space, even where it is powerless") {
    Page page("<div id=a style='background-image: linear-gradient(in hsl, hsl(30 0% 100%), blue)'></div>"
        "<div id=b style='background-image: linear-gradient(in oklch, hsl(30 0% 100%), blue)'></div>"
        "<div id=c style='background-image: linear-gradient(in hsl, red, white)'></div>");
    PrLayoutTree* tree = prCssPresentationTree(page.presentation);
    PrLayoutGradient a = prLayoutBackgroundLayer(tree, page.decoration("a").backgroundLayers, 0).gradient;
    REQUIRE(a.stopCount == 2);
    CHECK(a.stops[0].hasHue);
    CHECK(a.stops[0].hue == 30.0f);
    CHECK(a.stops[1].hasHue);
    CHECK(a.stops[1].hue == doctest::Approx(240));
    // A conversion to another space makes a powerless hue missing, so it takes the hue of the stop that it mixes with.
    PrLayoutGradient b = prLayoutBackgroundLayer(tree, page.decoration("b").backgroundLayers, 0).gradient;
    REQUIRE(b.stopCount == 2);
    CHECK(b.stops[0].hasHue);
    CHECK(b.stops[0].hue == b.stops[1].hue);
    // White converted to HSL is within rounding of full lightness, which leaves its hue powerless.
    PrLayoutGradient c = prLayoutBackgroundLayer(tree, page.decoration("c").backgroundLayers, 0).gradient;
    REQUIRE(c.stopCount == 2);
    CHECK(c.stops[1].hue == c.stops[0].hue);
}

TEST_CASE("the root takes the body's background layers when its own background is empty") {
    Page page("<body id=body style='background: linear-gradient(red, blue)'>");
    PrLayoutTree* tree = prCssPresentationTree(page.presentation);
    PrDomElement* root = prDomDocumentElement(page.document);
    PrLayoutNode rootNode = prCssPresentationLayoutNode(page.presentation, prDomElementAsNode(root));
    PrLayoutStyle rootStyle = prLayoutStyleRecord(tree, prLayoutNodeStyle(tree, rootNode));
    PrLayoutDecorationStyle rootDecoration =
        prLayoutDecorationStyleRecord(tree, rootStyle.groups[PrLayoutGroup_Decoration]);
    CHECK(prLayoutBackgroundLayerCount(tree, rootDecoration.backgroundLayers) == 1);
    CHECK(page.decoration("body").backgroundLayers == kPrLayoutBackgroundNone);
}

TEST_CASE("system colors and light-dark() resolve in the element's color scheme") {
    Page page("<div id=light style='background: Canvas; color: light-dark(red, blue)'></div>"
        "<div id=dark style='color-scheme: dark; background: Canvas; color: light-dark(red, blue)'></div>"
        "<div id=both style='color-scheme: light dark; background: CanvasText'></div>"
        "<div id=accent style='accent-color: rgb(255 128 0); background: AccentColor; "
        "border: solid ThreeDDarkShadow'></div><mark id=mark>x</mark>");
    CHECK(sameColor(page.decoration("light").background, PrColor{1, 1, 1, 1}));
    CHECK(sameColor(page.color("light").color, PrColor{1, 0, 0, 1}));
    CHECK(sameColor(page.color("dark").color, PrColor{0, 0, 1, 1}));
    CHECK(prFloat16ToFloat(page.decoration("dark").background.r) < 0.2f);
    // With both schemes, the user's preference, which is light, decides.
    CHECK(sameColor(page.decoration("both").background, PrColor{0, 0, 0, 1}));
    CHECK(sameColor(page.decoration("accent").background, PrColor{1, 0.5f, 0, 1}));
    // A deprecated system color stands for the one it computes to.
    CHECK(sameColor(page.decoration("accent").borderColor[PrRectSide_Top], PrColor{0.463f, 0.463f, 0.463f, 1}));
    CHECK(sameColor(page.decoration("mark").background, PrColor{1, 1, 0, 1}));
}

TEST_CASE("color functions resolve: color-mix(), relative colors, contrast-color() and device-cmyk()") {
    Page page("<div id=mix style='background: color-mix(in srgb, red, blue)'></div>"
        "<div id=share style='background: color-mix(in srgb, red 20%, blue 20%)'></div>"
        "<div id=three style='background: color-mix(in srgb, red, lime, blue)'></div>"
        "<div id=relative style='background: rgb(from red r 255 b / calc(alpha / 2))'></div>"
        "<div id=hue style='background: hsl(from red calc(h + 120) s l)'></div>"
        "<div id=contrast style='background: contrast-color(navy)'></div>"
        "<div id=cmyk style='background: device-cmyk(0 0.5 1 0)'></div><div id=none style='background: rgb(0 0 255 / none)'></div>"
        "<div id=parent style='color: rgb(0 0 255)'><div id=child style='border: solid; border-color: "
        "color-mix(in srgb, currentcolor, white)'></div></div>");
    CHECK(sameColor(page.decoration("mix").background, PrColor{0.5f, 0, 0.5f, 1}));
    // Percentages that sum below 100% scale the alpha.
    CHECK(sameColor(page.decoration("share").background, PrColor{0.5f, 0, 0.5f, 0.4f}));
    CHECK(sameColor(page.decoration("three").background, PrColor{1.0f / 3, 1.0f / 3, 1.0f / 3, 1}));
    CHECK(sameColor(page.decoration("relative").background, PrColor{1, 1, 0, 0.5f}));
    CHECK(sameColor(page.decoration("hue").background, PrColor{0, 1, 0, 1}));
    CHECK(sameColor(page.decoration("contrast").background, PrColor{1, 1, 1, 1}));
    CHECK(sameColor(page.decoration("cmyk").background, PrColor{1, 0.5f, 0, 1}));
    // A missing alpha draws as zero.
    CHECK(prFloat16ToFloat(page.decoration("none").background.a) == 0.0f);
    // currentcolor outside the color property resolves where it is used.
    CHECK(sameColor(page.decoration("child").borderColor[PrRectSide_Top], PrColor{0.5f, 0.5f, 1, 1}));
}

TEST_CASE("outline and background-clip project onto the decoration group, an outline of style none with no width") {
    Page page("<div id=a style='outline: 3px dashed blue; outline-offset: 2px; background-clip: content-box'></div>"
        "<div id=b style='outline-style: auto; outline-width: 1px; outline-offset: -0.1px'></div><div id=c "
        "style='outline-width: 4px'></div>");
    CHECK(page.decoration("a").outlineStyle == PrDrawBorderStyle_Dashed);
    CHECK(page.decoration("a").outlineWidth.raw == prLayoutUnitFromPx(3).raw);
    CHECK(page.decoration("a").outlineOffset.raw == prLayoutUnitFromPx(2).raw);
    CHECK(sameColor(page.decoration("a").outlineColor, PrColor{0, 0, 1, 1}));
    CHECK(page.decoration("a").backgroundClip == PrLayoutBackgroundBox_ContentBox);
    CHECK(page.decoration("b").outlineStyle == PrDrawBorderStyle_Solid);
    // The offset computes unsnapped, and layout snaps it as a border width, as it does the outline's width.
    CHECK(page.decoration("b").outlineOffset.raw == prLayoutUnitFromPx(-0.1f).raw);
    CHECK(page.decoration("c").outlineStyle == PrDrawBorderStyle_None);
    CHECK(page.decoration("c").outlineWidth.raw == 0);
    CHECK(page.decoration("c").backgroundClip == PrLayoutBackgroundBox_BorderBox);
}

TEST_CASE("a calculation that folds into a length inside a function leaves the function's computed value whole") {
    Page page("<!doctype html><style>#t { font-size: 40px }</style><div id=t></div>");
    prDomElementSetAttribute(byId(page, "t"), "style",
        "background-image: radial-gradient(circle calc(-0.5em + 10px) at calc(-1em + 10px) calc(-2em + 10px), red, blue)");
    update(page);
    std::string value = resolved(page, "t", PrCssProperty_BackgroundImage);
    CHECK(value.find("at -30px -70px, ") != std::string::npos);
    CHECK(value.back() == ')');
}

TEST_CASE("an element that loses its box before its children's boxes are made gives them none, and gives them boxes "
        "again once it has a box") {
    // A query restyles the page before its first update, which queues the boxes of the div's text.
    Page page("<!doctype html><div id=div>hello world</div>");
    prCssPresentationDestroy(page.presentation);
    uint32_t sheetCount = 0;
    const PrCssUserAgentSheet* sheets = prHtmlUserAgentSheets(&sheetCount);
    PrCssPresentationDesc desc{
        .document = page.document,
        .font = page.defaultFont,
        .shaper = page.shaper,
        .agent = page.agent,
        .userAgentSheets = sheets,
        .userAgentSheetCount = sheetCount,
    };
    page.presentation = prCssPresentationCreate(&desc);
    prDomElementSetAttribute(byId(page, "div"), "hidden", "until-found");
    resolved(page, "div", PrCssProperty_Display);
    prDomElementSetAttribute(byId(page, "div"), "hidden", "");
    update(page);
    prDomElementRemoveAttribute(byId(page, "div"), "hidden");
    update(page);
    CHECK(prLayoutNodeFirstChild(prCssPresentationTree(page.presentation), page.node("div")) != kPrLayoutNodeNone);
}

TEST_CASE("an attribute that changes while its element is out of the document gives the element its hints again") {
    Page page("<!doctype html><svg><g id=g><rect id=r width=50 height=10 /></g></svg>");
    update(page);
    CHECK(resolved(page, "r", PrCssProperty_Width) == "50px");
    PrDomElement* rect = byId(page, "r");
    PrDomNode* group = prDomElementAsNode(byId(page, "g"));
    prDomNodeRetain(prDomElementAsNode(rect));
    REQUIRE(prDomNodeRemoveChild(group, prDomElementAsNode(rect)) == PrStatus_Ok);
    prDomElementSetAttribute(rect, "width", "100");
    REQUIRE(prDomNodeAppendChild(group, prDomElementAsNode(rect)) == PrStatus_Ok);
    prDomNodeRelease(prDomElementAsNode(rect));
    update(page);
    CHECK(resolved(page, "r", PrCssProperty_Width) == "100px");
    CHECK(verified(page.presentation));
}

TEST_CASE("a CSS animation in a marker's content moves the marker frame by frame") {
    std::pair<std::string, std::string> painted = paintedAtTwoTimes(
        "<!doctype html><style>.turn { animation: turn 4s linear infinite }"
            " @keyframes turn { to { transform: rotate(90deg) } }</style>"
            "<svg width=0 height=0><marker id=m viewBox='-10 -10 20 20' markerWidth=20 markerHeight=20"
            " markerUnits=userSpaceOnUse><rect id=r class=turn x=-6 y=-6 width=12 height=12 /></marker></svg>"
            "<svg width=160 height=60><path d='M 20 30 H 140' stroke=black marker-start='url(#m)' /></svg>");
    CHECK(painted.first != painted.second);
}

TEST_CASE("a CSS animation of an instance's transform in a use element's shadow tree moves it frame by frame") {
    // An instance root's parent is the use's shadow root, but in the flat tree it is the use: SVG content, which no
    // compositor runs, so the presentation applies the animation.
    std::pair<std::string, std::string> painted = paintedAtTwoTimes(
        "<!doctype html><style>.move { animation: move 4s linear infinite }"
            " @keyframes move { to { transform: translateX(80px) } }</style>"
            "<svg width=160 height=60><defs><rect id=r class=move x=20 y=20 width=20 height=20 /></defs><use href='#r' /></svg>");
    CHECK(painted.first != painted.second);
}

TEST_CASE("a host shows its shadow tree, a slot its assigned nodes, and a child that no slot takes has no box") {
    Page page("<div id=host><span id=named slot=x>a</span><span id=plain>b</span></div>");
    PrDomShadowRoot* shadow = attachShadow(page, "host", "<p id=inner>i</p><slot name=x></slot>");
    update(page);
    PrLayoutTree* tree = prCssPresentationTree(page.presentation);
    PrLayoutNode inner = prCssPresentationLayoutNode(page.presentation, prDomElementAsNode(inShadow(shadow, "#inner")));
    CHECK(prLayoutNodeFirstChild(tree, page.node("host")) == inner);
    CHECK(prLayoutNodeParent(tree, page.node("named")) == page.node("host"));
    CHECK(page.node("plain") == kPrLayoutNodeNone);

    // Naming a slot that the tree lacks takes the element's box away, and adding that slot gives it back. A default
    // slot takes the child that names none.
    prDomElementSetAttribute(byId(page, "named"), "slot", "y");
    update(page);
    CHECK(page.node("named") == kPrLayoutNodeNone);
    prHtmlSetInnerHtml(prDomShadowRootAsNode(shadow), "<slot name=y></slot><slot></slot>");
    update(page);
    CHECK(page.node("named") != kPrLayoutNodeNone);
    CHECK(page.node("plain") != kPrLayoutNodeNone);
}

TEST_CASE("a slot shows its children while nothing is assigned to it, and a slotted element inherits from its slot") {
    Page page("<div id=host style='color: red'><span id=light slot=x>a</span></div>");
    PrDomShadowRoot* shadow =
        attachShadow(page, "host", "<style>slot { color: rgb(0, 128, 0) }</style><slot><em id=fallback>f</em></slot>"
            "<slot name=x></slot>");
    update(page);
    CHECK(resolvedOf(byId(page, "light"), PrCssProperty_Color) == "rgb(0, 128, 0)");
    CHECK(prCssPresentationLayoutNode(page.presentation, prDomElementAsNode(inShadow(shadow,
        "#fallback"))) != kPrLayoutNodeNone);
    // Assigning a node to the slot replaces its fallback content.
    prDomElementSetAttribute(byId(page, "light"), "slot", "");
    update(page);
    CHECK(prCssPresentationLayoutNode(page.presentation, prDomElementAsNode(inShadow(shadow,
        "#fallback"))) == kPrLayoutNodeNone);
    CHECK(page.node("light") != kPrLayoutNodeNone);
}

TEST_CASE("a tree's rules match its own elements, its host through :host and its slotted elements through ::slotted") {
    Page page("<style>p { width: 1px } #host { width: 10px } #host { height: 1px }</style>"
        "<div id=host class=on><span id=light class=s></span></div><p id=outer></p>");
    PrDomShadowRoot* shadow = attachShadow(page, "host", "<style>p { width: 2px } :host { width: 20px; height: 5px !important }"
        ":host(.on) p { height: 3px } ::slotted(.s) { display: block; width: 4px }"
        "</style><p id=inner></p><slot></slot>");
    update(page);
    CHECK(px(page.box("outer").width) == 1.0f);
    PrDomElement* inner = inShadow(shadow, "#inner");
    CHECK(resolvedOf(inner, PrCssProperty_Width) == "2px");
    CHECK(resolvedOf(inner, PrCssProperty_Height) == "3px");
    // The host's own normal rule wins over its :host rule, and the :host rule's important declaration wins
    // (css-cascade-5/#cascade-context).
    CHECK(px(page.box("host").width) == 10.0f);
    CHECK(resolvedOf(byId(page, "host"), PrCssProperty_Height) == "5px");
    CHECK(resolvedOf(byId(page, "light"), PrCssProperty_Width) == "4px");
    // A change to the host's class reaches the shadow tree's elements through :host().
    prDomElementSetAttribute(byId(page, "host"), "class", "");
    update(page);
    CHECK(resolvedOf(inner, PrCssProperty_Height) != "3px");
}

TEST_CASE("a sheet's default namespace does not keep :host from matching its featureless host, but :host(*) obeys it") {
    Page page("<div id=bare></div><div id=argument></div>");
    attachShadow(page, "bare", "<style>@namespace url(http://www.w3.org/2000/svg); :host { width: 20px }</style>");
    attachShadow(page, "argument", "<style>@namespace url(http://www.w3.org/2000/svg); :host(*) { width: 20px }</style>");
    update(page);
    CHECK(resolvedOf(byId(page, "bare"), PrCssProperty_Width) == "20px");
    CHECK(resolvedOf(byId(page, "argument"), PrCssProperty_Width) != "20px");
}

TEST_CASE("an inherited group whose declarations restate its initial values is the initial group") {
    using namespace pr_impl;
    Page page("<style>.reset { font: 12px serif } .tabular { font-variant-numeric: tabular-nums }"
        ".normal { font-kerning: auto; font-variant-caps: initial }</style>"
        "<p id=a class=reset>a</p><p id=b class=tabular>b</p><p id=c class=normal>c</p>");
    auto group = [&](const char* id) {
        PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(page.document), id);
        const CssComputedStyle* style = cssPresentationStyle(page.presentation, element);
        REQUIRE(style != nullptr);
        return style->groups[CssStyleGroup_Typography];
    };
    CHECK(group("a") == group("c"));
    CHECK(group("b") != group("a"));
}

TEST_CASE("ex, cap, ch and ic measure the element's font, and their root forms the root's") {
    // Ahem's x-height and cap height are 0.8em, and every glyph is an em square. It has no water ideograph, and no face
    // of the page has one, so ic is an em.
    Page page("<html style='font: 10px Ahem'><body style='font: 20px Tinos'>"
        "<div id=a style='font: 20px Ahem; width: 2ex; height: 3cap'></div>"
        "<div id=b style='font: 20px Ahem; width: 3ch; height: 1ic'></div><div id=c style='width: 1rch; height: 2rex'></div>"
        "<div id=d style='font: 20px Ahem; writing-mode: vertical-rl; text-orientation: upright; width: 1ch; "
        "height: 1ch'></div><div id=e style='width: 2ex'></div>");
    auto size = [&](const char* id) {
        PrRect r = page.bounds(id);
        return PrVec2{r.right - r.left, r.bottom - r.top};
    };
    CHECK(size("a").x == 32);
    CHECK(size("a").y == 48);
    CHECK(size("b").x == 60);
    CHECK(size("b").y == 20);
    CHECK(size("c").x == 10);
    CHECK(size("c").y == 16);
    CHECK(size("d").x == 20);
    CHECK(size("d").y == 20);
    // Tinos measures its own x-height, which is not half its em.
    CHECK(size("e").x > 15);
    CHECK(size("e").x < 25);
    CHECK(size("e").x != 20);
}

TEST_CASE("a positioned inline box in a multi-column container, holding boxes out of flow, paints") {
    Page page(R"(<style>
        section { width: 300px; height: 100px; border: blue 1px solid }
        .columns { column-width: 100px; column-fill: auto; height: 100px }
        inline-block { display: inline-block }
        .cb { position: relative }
        abs { position: absolute; background: purple; width: 100px; height: 50px }
        abs.p0 { top: 0; left: 0; background: blue }
    </style><section>123 <span class="cb">456<br><inline-block>7 <abs></abs><abs class="p0"></abs></inline-block>
    </span></section><section class="columns">123 <span class="cb">456<br><inline-block>7 <abs></abs>
    <abs class="p0"></abs></inline-block></span></section>)");
    frame(page, 0);
}

TEST_CASE("letter-spacing and word-spacing add their lengths after each character and each space") {
    // Every Ahem glyph is an em square.
    Page page("<div style='font: 10px Ahem'><span id=a style='letter-spacing: 2px'>abc</span><br>"
        "<span id=b style='word-spacing: 50%'>a b</span><br><span id=c style='letter-spacing: 0.5em'>ab</span><br>"
        "<span id=d style='letter-spacing: normal; word-spacing: normal'>a b</span></div>");
    auto width = [&](const char* id) {
        PrRect r = page.bounds(id);
        return r.right - r.left;
    };
    CHECK(width("a") == 36);
    CHECK(width("b") == 35);
    CHECK(width("c") == 30);
    CHECK(width("d") == 30);
}
