// Font loading through a browsing context
//
// css-font-loading-3 - https://drafts.csswg.org/css-font-loading-3/

#include "pr/base64.h"
#include "pr/compress_builtin.h"
#include "pr/css_presentation.h"
#include "pr/font_set.h"
#include "pr/html_page.h"
#include "pr/layout_style.h"
#include "pr/layout_tree.h"
#include "pr/text_harfbuzz.h"

#include "css/src/css_presentation.h"
#include "css/src/css_root.h"
#include "html/tests/support/html_test_fetch.h"

#include "tools/harness/test.h"

#include <cstdio>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

// Returns the bytes of the checkout's font file `name`, or an empty string if it cannot be read.
std::string readFont(const std::string& name) {
    std::string path = std::string(PR_FONTS_DIR) + "/" + name;
    std::string bytes;
    if (FILE* file = std::fopen(path.c_str(), "rb")) {
        char buffer[65536];
        size_t read = 0;
        while ((read = std::fread(buffer, 1, sizeof(buffer), file)) != 0)
            bytes.append(buffer, read);
        std::fclose(file);
    }
    return bytes;
}

// Returns a font as a url() of a data: URL.
std::string fontUrl() {
    std::string bytes = readFont("HandwritingCff2.otf");
    std::string encoded(prBase64EncodedSize(bytes.size()), '\0');
    prBase64Encode(bytes, encoded.data(), encoded.size());
    return "url(data:font/otf;base64," + encoded + ")";
}

// Returns the text of a view. The null view reads as the empty string.
std::string textOf(PrStringView text) {
    return std::string(text.size != 0 ? text.data : "", text.size);
}

std::string take(PrRawString string) {
    PrStringView view = prRawStringView(&string);
    std::string text(view.data != nullptr ? view.data : "", view.size);
    prRawStringReset(&string);
    return text;
}

// The notifications from the font callback and the set's events, in order.
struct Heard {
    std::vector<std::string> said;
};

// The network of the test's Served, which runTasks runs with the agent's tasks.
PrNet* PR_NULLABLE gServedNet = nullptr;

// Runs the network and the tasks that are due, until neither has anything to run.
void runTasks(PrHtmlPage* page) {
    for (bool ran = true; ran;) {
        ran = gServedNet != nullptr && prNetNullRun(gServedNet);
        while (prDomAgentNextTaskDelay(prHtmlPageAgent(page)) == 0) {
            prDomAgentRunTask(prHtmlPageAgent(page));
            ran = true;
        }
    }
}

PrDomEventListener recorder(Heard* heard) {
    return PrDomEventListener{
        [](void* context, PrDomEvent* event) {
            PrCssFontFaceSetLoadEvent* load = prCssFontFaceSetLoadEventQuery(event);
            uint32_t faces = load != nullptr ? prCssFontFaceSetLoadEventFontFaces(load, nullptr, 0) : 0;
            static_cast<Heard*>(context)->said.push_back(textOf(prDomEventType(event)) + " " + std::to_string(faces));
        },
        heard,
        0,
        nullptr,
        nullptr,
    };
}

// A browsing context whose host serves the checkout's font files by the last segment of their URL. It renders in a font
// set with Tinos as serif and as the default, and Arimo as sans-serif.
struct Served {
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextHarfBuzzCreate();
    PrCompressCodec* compress = prCompressBuiltinCreate();
    PrTextFont* defaultFont = nullptr;
    std::vector<std::string> fetched;      // the files fetched, in order
    bool hold = false;                     // if true, fetches wait until release() answers them
    TestFetcher fetcher;

    Served() {
        gServedNet = fetcher.net;
        fetcher.answer = [this](PrFetchInterceptRequest* request) {
            CHECK(request->desc.destination == PrFetchDestination_Font);
            CHECK(request->desc.mode == PrFetchMode_Cors);
            fetched.push_back(nameOf(request));
            if (!hold)
                answer(request);
        };
        for (const char* file : {"Tinos.ttf", "Arimo.ttf"}) {
            std::string path = std::string(PR_FONTS_DIR) + "/" + file;
            const PrFont* font = nullptr;
            REQUIRE(prFontCreateFromFile(path, 0, nullptr, &font) == PrStatus_Ok);
            PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
            prFontSetAdd(fonts, &descriptor, font);
            prFontRelease(font);
        }
        prFontSetSetGeneric(fonts, PrFontGeneric_Serif, "Tinos");
        prFontSetSetGeneric(fonts, PrFontGeneric_SansSerif, "Arimo");
        prFontSetSetDefaultFamily(fonts, prFontFamilyGeneric(PrFontGeneric_Serif));
        PrTextFontDesc defaultFontDesc{.set = fonts, .style = prFontStyleDefault()};
        defaultFont = prTextFontCreate(&defaultFontDesc);
    }

    ~Served() {
        release();
        gServedNet = nullptr;
        prTextFontRelease(defaultFont);
        prCompressCodecDestroy(compress);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
    }

    // Returns the file that a request names: the last segment of its URL's path.
    static std::string nameOf(const PrFetchInterceptRequest* request) {
        std::string name = TestFetcher::urlOf(request);
        name = name.substr(0, name.find('?'));
        return name.substr(name.rfind('/') + 1);
    }

    // Answers a request with the font file that it names, or with a 404 if there is none.
    void answer(PrFetchInterceptRequest* request) {
        std::string bytes = readFont(nameOf(request));
        fetcher.respondTo(request, bytes, bytes.empty() ? 404 : 200);
    }

