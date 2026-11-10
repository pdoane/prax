#include "examples/agent_harness/conversation.h"

#include "pr/app_shell.h"
#include "pr/ascii.h"
#include "pr/callback.h"
#include "pr/charset_quickjs.h"
#include "pr/count_of.h"
#include "pr/css.h"
#include "pr/css_view.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/format.h"
#include "pr/html.h"
#include "pr/html_forms.h"
#include "pr/html_markdown.h"
#include "pr/kit_theme.h"
#include "pr/layout_frame.h"
#include "pr/markdown.h"
#include "pr/mathml_quickjs.h"
#include "pr/mem.h"
#include "pr/print.h"
#include "pr/storage_memory.h"
#include "pr/string_ops.h"
#include "pr/svg_quickjs.h"
#include "pr/time.h"
#include "pr/ui.h"
#include "pr/url_quickjs.h"
#include "pr/xhr_quickjs.h"

namespace harness {

namespace {

// The most of a tool's input or result, or of a record, that an open disclosure shows, in bytes.
constexpr size_t kShownBytes = 6000;

// The bytes that the cache of what the visuals fetch holds.
constexpr uint64_t kCacheBytes = 64 * 1024 * 1024;

// The tools whose calls the Claude app shows as what they make, rather than as calls.
constexpr const char* kPlanTool = "mcp__ccd_turn__update_plan";
constexpr const char* kWrapUpTool = "mcp__ccd_turn__wrap_up";
constexpr const char* kWidgetTool = "mcp__visualize__show_widget";
constexpr const char* kGuideTool = "mcp__visualize__read_me";
constexpr const char* kSendFileTool = "SendUserFile";

// The kinds of part of the conversation, which key them apart.
enum {
    PartKind_User,
    PartKind_Assistant,
    PartKind_Block,
    PartKind_Context,
    PartKind_Event,
    PartKind_Unsent,
};
typedef uint8_t PartKind;

// Returns the key of a part: its kind, the entry or reply that it is of, and the block that it shows, if it shows one.
uint64_t keyOf(PartKind kind, uint32_t entry, uint32_t block = kNone) {
    return (uint64_t(kind) << 60) | (uint64_t(entry & 0x0fffffffu) << 32) | block;
}

PartKind kindOf(uint64_t key) {
    return PartKind(key >> 60);
}

uint32_t entryOf(uint64_t key) {
    return uint32_t(key >> 32) & 0x0fffffffu;
}

uint32_t blockOf(uint64_t key) {
    return uint32_t(key);
}

// Mixes `value` into the hash `h`.
uint64_t mix(uint64_t h, uint64_t value) {
    h ^= value + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

// Returns `text`, cut to `limit` bytes at the start of a character.
PrStringView clipped(PrStringView text, size_t limit) {
    if (text.size <= limit)
        return text;
    size_t size = limit;
    while (size != 0 && (uint8_t(text.data[size]) & 0xc0) == 0x80)
        --size;
    return PrStringView{text.data, size};
}

// Returns the string member `key` of the object at `object`, or the null view.
PrStringView stringIn(const PrJson* json, PrJsonValueIndex object, PrStringView key) {
    PrJsonValueIndex value = prJsonMember(json, object, key);
    return value != kPrJsonMissing ? prJsonString(json, value) : PrStringView{};
}

// Reads a call's input into `json`, and returns its root, or kPrJsonMissing if it is not JSON.
PrJsonValueIndex inputOf(PrJson* json, const Block& call) {
    return prJsonParse(json, call.input, nullptr) ? 0 : kPrJsonMissing;
}

// Returns how a call names its tool: an MCP tool by its server and its tool, as "visualize · show_widget", and any
// other tool by its name.
String toolLabel(PrStringView name) {
    if (!prStringStartsWith(name, "mcp__"))
        return String(name);
    PrStringView rest = prStringFrom(name, 5);
    size_t split = prStringFind(rest, "__");
    if (split == rest.size)
        return String(rest);
    return format("{} · {}", prStringSlice(rest, 0, split), prStringFrom(rest, split + 2));
}

// Returns the text that the user typed in `e`: its text that is not the harness's context, or nothing for a message
// that is not the user's.
String typedText(const Entry& e) {
    String typed;
    if (e.kind != EntryKind_User || e.meta || e.answers)
        return typed;
    for (const Block& block : e.blocks) {
        if (block.kind != BlockKind_Text || block.context)
            continue;
        if (!typed.isEmpty())
            typed.append("\n\n");
        typed.append(block.text);
    }
    return typed;
}

// Returns the result that answers `call`, or null while none has arrived.
const Block* resultOf(const Transcript& t, const Block& call) {
    if (call.result.entry == kNone)
        return nullptr;
    return &t.entries[call.result.entry].blocks[call.result.block];
}

// Applies a call that sets the plan to the plan's state: `items` replaces the plan, and `advance` completes the item in
// progress and starts the next that is pending.
template <typename Plan>
void applyPlan(PrJson* json, Array<Plan>* plan, const Block& call) {
    PrJsonValueIndex input = inputOf(json, call);
    PrJsonValueIndex items = prJsonMember(json, input, "items");
    if (prJsonKind(json, items) == PrJsonKind_Array) {
        plan->clear();
        for (PrJsonValueIndex at = prJsonFirst(json, items); at != kPrJsonMissing; at = prJsonAfter(json, items, at))
            plan->push(Plan{stringIn(json, at, "content"), stringIn(json, at, "status")});
    }
    if (prJsonKind(json, prJsonMember(json, input, "advance")) != PrJsonKind_True)
        return;
    for (Plan& item : *plan) {
        if (item.status == "in_progress")
            item.status = "completed";
    }
    for (Plan& item : *plan) {
        if (item.status == "pending") {
            item.status = "in_progress";
            break;
        }
    }
}

// The page that the conversation shows in: the theme's sheet, which setTheme fills, the conversation's own, and the
// column of its parts. A hyperlink opens in a new top-level browsing context, which the host opens in the user's
// browser, rather than replacing the conversation.
constexpr const char* kPage = R"html(<!DOCTYPE html><html><head><meta charset=utf-8><base target=_blank>
<style id=theme></style><style>
html, body { margin: 0; background: transparent; }
main { box-sizing: border-box; max-width: 820px; margin: 0 auto; padding: 16px; display: flex; flex-direction: column;
    gap: 16px; }
.meta { font: var(--kit-font-meta); letter-spacing: var(--kit-tracking-meta); color: var(--kit-text3); }
.faint { color: var(--kit-text4); }
.user { display: flex; flex-direction: column; align-items: flex-end; gap: 2px; }
.bubble { max-width: 80%; box-sizing: border-box; padding: 8px 12px; border-radius: 8px; background: var(--kit-bg3);
    white-space: pre-wrap; overflow-wrap: anywhere; }
.assistant { display: flex; flex-direction: column; gap: 8px; }
.part { display: flex; flex-direction: column; gap: 4px; }
summary { display: flex; align-items: center; gap: 6px; padding: 2px 4px; border-radius: 4px; list-style: none;
    cursor: default; user-select: none; }
summary:hover { background: var(--kit-bg2); }
summary::before { content: ""; flex: none; width: 5px; height: 5px; margin: 0 3px 0 1px;
    border-right: 1.5px solid var(--kit-text4); border-bottom: 1.5px solid var(--kit-text4); transform: rotate(-45deg); }
details[open] > summary::before { transform: rotate(45deg); }
details > :not(summary) { margin-top: 4px; }
.tool { font: var(--kit-font-strong); white-space: nowrap; }
.about { flex: 1; min-width: 0; overflow: hidden; white-space: nowrap; text-overflow: ellipsis; }
.about.mono { font: var(--kit-font-mono); color: var(--kit-text3); }
.about.small { font: var(--kit-font-body-small); color: var(--kit-text4); }
.badge { flex: none; font: var(--kit-font-meta); padding: 1px 6px; border-radius: 4px; background: var(--kit-bg3);
    color: var(--kit-text2); }
.badge.danger { color: var(--kit-danger); }
.running { flex: none; width: 6px; height: 6px; border-radius: 50%; background: var(--kit-text3); }
pre.block { box-sizing: border-box; margin: 0; padding: 6px 8px; border: 1px solid var(--kit-line1); border-radius: 6px;
    background: var(--kit-bg1); font: var(--kit-font-mono); color: var(--kit-text2); white-space: pre-wrap;
    overflow-wrap: anywhere; }
pre.block.error { color: var(--kit-danger); }
pre.block.primary { color: var(--kit-text1); }
.thought { font: var(--kit-font-body-italic); color: var(--kit-text3); white-space: pre-wrap; padding: 0 4px; }
.card { border: 1px solid var(--kit-line1); border-radius: 8px; background: var(--kit-bg1); }
.plan { display: flex; flex-direction: column; gap: 2px; padding: 8px 12px; }
.plan label { display: flex; align-items: center; gap: 6px; color: var(--kit-text2); }
.plan .completed { color: var(--kit-text3); }
.plan .in_progress { color: var(--kit-accent); }
input[type=checkbox] { appearance: none; flex: none; width: 13px; height: 13px; margin: 0; border-radius: 3px;
    border: 1px solid var(--kit-line3); background: transparent; }
input[type=checkbox]:checked { border-color: var(--kit-accent); background: var(--kit-accent) url("data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 14 14'%3E%3Cpath d='M3 7.5l2.5 2.5L11 4.5' fill='none' stroke='white' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'/%3E%3C/svg%3E") center / 100% no-repeat; }
.widget > .card { padding: 16px; }
.widget iframe { display: block; width: 100%; height: 0; border: 0; }
.wrapup { display: flex; flex-direction: column; gap: 8px; padding: 12px 14px; }
.recap { color: var(--kit-text3); white-space: pre-wrap; }
.ask { font: var(--kit-font-strong); }
.actions { display: flex; flex-wrap: wrap; gap: 6px; }
button { font: var(--kit-font-body-small); color: var(--kit-text1); background: var(--kit-bg3);
    border: 1px solid var(--kit-line3); border-radius: 4px; padding: 3px 10px; }
button:hover { background: var(--kit-bg4); }
.file { align-self: flex-start; display: flex; align-items: center; gap: 8px; padding: 6px 10px; border-radius: 6px;
    border: 1px solid var(--kit-line1); background: var(--kit-bg3); white-space: nowrap; }
.markdown { contain: content; }
</style></head><body><main id=column></main></body></html>)html";

// The style of a response's Markdown, which each response's shadow root adopts: a chat's message, in the theme's
// colors, which the custom properties of the page around it carry in.
constexpr const char* kMarkdownCss = R"css(
:host { display: block; }
.body { font: 13px/1.6 var(--kit-family-ui); color: var(--kit-text1); overflow-wrap: anywhere; }
p, ul, ol, pre, table, blockquote, hr { margin: 0 0 0.8em; }
h1, h2, h3, h4, h5, h6 { margin: 1.2em 0 0.5em; font-weight: 600; line-height: 1.3; }
h1 { font-size: 1.45em; } h2 { font-size: 1.25em; } h3 { font-size: 1.1em; } h4, h5, h6 { font-size: 1em; }
.body > :first-child { margin-top: 0; }
.body > :last-child { margin-bottom: 0; }
a { color: var(--kit-accent); }
ul, ol { padding-left: 1.6em; }
li > ul, li > ol { margin: 0; }
li > p { margin: 0; }
code { font-family: var(--kit-family-mono); font-size: 0.9em; background: var(--kit-bg3); padding: 0.1em 0.35em;
    border-radius: 4px; }
pre { background: var(--kit-bg1); border: 1px solid var(--kit-line1); border-radius: 6px; padding: 10px 12px;
    overflow-x: auto; line-height: 1.45; }
pre code { background: none; padding: 0; border-radius: 0; font-size: 12px; }
blockquote { border-left: 3px solid var(--kit-line3); padding-left: 12px; color: var(--kit-text2); }
table { border-collapse: collapse; }
th, td { border-bottom: 1px solid var(--kit-line1); padding: 6px 16px 6px 0; text-align: left; vertical-align: top; }
th { font-weight: 600; }
hr { border: 0; border-top: 1px solid var(--kit-line1); }
del { color: var(--kit-text3); }
li:has(> input[type=checkbox]) { list-style: none; }
ul:has(> li > input[type=checkbox]) { padding-left: 0.2em; }
input[type=checkbox] { appearance: none; display: inline-block; width: 13px; height: 13px; margin: 0 0.5em 0 0;
    border: 1px solid var(--kit-line3); border-radius: 3px; vertical-align: -2px; background: transparent; }
input[type=checkbox]:checked { border-color: var(--kit-accent); background: var(--kit-accent); }
li:has(> input[type=checkbox]:checked) { color: var(--kit-text3); text-decoration: line-through; }
img, svg { max-width: 100%; }
)css";

// What a visual loads with, ahead of its own code: the theme, the variables that the Claude app gives its visuals, and
// their classes; the root's data-mode, dark or light; and sendPrompt, which posts what the visual asks to the
// conversation.
constexpr const char* kWidgetCss = R"css(
:root { --text: var(--kit-text1); --secondary: var(--kit-text2); --muted: var(--kit-text3); --accent: var(--kit-accent);
    --text-primary: var(--kit-text1); --text-secondary: var(--kit-text2); --surface-1: var(--kit-bg2); --radius: 8px; }
html, body { margin: 0; background: transparent; }
body { font: 13px/1.6 var(--kit-family-ui); color: var(--kit-text1); }
.sr-only { position: absolute; width: 1px; height: 1px; overflow: hidden; clip-path: inset(50%); white-space: nowrap; }
svg { display: block; max-width: 100%; height: auto; }
svg text { font-family: var(--kit-family-ui); fill: var(--kit-text1); }
.th { font-size: 14px; font-weight: 600; }
.ts { font-size: 12px; fill: var(--kit-text2); }
.node > rect, .node > circle, .node > ellipse { fill: var(--fill); stroke: var(--stroke); }
.node > text { fill: var(--ink); }
.c-gray { --fill: #45443f; --stroke: #7a7870; --ink: #e8e6df; }
.c-purple { --fill: #3c3489; --stroke: #7f77dd; --ink: #eeedfe; }
.c-blue { --fill: #1f3b70; --stroke: #4a7fd8; --ink: #dbe7fb; }
.c-teal { --fill: #124f42; --stroke: #2e9b7f; --ink: #c8eedf; }
.c-green { --fill: #25502b; --stroke: #4e9a57; --ink: #dff2e2; }
.c-amber { --fill: #5a4410; --stroke: #c8952a; --ink: #faecc9; }
.c-red { --fill: #5c2222; --stroke: #d0574f; --ink: #fadbd8; }
)css";

constexpr const char* kWidgetScript =
    "<script>window.sendPrompt = text => parent.postMessage({type: 'prompt', text: String(text)}, '*');</script>";

// Prints what a visual's script logs, and the errors that it throws, to the standard error.
constexpr PrJsHtmlConsolePrinter kConsole = {
    [](JSContext*, PrJsHtmlConsoleLevel, uint32_t depth, PrStringView text) {
        printError("agent_harness: visual: {:{}}{}\n", "", depth * 2, text);
    },
};

double now(void* PR_NULLABLE) {
    return prTimeMonotonicMilliseconds();
}

PrDomNode* nodeOf(PrDomElement* element) {
    return prDomElementAsNode(element);
}

// Gives `element` its class and its text, each unless it is empty.
PrDomElement* decorated(PrDomElement* element, PrStringView className, PrStringView text) {
    if (className.size != 0)
        prDomElementSetAttribute(element, "class", className);
    if (text.size != 0)
        prDomNodeSetTextContent(nodeOf(element), text);
    return element;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// The page

Conversation::Conversation() : agent(prDomAgentCreate(nullptr)), display(prAppDisplayDefault()),
        preferences(prAppUserPreferencesDefault()), json(prJsonCreate()) {
    // The visuals see what a browser's pages see. They reach the conversation by postMessage alone.
    PrJsHtmlBind* const binds[] = {prJsSvgBind, prJsMathMlBind, prJsUrlBind, prJsCharsetBind, prJsXhrBind};
    PrJsHtmlEngineDesc desc{.binds = binds, .bindCount = uint32_t(countOf(binds)), .console = &kConsole};
    engine = prJsHtmlEngineCreate(&desc);
    // A task that a fetch posts from the network's loop wakes the host to run it.
    prDomAgentSetWake(agent, [](void* PR_NULLABLE context) {
        Conversation* c = static_cast<Conversation*>(context);
        if (c->wake != nullptr)
            c->wake(c->hostContext);
    }, this);
    net = prNetCreate(PrNetBackend_Default);
    tls = net != nullptr ? prTlsCreate(PrTlsBackend_Default, net) : nullptr;
    cache = prStorageMemoryCreate(kCacheBytes, PrStorageEviction_LeastRecentlyUsed);
    if (net != nullptr) {
        PrHttpDesc httpDesc{.net = net, .tls = tls};
        http = prHttpCreate(PrHttpBackend_Default, &httpDesc);
        PrFetchNetworkDesc fetch{.net = net, .http = http, .cache = cache};
        network = prFetchNetworkCreate(PrFetchNetworkBackend_Default, &fetch);
    }
}

// The engine goes before the pages that it ran scripts for, and the fetchers and HTTP are freed on the network's loop.
Conversation::~Conversation() {
    prJsHtmlEngineDestroy(engine);
    destroyPage();
    prDomAgentDestroy(agent);
    if (net != nullptr) {
        auto destroyOnLoop = [&] {
            prFetchNetworkDestroy(network);
            prHttpDestroy(http);
        };
        prNetPostBlocking(net, PR_SCOPED_CALLBACK(destroyOnLoop));
    }
    prTlsDestroy(tls);
    prNetDestroy(net);
    prStorageDestroy(cache);
    prJsonDestroy(json);
}

void Conversation::setText(PrTextFont* textFont, PrTextShaper* textShaper, PrGpuDevice* PR_NULLABLE gpuDevice) {
    font = textFont;
    shaper = textShaper;
    device = gpuDevice;
    makePage();
}

void Conversation::setWindow(PrAppWindow* PR_NULLABLE to) {
    window = to;
    if (view != nullptr)
        prHtmlViewSetWindow(view, window);
}

void Conversation::setHost(ConversationWakeFn wakeFn, ConversationReplyFn replyFn, void* PR_NULLABLE context) {
    wake = wakeFn;
    reply = replyFn;
    hostContext = context;
}

void Conversation::makePage() {
    destroyPage();
    PrHtmlHost host = prHtmlHostDefault();
    host.context = this;
    host.agent = agent;
    host.font = font;
    host.shaper = shaper;
    host.now = now;
    host.device = device;
    host.fetcher = network;
    // The page runs no script. Its iframes, the visuals, run theirs in the engine, as far as their sandbox allows.
    host.frame = frameHost;
    // A hyperlink opens in the user's browser.
    host.openWindow = [](void* PR_NULLABLE context, PrHtmlPage*, PrStringView url) {
        Conversation* c = static_cast<Conversation*>(context);
        if (c->window != nullptr)
            prAppOpenUrl(prAppWindowApp(c->window), url);
    };
    page = prHtmlPageCreate(&host);
    prHtmlPageLoad(page, kPage, "about:conversation", nullptr);
    PrDomNode* root = prDomDocumentAsNode(document());
    column = prDomNodeGetElementById(root, "column");
    themeStyle = prDomNodeGetElementById(root, "theme");
    if (themeStyle != nullptr)
        prDomNodeSetTextContent(nodeOf(themeStyle), themeCss);
    prCssStyleSheetCreate(document(), nullptr, &markdownSheet);
    prCssStyleSheetReplaceSync(markdownSheet, kMarkdownCss);
    // The window hears each toggle of a disclosure as it captures it, and each click and message as it bubbles.
    PrDomEventTarget* target = prHtmlWindowAsEventTarget(prHtmlPageWindow(page));
    PrDomEventListener toggled{
        .invoke = [](void* PR_NULLABLE context, PrDomEvent* event) { static_cast<Conversation*>(context)->opened(event); },
        .context = this,
        .identity = 1,
    };
    PrDomAddEventListenerOptions capture = prDomAddEventListenerOptionsDefault(target, "toggle");
    capture.capture = true;
    prDomEventTargetAddEventListener(target, "toggle", &toggled, &capture);
    PrDomEventListener clickedListener{
        .invoke =
            [](void* PR_NULLABLE context, PrDomEvent* event) { static_cast<Conversation*>(context)->clicked(event); },
        .context = this,
        .identity = 2,
    };
    prDomEventTargetAddEventListener(target, "click", &clickedListener, nullptr);
    PrDomEventListener messagedListener{
        .invoke =
            [](void* PR_NULLABLE context, PrDomEvent* event) { static_cast<Conversation*>(context)->messaged(event); },
        .context = this,
        .identity = 3,
    };
    prDomEventTargetAddEventListener(target, "message", &messagedListener, nullptr);
    // The window's tree draws the canvas beneath the page, so the page draws none.
    PrHtmlViewDesc desc{.page = page, .canvasFilled = true};
    view = prHtmlViewCreate(&desc);
    prHtmlViewSetDisplay(view, &display);
    prHtmlViewSetUserPreferences(view, &preferences);
    prHtmlViewSetWindow(view, window);
    shownKey = ~uint64_t(0);
    ++revision;
}

// Every iframe, at any depth, runs its scripts in the engine, as far as its sandbox allows.
void Conversation::frameHost(void* PR_NULLABLE context, PrHtmlPage*, PrHtmlHost* child) {
    Conversation* c = static_cast<Conversation*>(context);
    child->context = context;
    child->frame = frameHost;
    child->fetcher = c->network;
    *child = prJsHtmlEngineHost(c->engine, child, PrJsHtmlScripting_Enabled);
}

void Conversation::destroyPage() {
    for (const Item& item : items.values())
        prDomNodeRelease(nodeOf(item.element));
    items.clear();
    widgets.clear();
    prCssStyleSheetRelease(markdownSheet);
    markdownSheet = nullptr;
    prHtmlViewDestroy(view);
    prHtmlPageDestroy(page);
    view = nullptr;
    page = nullptr;
    column = nullptr;
    themeStyle = nullptr;
}

PrDomDocument* Conversation::document() {
    return prHtmlWindowDocument(prHtmlPageWindow(page));
}

void Conversation::setTheme(const PrKitTheme& theme) {
    // The theme is set at each frame, and is made into CSS only when its colors or its type change.
    if (themed && prMemEquals(&theme.palette, &palette, sizeof palette) && prMemEquals(theme.types, types, sizeof types))
        return;
    themed = true;
    palette = theme.palette;
    prMemCopy(types, theme.types, sizeof types);
    PrRawString css = prKitThemeCss(&theme);
    PrStringView text = prRawStringView(&css);
    bool changed = text != themeCss.view();
    if (changed)
        themeCss = text;
    prRawStringReset(&css);
    const PrKitPalette& p = theme.palette;
    dark = 0.2126f * p.bg0.r + 0.7152f * p.bg0.g + 0.0722f * p.bg0.b < 0.5f;
    if (!changed)
        return;
    if (themeStyle != nullptr)
        prDomNodeSetTextContent(nodeOf(themeStyle), themeCss);
    ++themeGeneration;
    ++revision;
}

void Conversation::setDisplay(const PrAppDisplay& to, const PrAppUserPreferences& preferred) {
    if (to.pixelRatio == display.pixelRatio && preferred.colorScheme == preferences.colorScheme)
        return;
    display = to;
    preferences = preferred;
    if (view != nullptr) {
        prHtmlViewSetDisplay(view, &display);
        prHtmlViewSetUserPreferences(view, &preferences);
    }
    ++revision;
}

PrLayoutNode Conversation::place(PrKit* kit, PrLayoutNode parent, uint64_t key) {
    node = prLayoutTreeFrameReplaced(kit->tree, parent, key, 0);
    PrKitPlace fill{.flex = PrKitFlex_Fill};
    prKitPlace(kit, node, &fill);
    PrLayoutContent content{.kind = PrLayoutContentKind_Painter, .revision = revision};
    if (view != nullptr)
        content.painter = prHtmlViewViewportPainter(view);
    prLayoutTreeFrameSetContent(kit->tree, node, &content);
    // The pointer shows what the page shows under it, as of the page's last hover.
    if (view != nullptr)
        prUiSetCursor(kit->ui, node, prHtmlViewOutput(view).cursor);
    return node;
}

void Conversation::runTasks() {
    bool ran = false;
    while (prDomAgentNextTaskDelay(agent) == 0 && prDomAgentRunTask(agent))
        ran = true;
    prJsHtmlEngineCheckpoint(engine);
    if (!ran)
        return;
    // A task may have changed the page, or a visual, which may have changed its size.
    bool end = atEnd();
    fitWidgets();
    followEnd(end);
    ++revision;
}

double Conversation::nextTask() {
    return prDomAgentNextTaskDelay(agent);
}

float Conversation::measure(float width) {
    prHtmlViewSetViewport(view, PrVec2{width, 800});
    prHtmlViewPrepare(view);
    PrDomElement* root = prDomDocumentElement(document());
    return root != nullptr ? float(prCssElementScrollHeight(root)) : 0.0f;
}

void Conversation::paint(PrVec2 extent, PrDrawList* list) {
    prHtmlViewSetViewport(view, extent);
    prHtmlViewPaint(view, list);
}

void Conversation::scrollTo(double top) {
    PrDomElement* root = prDomDocumentElement(document());
    if (root == nullptr)
        return;
    PrCssScrollToOptions to{.hasTop = true, .top = top >= 0 ? top : double(prCssElementScrollHeight(root))};
    prHtmlWindowScroll(prHtmlPageWindow(page), &to);
    ++revision;
}

// ---------------------------------------------------------------------------------------------------------------------
// Input

bool Conversation::event(PrLayoutTree* tree, const PrAppEvent& e) {
    if (view == nullptr || node == kPrLayoutNodeNone || !prLayoutNodeIsValid(tree, node))
        return false;
    PrRect box = prLayoutRectToPx(prLayoutNodeBounds(tree, node, PrLayoutCoordinates_Viewport));
    PrVec2 origin{box.left, box.top};
    PrVec2 at{};
    bool placed = prAppEventPosition(&e, &at);
    bool inside = placed && at.x >= box.left && at.x < box.right && at.y >= box.top && at.y < box.bottom;
    auto send = [&] {
        PrAppEvent moved = prAppEventPlaced(&e, origin, 1);
        prHtmlViewEvent(view, &moved, 0);
        ++revision;
    };
    switch (e.kind) {
        case PrAppEventKind_PointerDown:
            focused = inside;
            if (!inside)
                return false;
            buttons |= 1u << e.pointer.button;
            pressed = true;
            hovered = true;
            send();
            return false;
        case PrAppEventKind_PointerUp:
        case PrAppEventKind_PointerCancel:
            if (!pressed && !inside)
                return false;
            buttons &= ~(1u << e.pointer.button);
            pressed = buttons != 0 && e.kind == PrAppEventKind_PointerUp;
            send();
            return false;
        case PrAppEventKind_PointerMove:
            // The page sees the pointer leave it, and nothing more until it comes back.
            if (!inside && !pressed) {
                if (hovered) {
                    PrAppEvent leave = prAppEventPlaced(&e, origin, 1);
                    leave.kind = PrAppEventKind_PointerLeave;
                    prHtmlViewEvent(view, &leave, 0);
                    hovered = false;
                    ++revision;
                }
                return false;
            }
            hovered = inside;
            send();
            return false;
        case PrAppEventKind_PointerLeave:
            if (hovered || pressed)
                send();
            hovered = false;
            return false;
        case PrAppEventKind_Scroll:
            if (!inside)
                return false;
            send();
            return true;
        case PrAppEventKind_KeyDown:
        case PrAppEventKind_KeyUp:
        case PrAppEventKind_Text:
        case PrAppEventKind_Paste:
        case PrAppEventKind_CompositionUpdate:
        case PrAppEventKind_CompositionEnd:
            if (!focused)
                return false;
            send();
            return true;
        default:
            return false;
    }
}

// A disclosure that opens fills its body, which a closed one does not build. The open ones stay open as the page
// changes.
void Conversation::opened(PrDomEvent* event) {
    PrDomElement* details = prDomElementQuery(prDomEventTarget(event));
    if (details == nullptr)
        return;
    PrStringView part = prDomElementGetAttribute(details, "data-part");
    if (part.data == nullptr)
        return;
    uint64_t key = 0;
    for (size_t i = 0; i < part.size; ++i)
        key = key * 10 + uint64_t(part.data[i] - '0');
    bool isOpen = prDomElementHasAttribute(details, "open");
    if (isOpen) {
        open.add(key);
        if (prDomNodeLastChild(nodeOf(details)) == prDomNodeFirstChild(nodeOf(details)))
            fill(key, details);
    } else {
        open.erase(key);
    }
    ++revision;
}

// A click on an answer that a turn's close offers replies with it.
void Conversation::clicked(PrDomEvent* event) {
    PrDomElement* target = prDomElementQuery(prDomEventTarget(event));
    if (target == nullptr || reply == nullptr)
        return;
    PrStringView answer = prDomElementGetAttribute(target, "data-reply");
    if (answer.data != nullptr)
        reply(hostContext, answer);
}

// A visual's sendPrompt posts {type: 'prompt', text}, which replies with the text.
void Conversation::messaged(PrDomEvent* event) {
    PrHtmlMessageEvent* message = prHtmlMessageEventQuery(event);
    PrRawString text{};
    if (message == nullptr || !prJsHtmlEngineMessageJson(engine, prHtmlMessageEventSerializedData(message), &text))
        return;
    if (prJsonParse(json, prRawStringView(&text), nullptr) && stringIn(json, 0, "type") == "prompt" && reply != nullptr)
        reply(hostContext, stringIn(json, 0, "text"));
    prRawStringReset(&text);
}

// ---------------------------------------------------------------------------------------------------------------------
// Building

PrDomElement* Conversation::make(PrStringView tag, PrStringView className, PrStringView text) {
    PrDomElement* element = nullptr;
    prDomElementCreate(document(), tag, nullptr, &element);
    return decorated(element, className, text);
}

PrDomElement* Conversation::add(PrDomNode* parent, PrStringView tag, PrStringView className, PrStringView text) {
    PrDomElement* element = nullptr;
    prDomNodeAppendElement(parent, tag, nullptr, &element);
    return decorated(element, className, text);
}

PrDomElement* Conversation::add(PrDomElement* parent, PrStringView tag, PrStringView className, PrStringView text) {
    return add(nodeOf(parent), tag, className, text);
}

// Places the part `key` at the cursor: the subtree that the conversation keeps for it if it was built from what
// `signature` says, or else a new one that `build` makes in its place.
template <typename Build>
PrDomElement* Conversation::put(Cursor& cursor, uint64_t key, uint64_t signature, Build build) {
    Item* item = items.get(key);
    if (item != nullptr && item->signature != signature) {
        forget(key);
        item = nullptr;
    }
    if (item == nullptr) {
        // The new subtree's reference is the conversation's.
        item = &items.set(key, Item{.element = build(), .signature = signature});
    }
    item->generation = generation;
    PrDomNode* element = nodeOf(item->element);
    if (element != cursor.next)
        prDomNodeInsertBefore(cursor.parent, element, cursor.next);
    cursor.next = prDomNodeNextSibling(element);
    return item->element;
}

// Removes the part `key` from the page, and lets go of its subtree.
void Conversation::forget(uint64_t key) {
    Item* item = items.get(key);
    if (item == nullptr)
        return;
    prDomNodeRemove(nodeOf(item->element));
    prDomNodeRelease(nodeOf(item->element));
    items.erase(key);
    widgets.erase(key);
}

// Makes a disclosure for the part `key`: a details element whose summary the caller fills, and whose body fill() makes
// once it opens. Writes the summary to `summary`.
PrDomElement* Conversation::disclosure(uint64_t key, PrStringView className, PrDomElement* PR_NULLABLE* PR_NONNULL summary) {
    PrDomElement* details = make("details", className);
    char part[24];
    prDomElementSetAttribute(details, "data-part", formatToBuffer(Span(part), "{}", key));
    *summary = add(details, "summary");
    if (open.contains(key)) {
        prDomElementSetAttribute(details, "open", "");
        fill(key, details);
    }
    return details;
}

void Conversation::preformatted(PrDomElement* parent, PrStringView text, PrStringView className) {
    PrStringView shown = clipped(text, kShownBytes);
    add(parent, "pre", className, shown);
    if (shown.size < text.size) {
        char more[64];
        add(parent, "div", "meta", formatToBuffer(Span(more), "{} more bytes", text.size - shown.size));
    }
}

// Fills the body of the disclosure of part `key`: a call's input and result, a thought, or one of the harness's
// records.
void Conversation::fill(uint64_t key, PrDomElement* details) {
    const Transcript& t = *model.transcript;
    uint32_t entry = entryOf(key);
    if (entry >= t.entries.count())
        return;
    const Entry& e = t.entries[entry];
    switch (kindOf(key)) {
        case PartKind_Block: {
            if (blockOf(key) >= e.blocks.count())
                return;
            const Block& block = e.blocks[blockOf(key)];
            if (block.kind == BlockKind_Thinking) {
                add(details, "div", "thought", block.text);
                return;
            }
            preformatted(details, block.input, "block");
            if (const Block* result = resultOf(t, block))
                preformatted(details, result->text, result->error ? "block error" : "block primary");
            return;
        }
        case PartKind_Context:
            if (blockOf(key) < e.blocks.count())
                preformatted(details, e.blocks[blockOf(key)].text, "block");
            return;
        case PartKind_Event:
            for (const RecordRange& range : e.records)
                preformatted(details, t.record(range), "block");
            return;
        default:
            return;
    }
}

// Sets `text` as Markdown in a shadow root of `host`. Raw HTML shows as it is written, with GFM's disallowed tags
// escaped, and styles nothing outside the root. No script runs in the page, and a destination that can run code or read
// local files is dropped.
void Conversation::markdown(PrDomElement* host, PrStringView text) {
    PrDomShadowRootInit init = prDomShadowRootInitDefault(PrDomShadowRootMode_Open);
    PrDomShadowRoot* shadow = nullptr;
    if (prDomElementAttachShadow(host, &init, &shadow) != PrStatus_Ok)
        return;
    // Every response's root adopts the one Markdown sheet.
    prCssShadowRootSetAdoptedStyleSheets(shadow, &markdownSheet, 1);
    PrDomElement* body = add(prDomShadowRootAsNode(shadow), "div", "body");
    PrMarkdownOptions options = prMarkdownOptionsDefault();
    options.rawHtml = PrMarkdownRawHtml_Filter;
    options.destinations = PrMarkdownDestinations_Safe;
    prHtmlSetMarkdown(body, text, &options);
}

// Appends the iframe of a visual to `parent`: its code after the theme, sandboxed to scripts of an opaque origin of its
// own.
PrDomElement* Conversation::widget(PrDomElement* parent, PrStringView code) {
    PrDomElement* frame = add(parent, "iframe");
    String source = format("<!DOCTYPE html><html data-mode={}><meta charset=utf-8><style>{}{}</style>{}",
        dark ? "dark" : "light", themeCss, kWidgetCss, kWidgetScript);
    source.append(code);
    prDomElementSetAttribute(frame, "sandbox", "allow-scripts");
    prDomElementSetAttribute(frame, "title", "visual");
    prDomElementSetAttribute(frame, "srcdoc", source);
    return frame;
}

// Makes a call as what it makes, for the tools that the Claude app shows that way, or as a call that opens to its input
// and its result. Returns null for a call that shows nothing.
PrDomElement* Conversation::buildCall(uint32_t entry, uint32_t index) {
    const Transcript& t = *model.transcript;
    const Block& call = t.entries[entry].blocks[index];
    PrStringView name = call.name;
    uint64_t key = keyOf(PartKind_Block, entry, index);
    PrDomElement* part = make("div", "part");
    if (name == kPlanTool && plan.entry == entry && plan.block == index) {
        PrDomElement* card = add(part, "div", "card plan");
        for (const PlanItem& item : planItems) {
            PrDomElement* row = add(card, "label", item.status);
            PrDomElement* check = add(row, "input");
            prDomElementSetAttribute(check, "type", "checkbox");
            prDomElementSetAttribute(check, "disabled", "");
            if (item.status == "completed")
                prDomElementSetAttribute(check, "checked", "");
            add(row, "span", {}, item.content);
        }
        return part;
    }
    if (name == kWidgetTool) {
        PrJsonValueIndex input = inputOf(json, call);
        prDomElementSetAttribute(part, "class", "part widget");
        add(part, "div", "meta", stringIn(json, input, "title"));
        PrDomElement* card = add(part, "div", "card");
        widgets.set(key, widget(card, stringIn(json, input, "widget_code")));
        return part;
    }
    if (name == kWrapUpTool) {
        PrJsonValueIndex input = inputOf(json, call);
        PrDomElement* card = add(part, "div", "card wrapup");
        add(card, "div", "recap", stringIn(json, input, "recap"));
        PrStringView ask = stringIn(json, input, "ask");
        if (ask.size != 0)
            add(card, "div", "ask", ask);
        PrJsonValueIndex actions = prJsonMember(json, input, "actions");
        bool answered = (typed != kNone && typed > entry) || !model.unsent->isEmpty();
        if (answered || prJsonCount(json, actions) == 0)
            return part;
        PrDomElement* row = add(card, "div", "actions");
        for (PrJsonValueIndex at = prJsonFirst(json, actions); at != kPrJsonMissing; at = prJsonAfter(json, actions, at)) {
            PrStringView label = stringIn(json, at, "label");
            PrDomElement* button = add(row, "button", {}, label);
            prDomElementSetAttribute(button, "data-reply", label);
        }
        return part;
    }
    if (name == kSendFileTool) {
        PrJsonValueIndex input = inputOf(json, call);
        PrStringView caption = stringIn(json, input, "caption");
        if (caption.size != 0)
            add(part, "div", {}, caption);
        PrJsonValueIndex files = prJsonMember(json, input, "files");
        for (PrJsonValueIndex at = prJsonFirst(json, files); at != kPrJsonMissing; at = prJsonAfter(json, files, at)) {
            PrStringView file = prFilePathName(prJsonString(json, at));
            PrDomElement* card = add(part, "div", "file");
            // The file's type is its extension in capitals, without its dot.
            PrStringView extension = prFilePathExtension(file);
            String type;
            for (size_t i = extension.size != 0 ? 1 : 0; i < extension.size; ++i)
                type.append(prAsciiUpper(extension.data[i]));
            if (!type.isEmpty())
                add(card, "span", "badge", type);
            add(card, "span", {}, file);
        }
        return part;
    }
    if (!model.events && (name == kPlanTool || name == kGuideTool)) {
        prDomNodeRelease(nodeOf(part));
        return nullptr;
    }
    PrDomElement* summary = nullptr;
    PrDomElement* details = disclosure(key, {}, &summary);
    prDomNodeAppendChild(nodeOf(part), nodeOf(details));
    prDomNodeRelease(nodeOf(details));
    add(summary, "span", "tool", toolLabel(name));
    add(summary, "span", "about mono", call.summary);
    const Block* result = resultOf(t, call);
    if (result == nullptr)
        add(summary, "span", "running");
    else if (result->error)
        add(summary, "span", "badge danger", "error");
    return part;
}

// Places the blocks of an assistant's reply in its column, each keyed by its index, so that a block keeps its subtree
// as the blocks after it arrive.
void Conversation::showAssistant(PrDomElement* message, uint32_t entry) {
    const Transcript& t = *model.transcript;
    const Entry& e = t.entries[entry];
    Cursor cursor{nodeOf(message), prDomNodeFirstChild(nodeOf(message))};
    for (uint32_t i = 0; i < e.blocks.count(); ++i) {
        const Block& block = e.blocks[i];
        uint64_t key = keyOf(PartKind_Block, entry, i);
        switch (block.kind) {
            case BlockKind_Text:
                put(cursor, key, mix(1, block.text.count()), [&] {
                    PrDomElement* host = make("div", "markdown");
                    markdown(host, block.text);
                    return host;
                });
                break;
            case BlockKind_Thinking: {
                // Thinking whose text the model withheld shows only while the harness's records do.
                if (block.text.isEmpty() && !model.events)
                    break;
                put(cursor, key, mix(2, block.text.isEmpty()), [&] {
                    if (block.text.isEmpty())
                        return make("div", "meta faint", "Thought");
                    PrDomElement* summary = nullptr;
                    PrDomElement* details = disclosure(key, "part", &summary);
                    add(summary, "span", "meta", "Thinking");
                    return details;
                });
                break;
            }
            case BlockKind_ToolUse: {
                const Block* result = resultOf(t, block);
                bool planShown = plan.entry == entry && plan.block == i;
                bool answered = (typed != kNone && typed > entry) || !model.unsent->isEmpty();
                uint64_t signature = mix(mix(mix(mix(3, result != nullptr), result != nullptr && result->error),
                    mix(planShown, answered)), mix(model.events, block.name == kWidgetTool ? themeGeneration : 0));
                // The plan that shows changes as later calls advance it.
                if (planShown) {
                    for (const PlanItem& item : planItems) {
                        for (char ch : item.status.view())
                            signature = mix(signature, uint8_t(ch));
                        signature = mix(signature, item.content.count());
                    }
                }
                Item* kept = items.get(key);
                if (kept != nullptr && kept->signature == signature) {
                    put(cursor, key, signature, [] { return static_cast<PrDomElement*>(nullptr); });
                    break;
                }
                PrDomElement* built = buildCall(entry, i);
                if (built == nullptr)
                    break;
                put(cursor, key, signature, [&] { return built; });
                break;
            }
            case BlockKind_Image:
            case BlockKind_ToolResult:
            case BlockKind_Other:
                put(cursor, key, mix(4, block.kind), [&] {
                    PrDomElement* badge = make("div", "part");
                    add(badge, "span", "badge", block.kind == BlockKind_Image ? block.text.view() : block.name.view());
                    return badge;
                });
                break;
            default:
                break;
        }
    }
}

// Places an entry of the conversation, or nothing for a user message that only answers tool calls, whose results show
// with their calls. While the harness's records show, the context in a user's message shows before what the user typed,
// and the harness's own records show as a line that opens to the record.
void Conversation::showEntry(Cursor& cursor, uint32_t entry) {
    const Transcript& t = *model.transcript;
    const Entry& e = t.entries[entry];
    String text = typedText(e);
    bool message = !text.isEmpty() || e.kind == EntryKind_Assistant;
    if (e.answers || (!message && !model.events))
        return;
    if (model.events && e.kind == EntryKind_User && !e.meta) {
        for (uint32_t i = 0; i < e.blocks.count(); ++i) {
            if (!e.blocks[i].context)
                continue;
            uint64_t key = keyOf(PartKind_Context, entry, i);
            put(cursor, key, 5, [&] {
                PrStringView text = prStringTrim(e.blocks[i].text);
                size_t end = 1;
                while (end < text.size && text.data[end] != '>' && text.data[end] != ' ')
                    ++end;
                size_t inside = prStringFindByte(text, '>');
                PrStringView about = inside < text.size ? prStringTrim(prStringFrom(text, inside + 1)) : PrStringView{};
                about = prStringSlice(about, 0, prStringFindByte(about, '\n'));
                PrDomElement* summary = nullptr;
                PrDomElement* details = disclosure(key, "part", &summary);
                add(summary, "span", "meta", prStringSlice(text, 1, end - 1));
                add(summary, "span", "about small", clipped(about, 200));
                return details;
            });
        }
        if (text.isEmpty())
            return;
    }
    if (!message) {
        uint64_t key = keyOf(PartKind_Event, entry);
        put(cursor, key, 6, [&] {
            PrStringView kind =
                !e.subtype.isEmpty() ? e.subtype.view() : e.meta ? PrStringView("context") : e.type.view();
            PrStringView about = e.summary;
            if (about.size == 0 && e.blocks.count() != 0)
                about = e.blocks[0].text;
            PrDomElement* summary = nullptr;
            PrDomElement* details = disclosure(key, "part", &summary);
            add(summary, "span", "meta", kind);
            add(summary, "span", "about small", clipped(about, 200));
            return details;
        });
    } else if (e.kind == EntryKind_Assistant) {
        PrDomElement* turn = put(cursor, keyOf(PartKind_Assistant, entry), 7, [&] { return make("div", "assistant"); });
        showAssistant(turn, entry);
    } else {
        put(cursor, keyOf(PartKind_User, entry), mix(8, text.count()), [&] {
            PrDomElement* line = make("div", "user");
            add(line, "div", "bubble", text);
            return line;
        });
    }
}

void Conversation::show(const ConversationModel& next) {
    if (page == nullptr || column == nullptr || next.transcript == nullptr)
        return;
    const Transcript& t = *next.transcript;
    // The plan as the newest of its calls has left it, and the newest entry that the user typed.
    uint64_t key = mix(mix(mix(mix(t.revision, uint64_t(&t)), next.events), next.unsent->count()), themeGeneration);
    if (!next.unsent->isEmpty())
        key = mix(key, next.unsent->last().count());
    if (key == shownKey)
        return;
    shownKey = key;
    model = next;
    planItems.clear();
    plan = BlockRef{};
    typed = kNone;
    for (uint32_t entry : t.conversation) {
        const Entry& e = t.entries[entry];
        if (!typedText(e).isEmpty())
            typed = entry;
        for (uint32_t i = 0; i < e.blocks.count(); ++i) {
            if (e.blocks[i].kind == BlockKind_ToolUse && e.blocks[i].name == kPlanTool) {
                plan = BlockRef{entry, i};
                applyPlan(json, &planItems, e.blocks[i]);
            }
        }
    }

    bool end = atEnd();
    ++generation;
    Cursor cursor{nodeOf(column), prDomNodeFirstChild(nodeOf(column))};
    for (uint32_t entry : t.conversation)
        showEntry(cursor, entry);
    for (uint32_t i = 0; i < next.unsent->count(); ++i) {
        const String& text = (*next.unsent)[i];
        put(cursor, keyOf(PartKind_Unsent, i), mix(9, text.count()), [&] {
            PrDomElement* line = make("div", "user");
            add(line, "div", "bubble", text);
            add(line, "div", "meta faint", "Not sent: the example runs no agent");
            return line;
        });
    }
    // The parts that this show did not place are gone, and so are their subtrees.
    Array<uint64_t> gone;
    for (uint32_t i = 0; i < items.count(); ++i) {
        if (items.values()[i].generation != generation)
            gone.push(items.keys()[i]);
    }
    for (uint64_t stale : gone)
        forget(stale);
    fitWidgets();
    followEnd(end);
    ++revision;
}

void Conversation::clear() {
    for (const Item& item : items.values()) {
        prDomNodeRemove(nodeOf(item.element));
        prDomNodeRelease(nodeOf(item.element));
    }
    items.clear();
    widgets.clear();
    open.clear();
    shownKey = ~uint64_t(0);
    if (page != nullptr) {
        PrCssScrollToOptions top{.hasLeft = true, .hasTop = true};
        prHtmlWindowScroll(prHtmlPageWindow(page), &top);
    }
    ++revision;
}

// Sizes each visual's iframe to its document's content, which the visual's own layout gives.
void Conversation::fitWidgets() {
    for (PrDomElement* frame : widgets.values()) {
        PrHtmlIFrameElement* iframe = prHtmlIFrameElementQuery(prDomElementAsEventTarget(frame));
        PrHtmlWindow* inner = iframe != nullptr ? prHtmlIFrameElementContentWindow(iframe) : nullptr;
        PrDomDocument* content = inner != nullptr ? prHtmlWindowDocument(inner) : nullptr;
        PrDomElement* root = content != nullptr ? prDomDocumentElement(content) : nullptr;
        if (root == nullptr)
            continue;
        uint32_t height = uint32_t(prF64Ceil(prCssElementGetBoundingClientRect(root).height));
        char style[32];
        PrStringView want = formatToBuffer(Span(style), "height: {}px", height);
        if (prDomElementGetAttribute(frame, "style") != want)
            prDomElementSetAttribute(frame, "style", want);
    }
}

bool Conversation::atEnd() {
    PrHtmlWindow* w = prHtmlPageWindow(page);
    PrDomElement* root = prDomDocumentElement(document());
    if (root == nullptr)
        return true;
    return prHtmlWindowScrollY(w) + prHtmlWindowInnerHeight(w) >= double(prCssElementScrollHeight(root)) - 2;
}

// Keeps the end of the conversation in view if it was in view before the page changed.
void Conversation::followEnd(bool wasAtEnd) {
    if (!wasAtEnd)
        return;
    scrollTo(-1);
}

} // namespace harness
