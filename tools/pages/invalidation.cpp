// The invalidation check: makes changes to a captured page, checks each against a full recomputation, and counts its
// cost.
//
// Loads a page that tools/pages/pages.py captured, without its scripts, and makes a series of changes to it, as a
// page's scripts and its user would. For elements spread through the page, it toggles each class off and on, removes
// the element and puts it back, replaces its text, and inserts a float into it and removes it again. Then it resizes
// the viewport and back. After each change it brings the presentation up to date, paints it, and checks it against a
// full recomputation (prCssPresentationVerify), which also checks that each stage's work was bounded by the inputs that
// changed. It counts what each change cost and did, and how much of that was wasted: elements restyled to the same
// style, and boxes painted again although nothing that they paint changed. It also counts how the text was divided into
// the leaves, fragments and glyph commands that layout placed and painting recorded.
//
// tools/pages/invalidation.py runs this over the pages corpus and prints the totals, and ./dev.sh bench page-changes
// runs it without the checks to measure what each kind of change costs. Each run prints one JSON object. With --counts,
// each kind of change carries the total cost of its frames, as a benchmark's row does (harnessCountsWrite): the frame
// under "time", bringing the presentation up to date (style and layout) under "update", and recording its scene under
// "scene".
//
// --element I makes only the changes of the Ith chosen element, counting from zero, to investigate one that fails.
// --match TEXT makes only the changes of the first element whose class attribute holds TEXT, whose name is TEXT, or
// which has an attribute named TEXT. That element stays the same as the page is cut down around it
// (tools/pages/reduce.py). With --element, --mark FILE makes no change. It writes the page as it loaded to FILE, with a
// data-invalidation-target attribute on the element, which --match data-invalidation-target then names.
//
// --restyle-threads N restyles on N threads, as pr_shell's option does.
//
// Usage: invalidation [--elements N] [--element I [--mark FILE] | --match TEXT] [--no-verify] [--counts]
//              [--restyle-threads N] <capture.json> <page.html>

#include "pr/assert.h"
#include "pr/css_presentation.h"
#include "pr/dom.h"
#include "pr/html.h"
#include "pr/html_page.h"
#include "pr/layout_scene.h"
#include "pr/layout_tree.h"
#include "pr/text_harfbuzz.h"

#include "tools/harness/harness.h"
#include "tools/shell/page_host.h"

#include <memory>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <string_view>
#include <vector>

using namespace pr;
using namespace harness;
using page_host::Page;
using page_host::PageHost;

namespace {

// The viewport that the page is laid out in, which is the size that a window starts with, and the narrower viewport
// that a resize alternates with.
constexpr PrVec2 kViewport = {800, 600};
constexpr PrVec2 kNarrowViewport = {640, 600};

// How text was divided into placed pieces over a number of frames (PrLayoutTreeStats).
struct TextCounts {
    uint64_t leaves = 0;
    uint64_t fragments = 0;
    uint64_t bytes = 0;
    uint64_t lineChildren = 0;
    uint64_t recorded = 0;
    uint64_t glyphCommands = 0;

    void add(const PrLayoutTreeStats& laid) {
        leaves += laid.textLeavesPlaced;
        fragments += laid.textFragmentsMade;
        bytes += laid.textFragmentBytes;
        lineChildren += laid.lineChildrenMade;
        recorded += laid.textFragmentsRecorded;
        glyphCommands += laid.glyphCommandsRecorded;
    }

    // Appends the counts as JSON members, each after a comma.
    void write(std::string* out) const {
        *out += ", \"textLeaves\": " + std::to_string(leaves) + ", \"textFragments\": " + std::to_string(fragments) +
            ", \"textBytes\": " + std::to_string(bytes) + ", \"lineChildren\": " + std::to_string(lineChildren) +
            ", \"textRecorded\": " + std::to_string(recorded) + ", \"glyphCommands\": " + std::to_string(glyphCommands);
    }
};

// The cost of a kind of change, over the frames that followed each change of that kind.
struct Kind {
    std::string name;
    uint32_t frames = 0;
    uint64_t instructions = 0;
    uint64_t styled = 0;
    uint64_t styledUnchanged = 0;
    uint64_t blocks = 0;
    uint64_t commands = 0;
    TextCounts text{};

