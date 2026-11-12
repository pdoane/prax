#include "pr/css_quickjs.h"
#include "pr/dom_geometry_quickjs.h"
#include "pr/dom_quickjs.h"
#include "pr/html.h"
#include "pr/html_page.h"
#include "pr/html_quickjs.h"
#include "pr/mathml_quickjs.h"
#include "pr/memory.h"
#include "pr/quickjs_binding.h"
#include "pr/string.h"
#include "pr/svg_quickjs.h"
#include "pr/webidl_quickjs.h"

#include "html/quickjs/src/html_bindings_quickjs.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// html/#queue-a-microtask, in the same queue as the script engine's own jobs.
void queueMicrotask(void* PR_NULLABLE context, PrHtmlPage*, void (*run)(void* data), void* data) {
    jsEnqueueNative(static_cast<JSContext*>(context), run, data);
}

PrHtmlHost hostOf(JSContext* ctx) {
    PrHtmlHost host = prHtmlHostDefault();
    host.context = ctx;
    host.microtask = queueMicrotask;
    return host;
}

// A window running scripts, and what it needs to run them.
struct Page {
    JSRuntime* runtime;
    JSContext* ctx;
    PrHtmlPage* page;

    explicit Page(PrStringView markup) : runtime(jsRuntimeCreate()), ctx(JS_NewContext(runtime)), page(nullptr) {
        PrHtmlHost host = hostOf(ctx);
        page = prHtmlPageCreate(&host);
        prJsDomBind(ctx);
        prJsCssBind(ctx);
        prJsSvgBind(ctx);
        prJsMathMlBind(ctx);
        jsHtmlBind(ctx);
        REQUIRE(prHtmlPageLoad(page, markup, "about:blank", nullptr) == PrStatus_Ok);
        REQUIRE(jsHtmlSetGlobal(ctx, prHtmlPageWindow(page)));
    }

    ~Page() {
        JS_FreeContext(ctx);
        jsRuntimeDestroy(runtime);
        prHtmlPageDestroy(page);
    }

    Page(const Page&) = delete;
    Page& operator=(const Page&) = delete;

    // Runs `source`, and returns true if it ran without throwing.
    bool run(const char* source) {
        JSValue result = JS_Eval(ctx, source, PrStringView(source).size, "test", JS_EVAL_TYPE_GLOBAL);
        bool ok = !JS_IsException(result);
        if (!ok) {
            JSValue exception = JS_GetException(ctx);
            const char* text = JS_ToCString(ctx, exception);
            MESSAGE(String(text != nullptr ? text : "an exception"));
            JS_FreeCString(ctx, text);
            JS_FreeValue(ctx, exception);
        }
        JS_FreeValue(ctx, result);
        return ok;
    }

    // Runs the queued microtasks and tasks, and those that they queue.
    void settle() {
        do {
            JSContext* job = nullptr;
            while (JS_ExecutePendingJob(runtime, &job) > 0) {
            }
        } while (prDomAgentRunTask(prHtmlPageAgent(page)));
    }

    // Returns the live allocations after `source` has run, the tasks that it queued have run, and the collector has
    // freed what they left.
    size_t liveAfter(const char* source) {
        CHECK(run(source));
        settle();
        JS_RunGC(runtime);
        return prMemoryLiveCount();
    }