    // Answers the first `count` fetches held so far, in the order in which they were requested, or every fetch.
    void release(size_t count = SIZE_MAX) {
        prNetNullRun(fetcher.net);
        size_t taken = count < fetcher.held.size() ? count : fetcher.held.size();
        std::vector<PrFetchInterceptRequest*> answering(fetcher.held.begin(), fetcher.held.begin() + long(taken));
        for (PrFetchInterceptRequest* request : answering)
            answer(request);
    }

    // Creates a browsing context that has loaded `html`. If `decompress` is false, the host gives no codec. If
    // `presentation` is false, it gives no font or shaper, so the document has no presentation.
    PrHtmlPage* load(const char* html, bool decompress = true, bool presentation = true) {
        PrHtmlHost host = prHtmlHostDefault();
        host.context = this;
        host.font = presentation ? defaultFont : nullptr;
        host.shaper = presentation ? shaper : nullptr;
        host.compress = decompress ? compress : nullptr;
        host.fetcher = fetcher.fetcher;
        PrHtmlPage* page = prHtmlPageCreate(&host);
        PrHtmlResponseHeaders headers = {.contentType = "text/html; charset=utf-8"};
        REQUIRE(prHtmlPageLoad(page, html, "https://fonts.test/page.html", &headers) == PrStatus_Ok);
        return page;
    }
};

// Lays out the document, and then lays it out again while its faces load, until no load is left to wait for.
void settle(PrHtmlPage* page) {
    for (int i = 0; i < 4; ++i) {
        prHtmlPresentationUpdate(prHtmlPagePresentation(page), prLayoutVecFromPx(PrVec2{800, 600}));
        runTasks(page);
    }
}

// Returns the width of the element with `id`, in CSS px.
float widthOf(PrHtmlPage* page, const char* id) {
    PrCssPresentation* presentation = prHtmlPagePresentation(page);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(document), id);
    REQUIRE(element != nullptr);
    PrLayoutNode node = prCssPresentationLayoutNode(presentation, prDomElementAsNode(element));
    PrRect bounds = prLayoutRectToPx(prLayoutNodeBounds(prCssPresentationTree(presentation), node,
        PrLayoutCoordinates_Viewport));
    return bounds.right - bounds.left;
}

// Spans of the same text in the family `web`, in serif and in sans-serif.
constexpr const char kSpans[] = "<style>span { display: inline-block }</style>"
    "<span id=web style='font-family: Web, serif'>Hamburgefonstiv</span>"
    "<span id=serif style='font-family: serif'>Hamburgefonstiv</span>"
    "<span id=sans style='font-family: sans-serif'>Hamburgefonstiv</span>";

// Records the set's status as the font callback reports it, and its load events. It holds a reference to the document,
// so that it stops listening whether or not the browsing context outlives it.
struct SetHeard {
    Heard heard;
    PrDomEventListener listener = recorder(&heard);
    PrDomDocument* document;

    explicit SetHeard(PrHtmlPage* page) : document(prHtmlWindowDocument(prHtmlPageWindow(page))) {
        prDomNodeRetain(prDomDocumentAsNode(document));
        PrCssFontCallback callback = {
            nullptr,
            [](void* context, PrDomDocument*, PrCssFontFaceSet* set) {
                bool loading = prCssFontFaceSetStatus(set) == PrCssFontFaceSetLoadStatus_Loading;
                static_cast<Heard*>(context)->said.push_back(loading ? "set loading" : "set loaded");
            },
            &heard,
        };
        prCssDocumentSetFontCallback(document, &callback);
        for (PrStringView type : {"loadingdone", "loadingerror"})
            prDomEventTargetAddEventListener(prCssFontFaceSetAsEventTarget(prCssDocumentFonts(document)), type,
                &listener, nullptr);
    }

    ~SetHeard() {
        prCssDocumentSetFontCallback(document, nullptr);
        for (PrStringView type : {"loadingdone", "loadingerror"})
            prDomEventTargetRemoveEventListener(prCssFontFaceSetAsEventTarget(prCssDocumentFonts(document)), type,
                &listener, nullptr);
        prDomNodeRelease(prDomDocumentAsNode(document));
    }
};

// Returns a face of family B that the test loads itself, whose fetch the host holds.
PrCssFontFace* loadingFace(PrHtmlPage* page) {
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    PrCssFontFace* face = prCssFontFaceCreate("B", "url(Arimo.ttf)", nullptr);
    REQUIRE(prCssFontFaceSetAdd(prCssDocumentFonts(document), face) == PrStatus_Ok);
    CHECK(prCssFontFaceLoad(face, document) == PrCssFontFaceStatus_Loading);
    runTasks(page);
    return face;
}

void update(PrHtmlPage* page) {
    prHtmlPresentationUpdate(prHtmlPagePresentation(page), prLayoutVecFromPx(PrVec2{800, 600}));
}