    // The total cost of its frames, and the part of it spent bringing the presentation up to date.
    HarnessCounters frame{};
    HarnessCounters update{};
};

// What a run found.
struct Totals {
    std::vector<Kind> kinds;
    uint32_t frames = 0;
    uint32_t failures = 0;
    std::string firstFailure;
    uint64_t instructions = 0;
    uint64_t visited = 0;
    uint64_t styled = 0;
    uint64_t styledUnchanged = 0;
    uint64_t blocks = 0;
    uint64_t commands = 0;
    TextCounts text{};
    uint64_t repaintedUnchanged = 0;
};

// Appends the elements under `node` to `out`, in tree order.
void collect(PrDomNode* node, std::vector<PrDomElement*>* out) {
    for (PrDomElement* e = prDomNodeFirstElementChild(node); e != nullptr;
            e = prDomNodeNextElementSibling(prDomElementAsNode(e))) {
        out->push_back(e);
        collect(prDomElementAsNode(e), out);
    }
}

// Runs a frame after a change, in a viewport of `viewport`. The presentation is brought up to date and painted, then
// checked, and its work is counted.
void frame(PrCssPresentation* presentation, bool verify, const char* step, Totals* totals, PrVec2 viewport = kViewport) {
    HarnessCounters started = harnessCounters();
    bool changed = prHtmlPresentationUpdate(presentation, prLayoutVecFromPx(viewport));
    HarnessCounters updated = harnessCounters();
    PrLayoutTree* tree = prCssPresentationTree(presentation);
    const PrLayoutScene* scene = prLayoutTreeScene(tree);
    prLayoutSceneRelease(scene);
    HarnessCounters spent = harnessSpent(started, harnessCounters());
    ++totals->frames;
    totals->instructions += uint64_t(spent.instructions);
    PrCssPresentationStats styled = prCssPresentationStats(presentation);
    totals->visited += styled.elementsVisited;
    totals->styled += styled.elementsStyled;
    totals->styledUnchanged += styled.elementsStyledUnchanged;
    PrLayoutTreeStats laid = changed ? prLayoutTreeStats(tree) : PrLayoutTreeStats{};
    totals->blocks += laid.blocksLaidOut;
    totals->commands += laid.commandsRecorded;
    totals->text.add(laid);
    totals->repaintedUnchanged += laid.boxesRepaintedUnchanged;
    Kind* kind = nullptr;
    for (Kind& k : totals->kinds) {
        if (k.name == step)
            kind = &k;
    }
    if (kind == nullptr) {
        totals->kinds.push_back(Kind{step});
        kind = &totals->kinds.back();
    }
    ++kind->frames;
    kind->instructions += uint64_t(spent.instructions);
    kind->frame = harnessSum(kind->frame, spent);
    kind->update = harnessSum(kind->update, harnessSpent(started, updated));
    kind->styled += styled.elementsStyled;
    kind->styledUnchanged += styled.elementsStyledUnchanged;
    kind->blocks += laid.blocksLaidOut;
    kind->commands += laid.commandsRecorded;
    kind->text.add(laid);
    if (!verify)
        return;
    PrString report{};
    if (!prCssPresentationVerify(presentation, &report)) {
        if (totals->failures++ == 0) {
            PrStringView what = prStringView(&report);
            totals->firstFailure = std::string(step) + ": " + std::string(what.data, what.size);
        }
    }
    prStringReset(&report);
}

// Returns true if `match` names the element: if its class attribute holds `match`, `match` is the element's name, or
// the element has an attribute with that name.
bool names(const PrDomElement* element, const char* match) {
    auto view = [](PrStringView text) {
        return std::string_view(text.data != nullptr ? text.data : "", text.size);
    };
    return view(prDomElementGetAttribute(element, "class")).find(match) != std::string_view::npos ||
        view(prDomElementLocalName(element)) == match || prDomElementHasAttribute(element, match);
}

// Appends `text` to `out` as a JSON string.
void appendJson(std::string* out, const std::string& text) {
    out->push_back('"');
    for (char c : text) {
        if (c == '"' || c == '\\')
            out->push_back('\\');
        if (uint8_t(c) < 0x20)
            c = ' ';
        out->push_back(c);
    }
    out->push_back('"');
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    uint32_t elements = 60;
    bool verify = true;
    bool counts = false;
    uint32_t only = UINT32_MAX;
    const char* match = nullptr;
    const char* marked = nullptr;
    int restyleThreads = -1;
    int at = 1;
    for (; at < argc && strncmp(argv[at], "--", 2) == 0; ++at) {
        if (strcmp(argv[at], "--elements") == 0 && at + 1 < argc)
            elements = uint32_t(atoi(argv[++at]));
        else if (strcmp(argv[at], "--no-verify") == 0)
            verify = false;
        else if (strcmp(argv[at], "--counts") == 0)
            counts = true;
        else if (strcmp(argv[at], "--element") == 0 && at + 1 < argc)
            only = uint32_t(atoi(argv[++at]));
        else if (strcmp(argv[at], "--match") == 0 && at + 1 < argc)
            match = argv[++at];
        else if (strcmp(argv[at], "--mark") == 0 && at + 1 < argc)
            marked = argv[++at];
        else if (strcmp(argv[at], "--restyle-threads") == 0 && at + 1 < argc)
            restyleThreads = atoi(argv[++at]);
        else
            break;
    }
    if (argc - at != 2) {
        fprintf(stderr, "usage: invalidation [--elements N] [--element I [--mark FILE] | --match TEXT] [--no-verify] [--counts] "
            "[--restyle-threads N] <capture.json> <page.html>\n");
        return 2;
    }
    std::unique_ptr<PrTextShaper, decltype(&prTextShaperDestroy)> shaper(prTextHarfBuzzCreate(), prTextShaperDestroy);
    PageHost host(shaper.get(), nullptr, nullptr);
    if (restyleThreads >= 0)
        host.setRestyleThreads(uint32_t(restyleThreads));
    host.disableScripts();
    host.useVirtualTime();
    std::string url;
    if (!host.addCapture(argv[at], &url, argv[at + 1]))
        return 1;
    Page* page = host.load(url);
    if (page == nullptr || host.settle() == PageHost::Settled::Failed)
        return 1;
    PrHtmlPage* htmlPage = PageHost::htmlPage(page);
    PrCssPresentation* presentation = htmlPage != nullptr ? prHtmlPagePresentation(htmlPage) : nullptr;
    PrDomDocument* document = htmlPage != nullptr ? prHtmlWindowDocument(prHtmlPageWindow(htmlPage)) : nullptr;
    PrDomElement* body = document != nullptr ? prDomDocumentBody(document) : nullptr;
    if (presentation == nullptr || body == nullptr) {
        fprintf(stderr, "%s: no rendering\n", argv[at + 1]);
        return 1;
    }

    Totals totals;
    frame(presentation, verify, "load", &totals);
    totals = Totals{{}, totals.frames, totals.failures, totals.firstFailure};
    std::vector<PrDomElement*> all;
    collect(prDomElementAsNode(body), &all);
    PrDomElement* floated = nullptr;
    if (prDomElementCreate(document, "div", nullptr, &floated) != PrStatus_Ok)
        return 1;
    prDomElementSetAttribute(floated, "style", "float: left; width: 120px; height: 60px");
    if (match != nullptr) {
        // The first element that --match names is the only element changed.
        std::vector<PrDomElement*> matched;
        for (PrDomElement* e : all) {
            if (names(e, match)) {
                matched.push_back(e);
                break;
            }
        }
        all = matched;
        only = 0;
    }
    uint32_t chosen = all.size() < elements ? uint32_t(all.size()) : elements;
    for (uint32_t i = 0; i < chosen; ++i) {
        if (only != UINT32_MAX && i != only)
            continue;
        PrDomElement* element = all[size_t(i) * all.size() / chosen];
        if (marked != nullptr) {
            prDomElementSetAttribute(element, "data-invalidation-target", "");
            PrString markup = prHtmlOuterHtml(prDomDocumentElement(document));
            PrStringView text = prStringView(&markup);
            FILE* out = fopen(marked, "wb");
            bool written = out != nullptr && fputs("<!DOCTYPE html>\n", out) >= 0 &&
                fwrite(text.data, 1, text.size, out) == text.size;
            if (out != nullptr)
                fclose(out);
            prStringReset(&markup);
            if (!written) {
                fprintf(stderr, "invalidation: cannot write %s\n", marked);
                return 1;
            }
            return 0;
        }
        if (only != UINT32_MAX) {
            PrStringView n = prDomElementLocalName(element);
            PrStringView c = prDomElementGetAttribute(element, "class");
            fprintf(stderr, "element %u: %.*s class=\"%.*s\"\n", i, int(n.size), n.data, int(c.size), c.data);
        }
        PrDomNode* node = prDomElementAsNode(element);
        // Turns each class off, then on again.
        PrDomTokenList* classes = prDomTokenListCreate(element, "class");
        uint32_t count = prDomTokenListLength(classes);
        for (uint32_t c = 0; c < count && c < 4; ++c) {
            // Removing the token rewrites the attribute that the item views.
            PrString token = prStringCopy(prDomTokenListItem(classes, c));
            PrStringView name = prStringView(&token);
            prDomTokenListRemove(classes, &name, 1);
            frame(presentation, verify, "class removed", &totals);
            prDomTokenListAdd(classes, &name, 1);
            frame(presentation, verify, "class added", &totals);
            prStringReset(&token);
        }
        prDomTokenListDestroy(classes);
        // Sets an attribute that no selector is likely to read.
        prDomElementSetAttribute(element, "data-invalidation", "1");
        frame(presentation, verify, "attribute set", &totals);
        // Removes the element, then puts it back where it was.
        PrDomNode* parent = prDomNodeParentNode(node);
        PrDomNode* next = prDomNodeNextSibling(node);
        if (parent != nullptr) {
            // A node removed with no reference to it is freed.
            prDomNodeRetain(node);
            prDomNodeRemove(node);
            frame(presentation, verify, "removed", &totals);
            prDomNodeInsertBefore(parent, node, next);
            frame(presentation, verify, "inserted", &totals);
            prDomNodeRelease(node);
        }
        // Replaces the element's first text child.
        for (PrDomNode* child = prDomNodeFirstChild(node); child != nullptr; child = prDomNodeNextSibling(child)) {
            if (prDomNodeType(child) == PrDomNodeType_Text) {
                prDomNodeSetTextContent(child, "Text the check put in place of what was here");
                frame(presentation, verify, "text replaced", &totals);
                break;
            }
        }
        // Inserts a float as the element's first child, beside the content that follows, then removes it.
        prDomNodeInsertBefore(node, prDomElementAsNode(floated), prDomNodeFirstChild(node));
        frame(presentation, verify, "float inserted", &totals);
        prDomNodeRemove(prDomElementAsNode(floated));
        frame(presentation, verify, "float removed", &totals);
    }
    // Narrows the viewport and widens it again, which lays out again whatever depends on its width.
    for (uint32_t i = 0; i < 2; ++i) {
        frame(presentation, verify, "resized", &totals, kNarrowViewport);
        frame(presentation, verify, "resized", &totals);
    }
    prDomNodeRelease(prDomElementAsNode(floated));

    std::string out = "{\"elements\": " + std::to_string(all.size()) + ", \"changed\": " + std::to_string(chosen) +
        ", \"frames\": " + std::to_string(totals.frames) +
        ", \"failures\": " + std::to_string(totals.failures) + ", \"firstFailure\": ";
    appendJson(&out, totals.firstFailure);
    out += ", \"instructions\": " + std::to_string(totals.instructions) + ", \"visited\": " +
        std::to_string(totals.visited) + ", \"styled\": " + std::to_string(totals.styled) +
        ", \"styledUnchanged\": " + std::to_string(totals.styledUnchanged) + ", \"blocks\": " +
        std::to_string(totals.blocks) + ", \"commands\": " + std::to_string(totals.commands) +
        ", \"repaintedUnchanged\": " + std::to_string(totals.repaintedUnchanged);
    totals.text.write(&out);
    out += ", \"kinds\": [";
    for (size_t i = 0; i < totals.kinds.size(); ++i) {
        const Kind& k = totals.kinds[i];
        out += i != 0 ? ", {\"name\": " : "{\"name\": ";
        appendJson(&out, k.name);
        out += ", \"frames\": " + std::to_string(k.frames) + ", \"instructions\": " + std::to_string(k.instructions) +
            ", \"styled\": " + std::to_string(k.styled) + ", \"styledUnchanged\": " +
            std::to_string(k.styledUnchanged) + ", \"blocks\": " + std::to_string(k.blocks) +
            ", \"commands\": " + std::to_string(k.commands);
        k.text.write(&out);
        if (counts) {
            HarnessCounts row{};
            harnessCountsAdd(&row, "time", k.frame);
            harnessCountsAdd(&row, "update", k.update);
            harnessCountsAdd(&row, "scene", harnessSpent(k.update, k.frame));
            TextBuffer written{};
            harnessCountsWrite(&row, &written);
            out.append(written.text.data, written.text.count);
            textReset(&written);
        }
        out += "}";
    }
    out += "]}\n";
    fputs(out.c_str(), stdout);
    return 0;
}