    // Returns the same as liveAfter, for when only reference counting frees memory: the collector never runs.
    size_t liveUncollected(const char* source) {
        CHECK(run(source));
        settle();
        return prMemoryLiveCount();
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// Each churn runs twice. The first run grows reused storage, and the second must leave nothing.
TEST_CASE("nodes a script makes and lets go of are freed, however it mutated them") {
    const char* kChurns[] = {
        "for (let i = 0; i < 200; ++i) { const d = document.createElement('div');"
            " d.appendChild(document.createElement('span')).textContent = 'x'; d.setAttribute('id', 'a' + i); }",
        "for (let i = 0; i < 200; ++i) { const d = document.body.appendChild(document.createElement('div'));"
            " d.appendChild(document.createTextNode('x')); d.remove(); }",
        "for (let i = 0; i < 200; ++i) document.body.innerHTML = '<p><b>x</b><i>y</i></p>';",
        "for (let i = 0; i < 200; ++i) { const a = document.createAttribute('x'); document.body.setAttributeNode(a);"
            " document.body.removeAttributeNode(a); }",
        "for (let i = 0; i < 200; ++i) document.body.cloneNode(true);",
        "for (let i = 0; i < 200; ++i) { const r = document.createRange(); r.selectNodeContents(document.body);"
            " r.cloneContents(); }",
        "const sheet = new CSSStyleSheet(); for (let i = 0; i < 200; ++i) sheet.replaceSync('a { color: red } b {}');",
        "const sheet = new CSSStyleSheet(); for (let i = 0; i < 200; ++i) { sheet.replaceSync('a { color: red }');"
            " sheet.cssRules[0].style.color; }",
        "const sheet = new CSSStyleSheet(); sheet.replaceSync('a {}');"
            " for (let i = 0; i < 200; ++i) { sheet.insertRule('@media print { b { top: 0 } }', 0);"
            " sheet.cssRules[0].cssRules[0].selectorText; sheet.deleteRule(0); }",
        "const style = document.head.appendChild(document.createElement('style'));"
            " for (let i = 0; i < 200; ++i) { style.textContent = 'a { top: ' + (i % 2) + 'px }';"
            " style.sheet.cssRules[0].style.top; } style.remove();",
        "for (let i = 0; i < 200; ++i) { const s = document.head.appendChild(document.createElement('style'));"
            " s.textContent = 'a {}'; s.remove(); }",
        // Trees in which some objects hold state.
        "for (let i = 0; i < 200; ++i) { const d = document.createElement('div');"
            " d.appendChild(document.createElement('p')).appendChild(document.createElement('b')).marker = i; }",
        "const m = new WeakMap(); for (let i = 0; i < 200; ++i) { const d = document.createElement('div');"
            " m.set(d.appendChild(document.createElement('p')), i); }",
        "for (let i = 0; i < 200; ++i) { const d = document.createElement('div'); new WeakRef(d.appendChild(document.createElement('p'))); }",
        "for (let i = 0; i < 200; ++i) { const d = document.body.appendChild(document.createElement('div'));"
            " Object.preventExtensions(d.appendChild(document.createElement('p'))); d.remove(); }",
        "for (let i = 0; i < 200; ++i) { const d = document.createElement('div');"
            " d.appendChild(document.createElement('p')).addEventListener('x', () => {}); }",
        "for (let i = 0; i < 200; ++i) { const t = document.createElement('video').textTracks;"
            " t.onaddtrack = () => {}; t.addEventListener('change', () => {}); }",
        "for (let i = 0; i < 200; ++i) { const v = document.body.appendChild(document.createElement('video'));"
            " v.audioTracks; v.load(); v.remove(); }",
    };
    for (const char* source : kChurns) {
        // A block of its own, so that its declarations do not clash when it runs again.
        String text = format("{{ {} }}", source);
        CAPTURE(text);
        // JS_Eval reads the text through its NUL terminator.
        String terminated = text;
        terminated.append('\0');
        const char* churn = terminated.data();
        Page page("<body></body>");
        page.liveAfter(churn);
        size_t before = page.liveAfter(churn);
        CHECK(page.liveAfter(churn) == before);
    }
}

// An aborted signal holds its reason, so a reason that reaches its signal makes a cycle that only the collector frees.
TEST_CASE("an aborted signal and its reason are freed once script lets go of them, whatever the reason reaches") {
    const char* kChurns[] = {
        "for (let i = 0; i < 200; ++i) new AbortController().abort(new Error('x'));",
        "for (let i = 0; i < 200; ++i) { const c = new AbortController(); c.abort(c.signal); }",
        "for (let i = 0; i < 200; ++i) { const c = new AbortController(); c.abort({ controller: c }); }",
        ("for (let i = 0; i < 200; ++i) { const c = new AbortController(); c.abort({ any: AbortSignal.any([c.signal]) }); }"),
    };
    for (const char* source : kChurns) {
        String text = format("{{ {} }}", source);
        CAPTURE(text);
        // JS_Eval reads the text through its NUL terminator.
        String terminated = text;
        terminated.append('\0');
        const char* churn = terminated.data();
        Page page("<body></body>");
        page.liveAfter(churn);
        size_t before = page.liveAfter(churn);
        CHECK(page.liveAfter(churn) == before);
    }
}

TEST_CASE("what a script puts on a node lasts while it can reach any node of its tree") {
    Page page("<body></body>");
    CHECK(page.run("globalThis.outer = document.createElement('div');"
        "(() => { const inner = outer.appendChild(document.createElement('span'));"
        " inner.marker = 1; inner.addEventListener('x', () => { globalThis.heard = true; }); })();"));
    JS_RunGC(page.runtime);
    CHECK(page.run("if (outer.firstChild.marker !== 1) throw new Error('expando');"
        "outer.firstChild.dispatchEvent(new Event('x'));if (!globalThis.heard) throw new Error('listener');"));
    // A node's object moves with it between trees.
    CHECK(page.run("globalThis.moved = outer.firstChild; document.body.appendChild(moved);"
        "globalThis.outer = null; globalThis.moved = null;"));
    JS_RunGC(page.runtime);
    CHECK(page.run("if (document.body.firstChild.marker !== 1) throw new Error('moved');"));
}

TEST_CASE("what a script puts on a node of the window's document lasts, though script never read the document") {
    // A named property reaches the node without the document's object.
    Page page("<body><div id=target></div></body>");
    CHECK(page.run("target.marker = 1; target.addEventListener('x', () => { globalThis.heard = true; });"));
    JS_RunGC(page.runtime);
    CHECK(page.run("if (target.marker !== 1) throw new Error('expando');"
        "target.dispatchEvent(new Event('x')); if (!globalThis.heard) throw new Error('listener');"));
}

TEST_CASE("an observer with a delivery pending lasts until it runs, however unreachable script leaves it") {
    Page page("<body></body>");
    CHECK(page.run("(() => { const node = document.createTextNode('');"
        " new MutationObserver(() => { globalThis.delivered = true; }).observe(node, {characterData: true}); node.data = 'x'; })();"));
    JS_RunGC(page.runtime);
    page.settle();
    CHECK(page.run("if (!globalThis.delivered) throw new Error('delivered');"));
}

// The objects of a tree that hold nothing that a new object would lack are recreated when a script next requests them.
// So anything that reaches any node of the tree reaches what the script stored on the others.
TEST_CASE("what a script puts on an object lasts while it can reach any node of the tree, whichever it holds") {
    Page page("<body></body>");
    // If the state is held through a leaf, it is an ancestor's state. If it is held through the root, it is a
    // descendant's state.
    CHECK(page.run("(() => { const a = document.createElement('div'); a.marker = 1;"
        " globalThis.leaf = a.appendChild(document.createElement('p')).appendChild(document.createElement('span'));"
        " globalThis.root = document.createElement('div');"
        " root.appendChild(document.createElement('p')).appendChild(document.createElement('span')).marker = 2; })();"));
    JS_RunGC(page.runtime);
    CHECK(page.run("if (leaf.parentNode.parentNode.marker !== 1) throw new Error('ancestor');"
        "if (root.firstChild.firstChild.marker !== 2) throw new Error('descendant');"));
    // A stateful node moved to another tree takes its state with it, and lets its old tree's objects be freed.
    CHECK(page.run("const moved = root.firstChild.firstChild; leaf.appendChild(moved); globalThis.root = null;"));
    JS_RunGC(page.runtime);
    CHECK(page.run("if (leaf.firstChild.marker !== 2) throw new Error('moved');"
        "if (leaf.parentNode.parentNode.marker !== 1) throw new Error('moved ancestor');"));
}

TEST_CASE("a [SameObject] attribute answers one object while script can tell, and what script puts on it lasts") {
    Page page("<body></body>");
    CHECK(page.run("globalThis.element = document.createElement('div');"
        "globalThis.list = element.classList; element.style.probe = 1;"
        "globalThis.sheet = new CSSStyleSheet(); sheet.replaceSync('a { color: red }');"
        "globalThis.rules = sheet.cssRules; globalThis.rule = rules[0]; rule.style.probe = 2;"));
    JS_RunGC(page.runtime);
    CHECK(page.run("if (element.classList !== list || element.style !== element.style) throw new Error('identity');"
        "if (element.style.probe !== 1) throw new Error('style');if (sheet.cssRules !== rules) throw new Error('rules');"
        "if (rule.style.probe !== 2) throw new Error('rule style');"
        "if (element.childNodes !== element.childNodes) throw new Error('childNodes');"));
    // If the attribute's object is the only reference, its owner stays alive.
    CHECK(page.run("globalThis.declarations = element.style; globalThis.element = null; globalThis.list = null;"));
    JS_RunGC(page.runtime);
    CHECK(page.run("if (declarations.probe !== 1) throw new Error('held');"));
}

TEST_CASE("a window's members that script holds stay usable after a new document replaces the window") {
    Page page("<body></body>");
    CHECK(page.run("globalThis.clock = performance; globalThis.orientation = screen.orientation;"
        "globalThis.reduced = navigator.preferences.reducedMotion;"));
    REQUIRE(prHtmlPageLoad(page.page, "<p>next</p>", "about:blank", nullptr) == PrStatus_Ok);
    page.settle();
    JS_RunGC(page.runtime);
    CHECK(page.run("clock.now(); clock.timeOrigin; orientation.type; orientation.angle; reduced.value;"));
}

TEST_CASE("a media element's track lists are its parts: one object each, which keeps the element alive") {
    Page page("<body></body>");
    CHECK(page.run("globalThis.video = document.createElement('video'); video.marker = 1;"
        "globalThis.texts = video.textTracks; texts.onchange = () => { globalThis.changed = true; };"
        "if (video.textTracks !== texts || !(texts instanceof TextTrackList) || !(texts instanceof EventTarget))"
        " throw new Error('textTracks');"
        "if (!(video.audioTracks instanceof AudioTrackList) || !(video.videoTracks instanceof VideoTrackList))"
        " throw new Error('kinds');if (texts.length !== 0 || Array.from(texts).length !== 0 || texts.getTrackById('a') !== null)"
        " throw new Error('empty');if (video.videoTracks.selectedIndex !== -1) throw new Error('selectedIndex');"
        "globalThis.video = null;"));
    JS_RunGC(page.runtime);
    CHECK(page.run("texts.dispatchEvent(new Event('change')); if (!globalThis.changed) throw new Error('handler');"));
}

TEST_CASE("a node's object stays itself while it is a weak reference's target and script reaches its tree") {
    Page page("<body></body>");
    CHECK(page.run("globalThis.map = new WeakMap(); globalThis.set = new WeakSet(); globalThis.freed = false;"
        "globalThis.registry = new FinalizationRegistry(() => { globalThis.freed = true; });"
        "(() => { globalThis.root = document.createElement('div');"
        " const kids = [0, 1, 2, 3].map(() => root.appendChild(document.createElement('span')));"
        " map.set(kids[0], 'v'); set.add(kids[1]); globalThis.ref = new WeakRef(kids[2]); registry.register(kids[3], 0); })();"));
    JS_RunGC(page.runtime);
    page.settle();
    CHECK(page.run("if (map.get(root.children[0]) !== 'v') throw new Error('WeakMap');"));
    CHECK(page.run("if (!set.has(root.children[1])) throw new Error('WeakSet');"));
    CHECK(page.run("if (ref.deref() !== root.children[2]) throw new Error('WeakRef');"));
    CHECK(page.run("if (freed) throw new Error('FinalizationRegistry');"));
}

TEST_CASE("a node's object keeps a prototype, extensibility and private names script gave it") {
    Page page("<body></body>");
    CHECK(page.run("globalThis.proto = Object.create(HTMLElement.prototype);class Base { constructor(o) { return o; } }"
        "globalThis.Field = class extends Base { #mark = 1; static has(o) { return #mark in o; } };"
        "globalThis.Method = class extends Base { #m() {} static has(o) { return #m in o; } };"
        "(() => { globalThis.root = document.createElement('div');"
        " const kids = [0, 1, 2, 3].map(() => root.appendChild(document.createElement('span')));"
        " Object.setPrototypeOf(kids[0], proto); Object.preventExtensions(kids[1]);"
        " new Field(kids[2]); new Method(kids[3]); })();"));
    JS_RunGC(page.runtime);
    CHECK(page.run("if (Object.getPrototypeOf(root.children[0]) !== proto) throw new Error('prototype');"));
    CHECK(page.run("if (Object.isExtensible(root.children[1])) throw new Error('extensible');"));
    CHECK(page.run("if (!Field.has(root.children[2])) throw new Error('private field');"));
    CHECK(page.run("if (!Method.has(root.children[3])) throw new Error('private brand');"));
}

// Each churn runs twice with the collector off. The first run grows reused storage, and the second must leave nothing,
// so reference counting alone frees what it made.
TEST_CASE("a tree whose objects hold no state is freed as script lets go of it, without the collector") {
    const char* kChurns[] = {
        "for (let i = 0; i < 200; ++i) { const d = document.createElement('div');"
            " d.appendChild(document.createElement('span')).appendChild(document.createTextNode('x')); }",
        "for (let i = 0; i < 200; ++i) document.body.appendChild(document.createElement('div'))"
            ".appendChild(document.createElement('span')).parentNode.remove();",
        "for (let i = 0; i < 200; ++i) { const d = document.createElement('div').attachShadow({mode: 'open'});"
            " d.appendChild(document.createElement('span')); }",
        // A stateful node that leaves a tree keeps nothing of that tree alive.
        "globalThis.kept = globalThis.kept || Object.assign(document.createElement('b'), {marker: 1});"
            " for (let i = 0; i < 200; ++i) { const d = document.createElement('div');"
            " d.appendChild(document.createElement('p')).appendChild(kept); kept.remove(); }",
    };
    for (const char* source : kChurns) {
        String text = format("{{ {} }}", source);
        CAPTURE(text);
        // JS_Eval reads the text through its NUL terminator.
        String terminated = text;
        terminated.append('\0');
        const char* churn = terminated.data();
        Page page("<body></body>");
        JS_SetGCThreshold(page.runtime, SIZE_MAX);
        page.liveUncollected(churn);
        size_t before = page.liveUncollected(churn);
        CHECK(page.liveUncollected(churn) == before);
    }
}

TEST_CASE("the engine passes a hyperlink's request for a new browsing context to the host, with its context") {
    String opened;
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &opened;
    host.openWindow = [](void* context, PrHtmlPage*, PrStringView url) {
        static_cast<String*>(context)->assign(url);
    };
    PrJsHtmlEngineDesc desc{};
    PrJsHtmlEngine* engine = prJsHtmlEngineCreate(&desc);
    PrHtmlHost scripted = prJsHtmlEngineHost(engine, &host, PrJsHtmlScripting_Enabled);
    PrHtmlPage* page = prHtmlPageCreate(&scripted);
    REQUIRE(prHtmlPageLoad(page, "<a id=a target=_blank href=next></a>", "https://example.test/", nullptr) ==
        PrStatus_Ok);
    PrDomNode* root = prDomDocumentAsNode(prHtmlWindowDocument(prHtmlPageWindow(page)));
    prHtmlWindowNotifyUserActivation(prHtmlPageWindow(page));
    prHtmlElementClick(prHtmlElementQuery(prDomElementAsEventTarget(prDomNodeGetElementById(root, "a"))));
    CHECK(opened == "https://example.test/next");
    prHtmlPageDestroy(page);
    prJsHtmlEngineDestroy(engine);
}