// Brings the presentation up to date and checks it against a full recomputation.
void verifyUpdate(PrHtmlPage* page) {
    update(page);
    PrRawString report = prRawStringNull();
    bool same = prCssPresentationVerify(prHtmlPagePresentation(page), &report);
    if (!same)
        MESSAGE(take(report));
    else
        prRawStringReset(&report);
    CHECK(same);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a style sheet's @font-face rules are the document's faces, before those added") {
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    std::string source = std::string("<style>@font-face { font-family: A; src: ") + fontUrl() +
        "; } @layer x { @font-face { font-family: B; src: url(b) } } "
        "@media print { @font-face { font-family: C; src: url(c) } }</style>";
    CHECK(prHtmlPageLoad(page, source, "about:blank", nullptr) == PrStatus_Ok);
    PrCssFontFaceSet* fonts = prCssDocumentFonts(prHtmlWindowDocument(prHtmlPageWindow(page)));
    // A face in a conditional group rule is left to the cascade.
    REQUIRE(prCssFontFaceSetSize(fonts) == 2);
    PrCssFontFace* first = prCssFontFaceSetAt(fonts, 0);
    CHECK(prCssFontFaceIsCssConnected(first));
    CHECK(take(prCssFontFaceDescriptor(first, PrCssFontFaceDescriptor_Family)) == "A");
    CHECK(take(prCssFontFaceDescriptor(prCssFontFaceSetAt(fonts, 1), PrCssFontFaceDescriptor_Family)) == "B");

    PrCssFontFace* added = prCssFontFaceCreate("D", "url(d)", nullptr);
    CHECK(prCssFontFaceSetAdd(fonts, added) == PrStatus_Ok);
    CHECK(prCssFontFaceSetAt(fonts, 2) == added);
    CHECK(prCssFontFaceSetAdd(fonts, first) == PrStatus_NotAllowed);
    CHECK_FALSE(prCssFontFaceSetDelete(fonts, first));
    prCssFontFaceSetClear(fonts);
    CHECK(prCssFontFaceSetSize(fonts) == 2);
    CHECK_FALSE(prCssFontFaceSetHas(fonts, added));
    prCssFontFaceRelease(added);
    prHtmlPageDestroy(page);
}

TEST_CASE("a face loads in a task, and its set switches to loading and back") {
    Heard heard;
    PrCssFontCallback callback = {
        [](void* context, PrDomDocument*, PrCssFontFace* face) {
            constexpr const char* kStatuses[] = {"unloaded", "loading", "loaded", "error"};
            static_cast<Heard*>(context)->said.push_back(std::string("face ") + kStatuses[prCssFontFaceStatus(face)]);
        },
        [](void* context, PrDomDocument*, PrCssFontFaceSet* set) {
            bool loading = prCssFontFaceSetStatus(set) == PrCssFontFaceSetLoadStatus_Loading;
            static_cast<Heard*>(context)->said.push_back(loading ? "set loading" : "set loaded");
        },
        &heard,
    };
    PrHtmlPage* page = prHtmlPageCreate(nullptr);
    CHECK(prHtmlPageLoad(page, "<p>text", "about:blank", nullptr) == PrStatus_Ok);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    prCssDocumentSetFontCallback(document, &callback);
    PrCssFontFaceSet* fonts = prCssDocumentFonts(document);
    PrDomEventListener listener = recorder(&heard);
    for (PrStringView type : {"loading", "loadingdone", "loadingerror"})
        prDomEventTargetAddEventListener(prCssFontFaceSetAsEventTarget(fonts), type, &listener, nullptr);

    std::string url = fontUrl();
    PrCssFontFace* good = prCssFontFaceCreate("Good", url, nullptr);
    PrCssFontFace* bad = prCssFontFaceCreate("Bad", "local(missing)", nullptr);
    prCssFontFaceSetAdd(fonts, good);
    prCssFontFaceSetAdd(fonts, bad);
    CHECK(prCssFontFaceLoad(good, document) == PrCssFontFaceStatus_Loading);
    CHECK(prCssFontFaceLoad(bad, document) == PrCssFontFaceStatus_Loading);
    CHECK(prCssFontFaceSetStatus(fonts) == PrCssFontFaceSetLoadStatus_Loading);
    runTasks(page);
    CHECK(prCssFontFaceStatus(good) == PrCssFontFaceStatus_Loaded);
    CHECK(prCssFontFaceStatus(bad) == PrCssFontFaceStatus_Error);
    CHECK_FALSE(prCssFontFaceFailedToParse(bad));
    CHECK(prCssFontFaceSetStatus(fonts) == PrCssFontFaceSetLoadStatus_Loaded);
    CHECK(heard.said == std::vector<std::string>{
        "face loading", "set loading", "face loading", "loading 0",
        "face loaded", "face error", "loadingdone 1",
        "loadingerror 1", "set loaded",
    });
    prCssDocumentSetFontCallback(document, nullptr);
    prCssFontFaceRelease(good);
    prCssFontFaceRelease(bad);
    prHtmlPageDestroy(page);
}

TEST_CASE("a family that is no name as written is a string") {
    PrCssFontFace* list = prCssFontFaceCreate("A, B", "url(a)", nullptr);
    PrCssFontFace* generic = prCssFontFaceCreate("serif", "url(a)", nullptr);
    PrCssFontFace* named = prCssFontFaceCreate("Named Family", "url(a)", nullptr);
    CHECK(take(prCssFontFaceDescriptor(list, PrCssFontFaceDescriptor_Family)) == "\"A, B\"");
    CHECK(take(prCssFontFaceDescriptor(generic, PrCssFontFaceDescriptor_Family)) == "\"serif\"");
    CHECK(take(prCssFontFaceDescriptor(named, PrCssFontFaceDescriptor_Family)) == "Named Family");
    CHECK(prCssFontFaceStatus(list) == PrCssFontFaceStatus_Unloaded);
    prCssFontFaceRelease(list);
    prCssFontFaceRelease(generic);
    prCssFontFaceRelease(named);
}

TEST_CASE("a FontFaceSetLoadEvent holds the faces it names") {
    PrCssFontFace* face = prCssFontFaceCreate("A", "url(a)", nullptr);
    PrCssFontFaceSetLoadEventInit init = prCssFontFaceSetLoadEventInitDefault();
    init.fontfaces = &face;
    init.count = 1;
    PrCssFontFaceSetLoadEvent* event = prCssFontFaceSetLoadEventCreate("loadingdone", &init);
    prCssFontFaceRelease(face);
    PrCssFontFace* held = nullptr;
    CHECK(prCssFontFaceSetLoadEventFontFaces(event, &held, 1) == 1);
    CHECK(take(prCssFontFaceDescriptor(held, PrCssFontFaceDescriptor_Family)) == "A");
    prDomEventRelease(prCssFontFaceSetLoadEventAsEvent(event));
}

TEST_CASE("text in a family of @font-face falls back while its face loads, then takes the face") {
    Served served;
    std::string html = std::string("<style>@font-face { font-family: Web; src: url(fonts/Arimo.ttf) }</style>") + kSpans;
    PrHtmlPage* page = served.load(html.c_str());
    prHtmlPresentationUpdate(prHtmlPagePresentation(page), prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(widthOf(page, "web") == widthOf(page, "serif"));
    settle(page);
    CHECK(served.fetched == std::vector<std::string>{"Arimo.ttf"});
    CHECK(widthOf(page, "web") == widthOf(page, "sans"));
    CHECK(widthOf(page, "web") != widthOf(page, "serif"));
    prHtmlPageDestroy(page);
}

TEST_CASE("a family whose faces all failed hides the installed family of its name") {
    Served served;
    PrHtmlPage* page = served.load(
        "<style>@font-face { font-family: Arimo; src: url(Missing.ttf) } span { display: inline-block }</style>"
            "<span id=arimo style='font-family: Arimo, serif'>Hamburgefonstiv</span>"
            "<span id=serif style='font-family: serif'>Hamburgefonstiv</span>");
    settle(page);
    CHECK(served.fetched == std::vector<std::string>{"Missing.ttf"});
    CHECK(widthOf(page, "arimo") == widthOf(page, "serif"));
    prHtmlPageDestroy(page);
}

TEST_CASE("a face whose unicode-range holds none of the text is not fetched") {
    Served served;
    std::string html = std::string("<style>@font-face { font-family: Web; src: url(Arimo.ttf); unicode-range: U+0-7F }"
        "@font-face { font-family: Web; src: url(Cousine.ttf); unicode-range: U+400-4FF }</style>") + kSpans;
    PrHtmlPage* page = served.load(html.c_str());
    settle(page);
    CHECK(served.fetched == std::vector<std::string>{"Arimo.ttf"});
    CHECK(widthOf(page, "web") == widthOf(page, "sans"));
    prHtmlPageDestroy(page);
}

TEST_CASE("a WOFF face loads with the host's codec, and fails without one") {
    Served served;
    std::string html = std::string("<style>@font-face { font-family: Web; src: url(AD.woff) }</style>") + kSpans;
    PrHtmlPage* decoded = served.load(html.c_str());
    PrHtmlPage* undecoded = served.load(html.c_str(), false);
    settle(decoded);
    settle(undecoded);
    CHECK(widthOf(decoded, "web") != widthOf(decoded, "serif"));
    CHECK(widthOf(undecoded, "web") == widthOf(undecoded, "serif"));
    PrCssFontFaceSet* fonts = prCssDocumentFonts(prHtmlWindowDocument(prHtmlPageWindow(undecoded)));
    CHECK(prCssFontFaceStatus(prCssFontFaceSetAt(fonts, 0)) == PrCssFontFaceStatus_Error);
    prHtmlPageDestroy(undecoded);
    prHtmlPageDestroy(decoded);
}

TEST_CASE("a document's faces set only its own text") {
    Served served;
    std::string html = std::string("<style>@font-face { font-family: Web; src: url(Arimo.ttf) }</style>") + kSpans;
    PrHtmlPage* defining = served.load(html.c_str());
    PrHtmlPage* plain = served.load(kSpans);
    settle(defining);
    settle(plain);
    CHECK(widthOf(defining, "web") == widthOf(defining, "sans"));
    CHECK(widthOf(plain, "web") == widthOf(plain, "serif"));
    prHtmlPageDestroy(plain);
    prHtmlPageDestroy(defining);
}

TEST_CASE("a document's set is loading until its text is laid out and the faces it asks for load") {
    Served served;
    Heard heard;
    PrCssFontCallback callback = {
        nullptr,
        [](void* context, PrDomDocument*, PrCssFontFaceSet* set) {
            bool loading = prCssFontFaceSetStatus(set) == PrCssFontFaceSetLoadStatus_Loading;
            static_cast<Heard*>(context)->said.push_back(loading ? "set loading" : "set loaded");
        },
        &heard,
    };
    PrHtmlPage* page = served.load("<style>@font-face { font-family: A; src: url(Arimo.ttf) }</style>"
        "<p style='font-family: A'>text");
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    PrCssFontFaceSet* fonts = prCssDocumentFonts(document);
    // The document has loaded, and the set waits for a layout of its text, which may request the face.
    CHECK(prCssFontFaceStatus(prCssFontFaceSetAt(fonts, 0)) == PrCssFontFaceStatus_Unloaded);
    CHECK(prCssFontFaceSetStatus(fonts) == PrCssFontFaceSetLoadStatus_Loading);
    prCssDocumentSetFontCallback(document, &callback);
    prHtmlPresentationUpdate(prHtmlPagePresentation(page), prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(prCssFontFaceStatus(prCssFontFaceSetAt(fonts, 0)) == PrCssFontFaceStatus_Loading);
    CHECK(prCssFontFaceSetStatus(fonts) == PrCssFontFaceSetLoadStatus_Loading);
    runTasks(page);
    CHECK(prCssFontFaceStatus(prCssFontFaceSetAt(fonts, 0)) == PrCssFontFaceStatus_Loaded);
    CHECK(heard.said == std::vector<std::string>{"set loaded"});
    prCssDocumentSetFontCallback(document, nullptr);
    prHtmlPageDestroy(page);
}

TEST_CASE("a document's set whose text uses none of its faces is loaded once laid out") {
    Served served;
    PrHtmlPage* page = served.load("<style>@font-face { font-family: A; src: url(Arimo.ttf) }</style><p>text");
    PrCssFontFaceSet* fonts = prCssDocumentFonts(prHtmlWindowDocument(prHtmlPageWindow(page)));
    CHECK(prCssFontFaceSetStatus(fonts) == PrCssFontFaceSetLoadStatus_Loading);
    prHtmlPresentationUpdate(prHtmlPagePresentation(page), prLayoutVecFromPx(PrVec2{800, 600}));
    CHECK(prCssFontFaceStatus(prCssFontFaceSetAt(fonts, 0)) == PrCssFontFaceStatus_Unloaded);
    CHECK(prCssFontFaceSetStatus(fonts) == PrCssFontFaceSetLoadStatus_Loaded);
    CHECK(served.fetched.empty());
    prHtmlPageDestroy(page);
}

TEST_CASE("a set whose last loading face is deleted or cleared switches to loaded without its load") {
    for (bool clear : {false, true}) {
        Served served;
        served.hold = true;
        PrHtmlPage* page = served.load("<p>text");
        update(page);
        PrCssFontFaceSet* fonts = prCssDocumentFonts(prHtmlWindowDocument(prHtmlPageWindow(page)));
        SetHeard heard(page);
        PrCssFontFace* face = loadingFace(page);
        REQUIRE(served.fetcher.waiting() == 1);
        CHECK(prCssFontFaceSetStatus(fonts) == PrCssFontFaceSetLoadStatus_Loading);
        if (clear)
            prCssFontFaceSetClear(fonts);
        else
            CHECK(prCssFontFaceSetDelete(fonts, face));
        // The deletion changed the document's fonts, so the set waits for the layout that may request a font.
        update(page);
        CHECK(prCssFontFaceSetStatus(fonts) == PrCssFontFaceSetLoadStatus_Loaded);
        CHECK(heard.heard.said == std::vector<std::string>{"set loading", "loadingdone 0", "set loaded"});
        // The face's load still settles the face, and changes nothing in the set that it left.
        served.release();
        runTasks(page);
        CHECK(prCssFontFaceStatus(face) == PrCssFontFaceStatus_Loaded);
        CHECK(prCssFontFaceSetStatus(fonts) == PrCssFontFaceSetLoadStatus_Loaded);
        CHECK(heard.heard.said.size() == 3);
        prCssFontFaceRelease(face);
        prHtmlPageDestroy(page);
    }
}

TEST_CASE("a face deleted while it loads and added again settles the set it rejoined") {
    Served served;
    served.hold = true;
    PrHtmlPage* page = served.load("<p>text");
    update(page);
    PrCssFontFaceSet* fonts = prCssDocumentFonts(prHtmlWindowDocument(prHtmlPageWindow(page)));
    SetHeard heard(page);
    PrCssFontFace* face = loadingFace(page);
    CHECK(prCssFontFaceSetDelete(fonts, face));
    update(page);
    CHECK(prCssFontFaceSetAdd(fonts, face) == PrStatus_Ok);
    CHECK(prCssFontFaceSetStatus(fonts) == PrCssFontFaceSetLoadStatus_Loading);
    served.release();
    runTasks(page);
    update(page);
    CHECK(prCssFontFaceSetStatus(fonts) == PrCssFontFaceSetLoadStatus_Loaded);
    CHECK(heard.heard.said == std::vector<std::string>{
        "set loading", "loadingdone 0", "set loaded", "set loading", "loadingdone 1",
        "set loaded",
    });
    prCssFontFaceRelease(face);
    prHtmlPageDestroy(page);
}

TEST_CASE("a face whose sheet leaves the document while it loads leaves the set loaded") {
    Served served;
    served.hold = true;
    PrHtmlPage* page = served.load("<style id=s>@font-face { font-family: A; src: url(Arimo.ttf) }</style>"
        "<p style='font-family: A'>text");
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    PrCssFontFaceSet* fonts = prCssDocumentFonts(document);
    update(page);
    runTasks(page);
    REQUIRE(served.fetcher.waiting() == 1);
    CHECK(prCssFontFaceSetStatus(fonts) == PrCssFontFaceSetLoadStatus_Loading);
    SetHeard heard(page);
    prDomNodeRemove(prDomElementAsNode(prDomNodeGetElementById(prDomDocumentAsNode(document), "s")));
    update(page);
    CHECK(prCssFontFaceSetStatus(fonts) == PrCssFontFaceSetLoadStatus_Loaded);
    CHECK(heard.heard.said == std::vector<std::string>{"loadingdone 0", "set loaded"});
    served.release();
    runTasks(page);
    CHECK(heard.heard.said.size() == 2);
    prHtmlPageDestroy(page);
}

TEST_CASE("a presentation awaits font requests while a change since its last sync may add a face") {
    Served served;
    PrHtmlPage* page = served.load("<style>@font-face { font-family: Unused; src: url(Arimo.ttf) }"
        "</style><p style='font-family: A'>text");
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    PrCssPresentation* presentation = prHtmlPagePresentation(page);
    settle(page);
    // A face that no text uses does not keep a laid out document waiting.
    CHECK_FALSE(cssPresentationAwaitsFontRequests(presentation));
    CHECK(prCssFontFaceSetStatus(prCssDocumentFonts(document)) == PrCssFontFaceSetLoadStatus_Loaded);

    SUBCASE("a sheet inserted") {
        PrDomElement* style = nullptr;
        REQUIRE(prDomElementCreate(document, "style", nullptr, &style) == PrStatus_Ok);
        prDomNodeSetTextContent(prDomElementAsNode(style), "@font-face { font-family: A; src: url(Arimo.ttf) }");
        REQUIRE(prDomNodeAppendChild(prDomElementAsNode(prDomDocumentHead(document)), prDomElementAsNode(style)) ==
            PrStatus_Ok);
        prDomNodeRelease(prDomElementAsNode(style));
    }
    SUBCASE("a rule inserted") {
        PrCssStyleSheet* sheet = prCssStyleSheetListStyleSheetAt(prCssDocumentStyleSheets(document), 0);
        REQUIRE(sheet != nullptr);
        REQUIRE(prCssStyleSheetInsertRule(sheet, "@font-face { font-family: A; src: url(Arimo.ttf) }", 0, nullptr) ==
            PrStatus_Ok);
    }
    SUBCASE("a face added") {
        PrCssFontFace* face = prCssFontFaceCreate("A", "url(Arimo.ttf)", nullptr);
        REQUIRE(prCssFontFaceSetAdd(prCssDocumentFonts(document), face) == PrStatus_Ok);
        prCssFontFaceRelease(face);
    }
    CHECK(cssPresentationAwaitsFontRequests(presentation));
    settle(page);
    CHECK_FALSE(cssPresentationAwaitsFontRequests(presentation));
    CHECK(served.fetched == std::vector<std::string>{"Arimo.ttf"});
    prHtmlPageDestroy(page);
}

TEST_CASE("a family whose matched face failed falls back to the next family, not to another of its faces") {
    Served served;
    PrHtmlPage* page = served.load("<style>@font-face { font-family: Web; font-weight: 400; src: url(Missing.ttf) }"
        "@font-face { font-family: Web; font-weight: 300; src: url(Arimo.ttf) }span { display: inline-block }</style>"
        "<span id=web style='font-family: Web, serif'>Hamburgefonstiv</span>"
        "<span id=serif style='font-family: serif'>Hamburgefonstiv</span>");
    settle(page);
    CHECK(served.fetched == std::vector<std::string>{"Missing.ttf"});
    CHECK(widthOf(page, "web") == widthOf(page, "serif"));
    prHtmlPageDestroy(page);
}

TEST_CASE("a failed face gives way to an earlier face of the same style") {
    Served served;
    std::string html = std::string("<style>@font-face { font-family: Web; src: url(Arimo.ttf) }"
        "@font-face { font-family: Web; src: url(Missing.ttf) }</style>") + kSpans;
    PrHtmlPage* page = served.load(html.c_str());
    settle(page);
    CHECK(served.fetched == std::vector<std::string>{"Missing.ttf", "Arimo.ttf"});
    CHECK(widthOf(page, "web") == widthOf(page, "sans"));
    prHtmlPageDestroy(page);
}

TEST_CASE("a rule put back where it was ranks where its rule is, not as the newest face") {
    Served served;
    PrHtmlPage* page = served.load("<style id=first>@font-face { font-family: Web; src: url(Arimo.ttf) }</style>"
        "<style id=second>@font-face { font-family: Web; src: url(Cousine.ttf) }</style>"
        "<style>span { display: inline-block }</style><span id=web style='font-family: Web'>Hamburgefonstiv</span>"
        "<span id=mono style='font-family: Cousine'>Hamburgefonstiv</span>");
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    settle(page);
    CHECK(widthOf(page, "web") != widthOf(page, "mono"));
    PrDomNode* first = prDomElementAsNode(prDomNodeGetElementById(prDomDocumentAsNode(document), "first"));
    PrDomNode* second = prDomElementAsNode(prDomNodeGetElementById(prDomDocumentAsNode(document), "second"));
    float before = widthOf(page, "web");
    prDomNodeRetain(first);
    prDomNodeRemove(first);
    settle(page);
    REQUIRE(prDomNodeInsertBefore(prDomNodeParentNode(second), first, second) == PrStatus_Ok);
    prDomNodeRelease(first);
    settle(page);
    // The second rule is still the last defined, and its face is checked first.
    CHECK(widthOf(page, "web") == before);
    prHtmlPageDestroy(page);
}

TEST_CASE("a face's size-adjust scales its font once") {
    Served served;
    PrHtmlPage* page = served.load("<style>@font-face { font-family: Web; src: url(Arimo.ttf) }"
        "@font-face { font-family: Half; src: url(Arimo.ttf); size-adjust: 50% }span { display: inline-block }</style>"
        "<span id=web style='font-family: Web'>Hamburgefonstiv</span>"
        "<span id=half style='font-family: Half'>Hamburgefonstiv</span>");
    settle(page);
    CHECK(widthOf(page, "half") == doctest::Approx(widthOf(page, "web") / 2).epsilon(0.02));
    // The two rules name one source, which is fetched once.
    CHECK(served.fetched == std::vector<std::string>{"Arimo.ttf"});
    prHtmlPageDestroy(page);
}

TEST_CASE("a rule put back whose source the document decoded sets its face at once, without a fetch") {
    Served served;
    std::string html = std::string("<style id=s>@font-face { font-family: Web; src: url(Arimo.ttf) }</style>") + kSpans;
    PrHtmlPage* page = served.load(html.c_str());
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    settle(page);
    float web = widthOf(page, "web");
    CHECK(web == widthOf(page, "sans"));
    PrDomNode* style = prDomElementAsNode(prDomNodeGetElementById(prDomDocumentAsNode(document), "s"));
    PrDomNode* head = prDomNodeParentNode(style);
    prDomNodeRetain(style);
    prDomNodeRemove(style);
    // Laid out without the style element, the text falls back.
    settle(page);
    CHECK(widthOf(page, "web") == widthOf(page, "serif"));
    // A new style element with the same text: its face is set in the first update afterward, from the source already
    // decoded.
    PrDomElement* copy = nullptr;
    REQUIRE(prDomElementCreate(document, "style", nullptr, &copy) == PrStatus_Ok);
    prDomNodeSetTextContent(prDomElementAsNode(copy), "@font-face { font-family: Web; src: url(Arimo.ttf) }");
    REQUIRE(prDomNodeAppendChild(head, prDomElementAsNode(copy)) == PrStatus_Ok);
    prDomNodeRelease(prDomElementAsNode(copy));
    prDomNodeRelease(style);
    update(page);
    CHECK(widthOf(page, "web") == web);
    settle(page);
    CHECK(widthOf(page, "web") == web);
    CHECK(served.fetched == std::vector<std::string>{"Arimo.ttf"});
    prHtmlPageDestroy(page);
}

TEST_CASE("a face tries its sources in order, fetching an earlier one before a decoded later one") {
    Served served;
    PrHtmlPage* page = served.load("<style>@font-face { font-family: First; src: url(Arimo.ttf) }"
        "@font-face { font-family: Second; src: url(Cousine.ttf), url(Arimo.ttf) }"
        "span { display: inline-block }</style><span id=first style='font-family: First'>Hamburgefonstiv</span>");
    settle(page);
    CHECK(served.fetched == std::vector<std::string>{"Arimo.ttf"});
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    PrDomElement* span = nullptr;
    REQUIRE(prDomElementCreate(document, "span", nullptr, &span) == PrStatus_Ok);
    REQUIRE(prDomElementSetAttribute(span, "style", "font-family: Second") == PrStatus_Ok);
    prDomNodeSetTextContent(prDomElementAsNode(span), "Hamburgefonstiv");
    REQUIRE(prDomNodeAppendChild(prDomElementAsNode(prDomDocumentBody(document)), prDomElementAsNode(span)) ==
        PrStatus_Ok);
    prDomNodeRelease(prDomElementAsNode(span));
    settle(page);
    CHECK(served.fetched == std::vector<std::string>{"Arimo.ttf", "Cousine.ttf"});
    prHtmlPageDestroy(page);
}

TEST_CASE("fonts that load one at a time, and leave and come back, lay out as a full recomputation does") {
    Served served;
    served.hold = true;
    PrHtmlPage* page = served.load("<style id=fonts>@font-face { font-family: Web; src: url(Arimo.ttf) }"
        "@font-face { font-family: Web; src: url(Cousine.ttf); unicode-range: U+30-39 }"
        "@font-face { font-family: Web; src: url(Twemoji.ttf); unicode-range: U+1F600-1F64F }</style>"
        "<style>body { font-family: Web, serif; width: 600px } .empty { display: inline-block }"
        ".float { float: left } td { border: 1px solid }</style>"
        "<p id=text>Hamburgefonstiv e\xcc\x81 2026 \xf0\x9f\x98\x80 \xd0\x96</p>"
        "<p><span class=empty id=empty></span></p><div class=float id=float>shrink to fit 123</div>"
        "<table><tr><td id=cell>a cell 456</td><td>\xf0\x9f\x98\x83</td></tr></table>"
        "<p style='font-family: sans-serif' id=plain>set in the default family</p>");
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    verifyUpdate(page);
    runTasks(page);
    float fallback = widthOf(page, "float");
    // Each load settles in its own frame.
    while (!(served.fetcher.waiting() == 0)) {
        served.release(1);
        runTasks(page);
        verifyUpdate(page);
        runTasks(page);
    }
    CHECK(served.fetched.size() == 3);
    CHECK(widthOf(page, "float") != fallback);
    float loaded = widthOf(page, "float");

    // The fonts' sheet is removed, the document is laid out without it, and the sheet comes back: its faces load from
    // the sources that the document decoded.
    PrDomNode* fonts = prDomElementAsNode(prDomNodeGetElementById(prDomDocumentAsNode(document), "fonts"));
    PrDomNode* head = prDomNodeParentNode(fonts);
    PrDomNode* next = prDomNodeNextSibling(fonts);
    prDomNodeRetain(fonts);
    prDomNodeRemove(fonts);
    verifyUpdate(page);
    runTasks(page);
    CHECK(widthOf(page, "float") == fallback);
    REQUIRE(prDomNodeInsertBefore(head, fonts, next) == PrStatus_Ok);
    prDomNodeRelease(fonts);
    verifyUpdate(page);
    CHECK(widthOf(page, "float") == loaded);
    runTasks(page);
    verifyUpdate(page);
    CHECK(served.fetched.size() == 3);
    prHtmlPageDestroy(page);
}

TEST_CASE("a set whose script face loads switches to loaded at the next style update, with nothing to lay out") {
    Served served;
    PrHtmlPage* page = served.load("<p>text");
    update(page);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    PrCssFontFaceSet* fonts = prCssDocumentFonts(document);
    PrCssFontFace* face = prCssFontFaceCreate("B", "url(Arimo.ttf)", nullptr);
    REQUIRE(prCssFontFaceSetAdd(fonts, face) == PrStatus_Ok);
    CHECK(prCssFontFaceLoad(face, document) == PrCssFontFaceStatus_Loading);
    runTasks(page);
    CHECK(prCssFontFaceStatus(face) == PrCssFontFaceStatus_Loaded);
    // The addition left the presentation's count of unloaded faces stale, and no text uses the face.
    cssPresentationUpdateStyle(prHtmlPagePresentation(page));
    CHECK(prCssFontFaceSetStatus(fonts) == PrCssFontFaceSetLoadStatus_Loaded);
    prCssFontFaceRelease(face);
    prHtmlPageDestroy(page);
}

TEST_CASE("a document with no presentation leaves a face whose sheet left out of its set") {
    Served served;
    served.hold = true;
    PrHtmlPage* page =
        served.load("<style id=s>@font-face { font-family: A; src: url(Arimo.ttf) }</style><p>text", true, false);
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    REQUIRE(prHtmlPagePresentation(page) == nullptr);
    PrCssFontFaceSet* fonts = prCssDocumentFonts(document);
    PrCssFontFace* face = prCssFontFaceSetAt(fonts, 0);
    CHECK(prCssFontFaceLoad(face, document) == PrCssFontFaceStatus_Loading);
    runTasks(page);
    REQUIRE(served.fetcher.waiting() == 1);
    CHECK(prCssFontFaceSetStatus(fonts) == PrCssFontFaceSetLoadStatus_Loading);
    {
        SetHeard heard(page);
        // The element, and the sheet that it owns, stay alive while they are out of the document.
        PrDomNode* style = prDomElementAsNode(prDomNodeGetElementById(prDomDocumentAsNode(document), "s"));
        prDomNodeRetain(style);
        prDomNodeRemove(style);
        runTasks(page);
        CHECK(prCssFontFaceSetStatus(fonts) == PrCssFontFaceSetLoadStatus_Loaded);
        CHECK(heard.heard.said == std::vector<std::string>{"loadingdone 0", "set loaded"});
        served.release();
        runTasks(page);
        CHECK(heard.heard.said.size() == 2);
        prDomNodeRelease(style);
    }
    prHtmlPageDestroy(page);
}

TEST_CASE("sources no face uses are bounded as they become unused, whether or not anything is fetched after") {
    Served served;
    PrHtmlPage* page = served.load("<p>text");
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(page));
    // More sources than the cache keeps unused, each held by a face that loaded from it.
    std::vector<PrCssFontFace*> faces;
    for (int i = 0; i < 80; ++i) {
        std::string source = "url(Arimo.ttf?" + std::to_string(i) + ")";
        PrCssFontFace* face = prCssFontFaceCreate("B", source, nullptr);
        prCssFontFaceLoad(face, document);
        faces.push_back(face);
    }
    runTasks(page);
    CHECK(prCssFontFaceStatus(faces.back()) == PrCssFontFaceStatus_Loaded);
    const CssRoot* style = cssDocumentRoot(document);
    CHECK(style->fontSources.count == 80);
    // The faces are released, nothing is fetched afterward, and the cache keeps no more than its bound. If faces are
    // released newest first, a source becomes the least recently used when its own last face is released, which evicts
    // it within that release.
    SUBCASE("oldest first") {
        for (PrCssFontFace* face : faces)
            prCssFontFaceRelease(face);
    }
    SUBCASE("newest first") {
        for (size_t i = faces.size(); i > 0; --i)
            prCssFontFaceRelease(faces[i - 1]);
    }
    CHECK(style->fontSources.count <= 64);
    CHECK(style->fontSources.count > 0);
    prHtmlPageDestroy(page);
}

TEST_CASE("a length in ch measures the face that loads once it has loaded") {
    Served served;
    PrHtmlPage* page = served.load("<style>@font-face { font-family: Web; src: url(Ahem.ttf) }</style>"
        "<div id=box style='font: 20px Web, serif; width: 2ch'>x</div>");
    prHtmlPresentationUpdate(prHtmlPagePresentation(page), prLayoutVecFromPx(PrVec2{800, 600}));
    float before = widthOf(page, "box");
    CHECK(before < 30);
    settle(page);
    // Ahem's "0" is an em wide.
    CHECK(widthOf(page, "box") == 40);
    prHtmlPageDestroy(page);
}
