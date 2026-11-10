// The agent harness example: the shape of an agent harness, on Claude Code's session transcripts.
//
// The window lists the sessions of every store in the user's home, and shows the conversation of the one selected: the
// user's messages, and the assistant's text, thinking and tool calls, each call with its result. The harness's own
// records, the context that it attached for the model and its events, show as one line each while Events is on, and
// open to their records as the file holds them. A session that is being written grows on screen as its file does, and
// the transcript follows its end while it shows its bottom. The composer takes a reply, which Return sends and
// Shift-Return breaks onto another line. A reply goes no further than the transcript, because the example runs no
// agent.
//
// The lists, the header and the composer are the kit's. The conversation is one HTML page (conversation.h), which the
// harness builds from C.

#include "pr/app.h"
#include "pr/ascii.h"
#include "pr/compress.h"
#include "pr/count_of.h"
#include "pr/draw_cpu.h"
#include "pr/draw_renderer.h"
#include "pr/file.h"
#include "pr/float_convert.h"
#include "pr/font_bundle.h"
#include "pr/format.h"
#include "pr/image.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_choice.h"
#include "pr/kit_collection.h"
#include "pr/kit_dock.h"
#include "pr/kit_icon.h"
#include "pr/kit_input.h"
#include "pr/kit_surface.h"
#include "pr/kit_text.h"
#include "pr/kit_theme.h"
#include "pr/min_max.h"
#include "pr/print.h"
#include "pr/set.h"
#include "pr/sort.h"
#include "pr/string_ops.h"
#include "pr/studio_app.h"
#include "pr/thread.h"
#include "pr/time.h"
#include "pr/ui.h"

#include "examples/agent_harness/conversation.h"
#include "examples/agent_harness/transcript.h"

namespace {

using namespace harness;

// How often the transcript shown reads what its file gained, in seconds.
constexpr double kFollowInterval = 0.5;

// The timers that follow the transcript's file, and that wake for the conversation's next task.
constexpr uint64_t kFollowTimer = 1;
constexpr uint64_t kTaskTimer = 2;

// The widest that the conversation stands, in px, as a reader's column.
constexpr float kColumnWidth = 820;

// The example's state.
struct Harness {
    PrApp* app = nullptr;
    PrStudioWindow* window = nullptr;
    Array<Store> stores;
    Array<SessionInfo> sessions;
    uint32_t store = kNone;         // the store whose sessions the list shows, or every store's
    Array<uint32_t> listed;         // the sessions that the list shows, in its order
    PrKitSelection selection{};     // the list's cursor, by row of `listed`
    uint32_t shown = kNone;         // the session shown
    Transcript transcript;
    Conversation conversation;      // the page that shows the transcript
    PrTextFont* font = nullptr;     // what the conversation sets its text in
    bool events = false;            // the harness's records show
    String draft;
    Array<String> unsent;           // replies that the composer sent, which go no further
    float split[2] = {0.24f, 0.76f};
    float sideSplit[2] = {0.65f, 0.35f}; // the session list over the tree of agents
    Array<AgentInfo> agents;             // the subagents of the session shown
    uint32_t agent = kNone;              // the agent whose transcript shows, or kNone for the session's
    Set<uint32_t> folded;                // the rows of the tree that are closed, by agent, kNone for the session
    PrKitSelection agentSelection{};
};

Harness& harnessOf(void* context) {
    return *static_cast<Harness*>(context);
}

// Returns the name that the list gives a store: its configuration directory's name after ".claude-", or "default" for
// ~/.claude.
PrStringView storeLabel(const Store& store) {
    PrStringView name = store.name;
    if (prStringStartsWith(name, ".claude-"))
        return prStringFrom(name, 8);
    return name == ".claude" ? PrStringView("default") : name;
}

// Returns how long ago `time` was, in milliseconds since the Unix epoch, in the largest whole unit.
String agoOf(double time) {
    double seconds = max(0.0, (prTimeWallMilliseconds() - time) / 1000);
    if (seconds < 60)
        return String("now");
    if (seconds < 3600)
        return format("{}m", uint32_t(seconds / 60));
    if (seconds < 86400)
        return format("{}h", uint32_t(seconds / 3600));
    return format("{}d", uint32_t(seconds / 86400));
}

// Shows the session at `index` of the list. Shows the transcript of agent `agent` of the session shown, or the
// session's own for kNone.
void showAgent(Harness& h, uint32_t agent) {
    if (h.shown == kNone)
        return;
    h.agent = agent < h.agents.count() ? agent : kNone;
    h.unsent.clear();
    h.conversation.clear();
    h.transcript.load(h.agent != kNone ? h.agents[h.agent].path : h.sessions[h.shown].path);
}

// Shows the session at `index` of the list, with its tree of agents.
void showSession(Harness& h, uint32_t index) {
    if (index == h.shown || index >= h.sessions.count())
        return;
    h.shown = index;
    h.agents = listAgents(h.sessions[index].path);
    h.folded.clear();
    showAgent(h, kNone);
}

// ---------------------------------------------------------------------------------------------------------------------
// The session list

// Creates the choice of the store whose sessions the list shows, or every store's, and lists those sessions.
void chooseStore(Harness& h, PrKit* kit, PrLayoutNode pane) {
    PrLayoutNode bar = prKitBox(kit, pane, {}, {.padding = prKitEdgesXY(6, 8)});
    PrLayoutNode stores = prKitSegmented(kit, bar, {}, {});
    for (uint32_t i = kNone; i == kNone || i < h.stores.count(); ++i) {
        PrStringView label = i == kNone ? PrStringView("all") : storeLabel(h.stores[i]);
        if (prKitActivated(kit, prKitSegment(kit, stores, {.label = label, .selected = h.store == i})))
            h.store = i;
    }
    h.listed.clear();
    for (uint32_t i = 0; i < h.sessions.count(); ++i) {
        if (h.store == kNone || h.sessions[i].store == h.store)
            h.listed.push(i);
    }
    // The cursor stays on the session shown while the list changes around it.
    uint32_t row = h.listed.find(h.shown);
    if (row != kArrayMissing)
        h.selection = PrKitSelection{.anchor = row, .lead = row};
}

void buildSessions(Harness& h, PrKit* kit, PrLayoutNode pane) {
    char meta[32];
    PrLayoutNode header = prKitPanelHeader(kit, pane, {.title = "SESSIONS"});
    chooseStore(h, kit, pane);
    prKitLabel(kit, header, {.text = formatToBuffer(Span(meta), "{}", h.listed.count())}, {
        .role = PrKitTextRole_Meta,
        .tone = PrKitTone_Faint,
    });
    PrLayoutNode view = prKitListView(kit, pane, {.count = h.listed.count(), .selection = &h.selection}, {
        .rowHeight = 24,
        .place = {.flex = PrKitFlex_Fill},
    });
    PrKitWindow window = prKitListWindow(kit, view);
    for (uint32_t row = window.first; row < window.first + window.count; ++row) {
        uint32_t index = h.listed[row];
        const SessionInfo& session = h.sessions[index];
        PrLayoutNode node = prKitListRow(kit, view, {.index = row, .item = index, .selected = index == h.shown});
        PrStringView title = session.title.isEmpty() ? PrStringView("Untitled") : session.title.view();
        prKitRowLabel(kit, node, {.text = title}, PrKitTone_Auto);
        String ago = agoOf(session.modified);
        if (h.store == kNone)
            ago = format("{}  {}", storeLabel(h.stores[session.store]), ago);
        prKitRowMeta(kit, node, {.text = ago});
    }
    // The rows report their clicks as they are created, so the edit is read after them.
    PrKitSelectionEdit edit{};
    if (prKitSelectionEdited(kit, view, &edit) && h.selection.lead < h.listed.count())
        showSession(h, h.listed[h.selection.lead]);
}

// ---------------------------------------------------------------------------------------------------------------------
// The conversation

// Creates the composer: a text area that Return sends from, and Shift-Return breaks onto another line.
void buildComposer(Harness& h, PrKit* kit, PrLayoutNode parent) {
    PrLayoutNode composer = prKitSurface(kit, parent, {}, {
        .box = {
            .direction = PrKitDirection_Row,
            .align = PrKitAlign_End,
            .gap = 6,
            .padding = prKitEdgesAll(8),
            .place = {.width = prKitPercent(100), .maxWidth = prKitPx(kColumnWidth), .align = PrKitAlign_Center},
        },
        .background = PrKitBackground_Raised,
        .rules = PrKitRuleFlag_All,
    });
    // Return is the composer's, ahead of the text area, which keeps Shift-Return.
    bool send = prUiKeyCaptured(kit->ui, composer, PrAppKey_Enter, 0);
    PrKitButtonStyle ghost{.variant = PrKitButtonVariant_Ghost};
    prKitButton(kit, composer, {.icon = PrKitIconKind_Plus}, ghost);
    PrLayoutNode area = prKitTextArea(kit, composer, {.text = h.draft, .placeholder = "Reply", .rows = 2}, {});
    PrKitPlace grow{.flex = PrKitFlex_Grow};
    prKitPlace(kit, area, &grow);
    PrStringView text{};
    if (prKitTextChanged(kit, area, &text))
        h.draft = text;
    send = prKitActivated(kit, prKitButton(kit, composer, {.icon = PrKitIconKind_ArrowRight}, ghost)) || send;
    if (send && !h.draft.isEmpty()) {
        h.unsent.push(move(h.draft));
        h.draft.clear();
    }
}

void buildConversation(Harness& h, PrKit* kit, PrLayoutNode pane) {
    const Transcript& t = h.transcript;
    PrLayoutNode header = prKitSurface(kit, pane, {}, {
        .box = {.direction = PrKitDirection_Row, .align = PrKitAlign_Center, .gap = 10, .padding = prKitEdgesXY(8, 14)},
        .background = PrKitBackground_Header,
        .rules = PrKitRuleFlag_Bottom,
    });
    PrStringView title = "No session";
    if (h.agent != kNone)
        title = !h.agents[h.agent].description.isEmpty() ? h.agents[h.agent].description : h.agents[h.agent].agentType;
    else if (!t.title.isEmpty())
        title = t.title;
    else if (h.shown != kNone)
        title = h.sessions[h.shown].title;
    PrLayoutNode heading = prKitLabel(kit, header, {.text = title}, {
        .role = PrKitTextRole_Heading,
        .wrap = PrKitWrap_Clip,
    });
    PrKitPlace grow{.flex = PrKitFlex_Grow};
    prKitPlace(kit, heading, &grow);
    if (h.shown != kNone) {
        const SessionInfo& session = h.sessions[h.shown];
        char meta[64];
        prKitLabel(kit, header, {.text = h.stores[session.store].name}, {
            .role = PrKitTextRole_Meta,
            .tone = PrKitTone_Faint,
            .wrap = PrKitWrap_Nowrap,
        });
        prKitLabel(kit, header, {
            .text = formatToBuffer(Span(meta), "{} messages, {:.1f} MB", t.conversation.count(),
                double(session.size) / (1024 * 1024)),
        }, {.role = PrKitTextRole_Meta, .tone = PrKitTone_Faint, .wrap = PrKitWrap_Nowrap});
    }
    prKitToggle(kit, header, {.value = &h.events, .label = "Events"}, {});

    // The conversation's page fills the pane between the header and the composer, and scrolls itself.
    h.conversation.setTheme(kit->theme);
    h.conversation.show(ConversationModel{.transcript = &t, .events = h.events, .unsent = &h.unsent});
    h.conversation.place(kit, pane, prUiKeyIndex(kit->ui, pane, 1));

    PrLayoutNode footer = prKitBox(kit, pane, {}, {.padding = prKitEdgesXY(10, 16)});
    buildComposer(h, kit, footer);
}

// ---------------------------------------------------------------------------------------------------------------------
// The tree of agents

// Creates the tree of the session's agents: the session, and under it the agents that it spawned, each over the agents
// that it spawned in turn. Selecting a row shows its transcript.
void buildAgents(Harness& h, PrKit* kit, PrLayoutNode pane) {
    char meta[32];
    prKitPanelHeader(kit, pane, {.title = "AGENTS", .meta = formatToBuffer(Span(meta), "{}", h.agents.count())});
    // The rows that show: the session's, and each agent's whose ancestors are open, with the agent of each row.
    Array<PrKitTreeItem> rows;
    Array<uint32_t> agentOf;
    rows.push(PrKitTreeItem{.depth = 0, .hasChildren = !h.agents.isEmpty(), .open = !h.folded.contains(kNone)});
    agentOf.push(kNone);
    for (uint32_t i = 0; i < h.agents.count(); ++i) {
        bool shown = !h.folded.contains(kNone);
        for (uint32_t at = h.agents[i].parent; shown && at != kNone; at = h.agents[at].parent)
            shown = !h.folded.contains(at);
        if (!shown)
            continue;
        bool children = i + 1 < h.agents.count() && h.agents[i + 1].parent == i;
        rows.push(PrKitTreeItem{.depth = h.agents[i].depth, .hasChildren = children, .open = !h.folded.contains(i)});
        agentOf.push(i);
    }
    // The cursor stays on the transcript shown.
    uint32_t row = agentOf.find(h.agent);
    if (row != kArrayMissing)
        h.agentSelection = PrKitSelection{.anchor = row, .lead = row};
    PrLayoutNode view = prKitTreeView(kit, pane, {
        .count = rows.count(),
        .selection = &h.agentSelection,
        .items = rows.data(),
    }, {.rowHeight = 24, .place = {.flex = PrKitFlex_Fill}});
    PrKitWindow window = prKitListWindow(kit, view);
    for (uint32_t r = window.first; r < window.first + window.count; ++r) {
        uint32_t agent = agentOf[r];
        PrLayoutNode node = prKitTreeRow(kit, view, {.index = r, .item = uint64_t(agent) + 1, .selected = agent == h.agent});
        if (agent == kNone) {
            prKitRowLabel(kit, node, {.text = "Session"}, PrKitTone_Auto);
            continue;
        }
        const AgentInfo& info = h.agents[agent];
        PrStringView label = !info.description.isEmpty() ? info.description.view() : info.agentType.view();
        prKitRowLabel(kit, node, {.text = label}, PrKitTone_Auto);
        prKitRowMeta(kit, node, {.text = info.agentType});
    }
    // The rows report their clicks and folds as they are created, so both are read after them.
    PrKitTreeFold fold{};
    if (prKitFolded(kit, view, &fold) && fold.row < agentOf.count()) {
        if (fold.open)
            h.folded.erase(agentOf[fold.row]);
        else
            h.folded.add(agentOf[fold.row]);
    }
    PrKitSelectionEdit edit{};
    if (prKitSelectionEdited(kit, view, &edit) && h.agentSelection.lead < agentOf.count() &&
            agentOf[h.agentSelection.lead] != h.agent)
        showAgent(h, agentOf[h.agentSelection.lead]);
}

void build(void* context, PrStudioWindow* window, PrLayoutNode root, PrVec2) {
    Harness& h = harnessOf(context);
    PrKit* kit = prStudioWindowKit(window);
    PrLayoutNode split = prKitSplit(kit, root, {.count = 2, .weights = h.split}, {
        .direction = PrKitDirection_Row,
        .minShare = 0.12f,
        .place = {.flex = PrKitFlex_Fill},
    });
    PrLayoutNode side = prKitSplit(kit, prKitSplitPane(kit, split, 0), {.count = 2, .weights = h.sideSplit}, {
        .direction = PrKitDirection_Column,
        .minShare = 0.15f,
        .place = {.flex = PrKitFlex_Fill},
    });
    buildSessions(h, kit, prKitSplitPane(kit, side, 0));
    buildAgents(h, kit, prKitSplitPane(kit, side, 1));
    buildConversation(h, kit, prKitSplitPane(kit, split, 1));
}

// ---------------------------------------------------------------------------------------------------------------------
// The application

bool open(void* context, PrStudioWindow* window) {
    Harness& h = harnessOf(context);
    h.window = window;
    PrTextFontDesc font{.set = prStudioWindowFonts(window), .style = prFontStyleDefault()};
    h.font = prTextFontCreate(&font);
    h.conversation.setText(h.font, prStudioWindowShaper(window), prStudioAppDevice(prStudioWindowStudioApp(window)));
    h.conversation.setWindow(prStudioWindowAppWindow(window));
    if (!h.sessions.isEmpty()) {
        h.selection = PrKitSelection{};
        showSession(h, 0);
    }
    return true;
}

// Follows the transcript's file, and has the conversation's tasks wake the application, and its replies go to the
// composer's.
bool start(void* context, PrStudioApp* app) {
    Harness& h = harnessOf(context);
    h.app = prStudioAppApp(app);
    prAppTimerSet(h.app, kFollowTimer, kFollowInterval, kFollowInterval);
    h.conversation.setHost([](void* PR_NULLABLE c) { prAppWake(harnessOf(c).app); },
        [](void* PR_NULLABLE c, PrStringView text) { harnessOf(c).unsent.push(String(text)); }, &h);
    return true;
}

// Asks for the next frame at once if a task of the conversation is due, or when the next one is.
bool laidOut(void* context, PrStudioWindow*) {
    Harness& h = harnessOf(context);
    double next = h.conversation.nextTask();
    if (next > 0)
        prAppTimerSet(h.app, kTaskTimer, next / 1000, 0);
    return next == 0;
}

// Gives the conversation the frame's display and preferences, and runs its page's due tasks, before the frame's rounds.
void beginFrame(void* context, PrStudioWindow*, const PrAppFrame* f) {
    Harness& h = harnessOf(context);
    h.conversation.setDisplay(f->display, f->preferences);
    h.conversation.runTasks();
}

// Gives the conversation's page what it takes of the window's input. The kit sees the pointer as well, so that its
// hover follows the pointer, and nothing that the page alone takes: the wheel over the page and the keys while the page
// has them.
bool windowEvent(void* context, PrStudioWindow* window, const PrAppEvent* e) {
    Harness& h = harnessOf(context);
    bool taken = h.conversation.event(prStudioWindowKit(window)->tree, *e);
    if (taken)
        prAppWindowRequestFrame(prStudioWindowAppWindow(window));
    return taken;
}

void close(void* context, PrStudioWindow*) {
    Harness& h = harnessOf(context);
    h.conversation.setWindow(nullptr);
    prTextFontRelease(h.font);
    h.font = nullptr;
    h.window = nullptr;
}

// Reads what the transcript's file gained, and draws a frame if it changed, or for the conversation's tasks.
void event(void* context, PrStudioApp*, const PrAppEvent* e) {
    Harness& h = harnessOf(context);
    if (h.window == nullptr)
        return;
    bool follow = e->kind == PrAppEventKind_Timer && e->tag == kFollowTimer && h.transcript.refresh();
    bool task = e->kind == PrAppEventKind_Wake || (e->kind == PrAppEventKind_Timer && e->tag == kTaskTimer);
    if (follow || task)
        prAppWindowRequestFrame(prStudioWindowAppWindow(h.window));
}

// Prints how long the stores took to list, and how long the largest sessions take to read, with what they hold.
void printStats(Harness& h, double listed) {
    uint64_t bytes = 0;
    for (const SessionInfo& session : h.sessions)
        bytes += session.size;
    print("{} stores, {} sessions, {:.1f} MB, listed in {:.0f} ms\n", h.stores.count(), h.sessions.count(),
        double(bytes) / (1024 * 1024), listed);
    Array<uint32_t> largest;
    for (uint32_t i = 0; i < h.sessions.count(); ++i)
        largest.push(i);
    sortItems(largest.span(), [&](uint32_t a, uint32_t b) { return h.sessions[a].size > h.sessions[b].size; });
    for (uint32_t i = 0; i < min(5u, largest.count()); ++i) {
        const SessionInfo& session = h.sessions[largest[i]];
        double start = prTimeMonotonicMilliseconds();
        h.transcript.load(session.path);
        double read = prTimeMonotonicMilliseconds() - start;
        uint32_t blocks = 0;
        for (const Entry& entry : h.transcript.entries)
            blocks += entry.blocks.count();
        Array<AgentInfo> agents = listAgents(session.path);
        uint32_t deepest = 0;
        for (const AgentInfo& agent : agents)
            deepest = max(deepest, agent.depth);
        print("{:7.1f} MB in {:6.0f} ms: {} entries, {} blocks, {} in the conversation, {} agents {} deep: {}\n",
            double(session.size) / (1024 * 1024), read, h.transcript.entries.count(), blocks,
            h.transcript.conversation.count(), agents.count(), deepest, session.title);
        // The conversation as the window shows it: one page, built and measured in the column.
        start = prTimeMonotonicMilliseconds();
        h.conversation.clear();
        h.conversation.show(ConversationModel{.transcript = &h.transcript, .events = h.events, .unsent = &h.unsent});
        double built = prTimeMonotonicMilliseconds() - start;
        float height = h.conversation.measure(kColumnWidth);
        print("{:24}{} parts, built in {:.0f} ms, {:.0f} px tall, laid out in {:.0f} ms\n", "",
            h.conversation.itemCount(), built, height, prTimeMonotonicMilliseconds() - start - built);
    }
}

// Renders the conversation of a session as the window's page shows it, at its end or at `top` px, to the PNG at `path`,
// with the visuals' tasks run for up to a few seconds. `session` is the session's place in the list, or a part of its
// file's path. Returns false, saying why, if it cannot.
bool renderSession(Harness& h, PrStringView session, double top, PrStringView path) {
    uint32_t index = kNone;
    bool number = session.size != 0;
    for (size_t i = 0; i < session.size; ++i)
        number = number && prAsciiIsDigit(session.data[i]);
    if (number)
        index = 0;
    for (size_t i = 0; number && i < session.size; ++i)
        index = index * 10 + uint32_t(session.data[i] - '0');
    for (uint32_t i = 0; !number && i < h.sessions.count() && index == kNone; ++i) {
        if (prStringFind(h.sessions[i].path, session) != h.sessions[i].path.count())
            index = i;
    }
    if (index >= h.sessions.count()) {
        printError("agent_harness: there is no session {}\n", session);
        return false;
    }
    h.transcript.load(h.sessions[index].path);
    constexpr PrVec2 kExtent{1000, 1400};
    PrDrawList* list = prDrawListCreate();
    h.conversation.paint(kExtent, list);
    prDrawListRelease(list);
    h.conversation.show(ConversationModel{.transcript = &h.transcript, .events = h.events, .unsent = &h.unsent});
    // The visuals fetch what they load from the network, whose answers arrive as tasks, and draw in frames. The
    // window's frames come every 16 ms or so.
    double deadline = prTimeMonotonicMilliseconds() + 5000;
    while (prTimeMonotonicMilliseconds() < deadline) {
        double next = h.conversation.nextTask();
        prThreadSleep(next >= 0 ? min(next, 16.0) : 16.0);
        h.conversation.runTasks();
        list = prDrawListCreate();
        h.conversation.paint(kExtent, list);
        prDrawListRelease(list);
    }
    h.conversation.scrollTo(top);
    list = prDrawListCreate();
    h.conversation.paint(kExtent, list);
    // The window's background is beneath the page, which draws none of its own.
    PrColor bg = prKitPaletteStudio(PrKitThemeVariant_Dark).bg0;
    uint32_t width = uint32_t(kExtent.x);
    uint32_t height = uint32_t(kExtent.y);
    Array<uint8_t> rgba;
    rgba.resize(width * height * 4);
    for (uint32_t i = 0; i < rgba.count(); i += 4) {
        rgba[i] = uint8_t(bg.r * 255);
        rgba[i + 1] = uint8_t(bg.g * 255);
        rgba[i + 2] = uint8_t(bg.b * 255);
        rgba[i + 3] = 255;
    }
    PrDrawRenderer* renderer = prDrawCpuCreate(nullptr);
    PrPixels pixels{rgba.data(), size_t(width) * 4, {width, height}};
    bool ok = renderer != nullptr &&
        prDrawRendererRenderPixels(renderer, list, pixels, PrPixelFormat_Rgba8) == PrStatus_Ok;
    prDrawRendererDestroy(renderer);
    prDrawListRelease(list);
    PrCompressCodec* compress = prCompressCodecCreate(PrCompressCodecBackend_Default);
    PrImageCodec* images = compress != nullptr ? prImageCodecCreate(PrImageCodecBackend_Builtin, compress) : nullptr;
    PrPixelView view{rgba.data(), size_t(width) * 4, {width, height}};
    size_t size = 0;
    Array<uint8_t> png;
    if (ok && images != nullptr && prImageCodecEncode(images, "image/png", view, nullptr, 0, &size) == PrStatus_Ok) {
        png.resize(uint32_t(size));
        ok = prImageCodecEncode(images, "image/png", view, png.data(), size, &size) == PrStatus_Ok &&
            prFileWrite(path, PrByteView{png.data(), size});
    } else {
        ok = false;
    }
    prImageCodecDestroy(images);
    prCompressCodecDestroy(compress);
    if (!ok)
        printError("agent_harness: the conversation could not be rendered to {}\n", path);
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    static Harness h;
    double listing = prTimeMonotonicMilliseconds();
    h.stores = findStores();
    h.sessions = listSessions(h.stores);
    listing = prTimeMonotonicMilliseconds() - listing;
    PrFontSet* fonts = prFontSetCreate(nullptr);
    const PrFontBundle bundles[] = {PrFontBundle_Complete};
    if (prFontBundleInstall(fonts, bundles, uint32_t(countOf(bundles)), PR_FONT_BUNDLE_DIR) == 0) {
        printError("agent_harness: the fonts under {} could not be installed\n", PR_FONT_BUNDLE_DIR);
        prFontSetDestroy(fonts);
        return 1;
    }
    // --render <session> <path> [top] [events] renders the conversation of a session, by its place in the list or a
    // part of its path, to a PNG.
    if (argc > 3 && PrStringView(argv[1]) == "--render") {
        PrTextShaper* shaper = prTextShaperCreate(PrTextShaperBackend_Default);
        PrTextFontDesc font{.set = fonts, .style = prFontStyleDefault()};
        h.font = prTextFontCreate(&font);
        PrKitTheme theme = prKitThemeStudio(PrKitThemeVariant_Dark, fonts);
        h.conversation.setText(h.font, shaper, nullptr);
        h.conversation.setTheme(theme);
        h.events = argc > 5 && PrStringView(argv[5]) == "events";
        double top = -1;
        if (argc > 4)
            prF64ParsePrefix(argv[4], &top);
        bool ok = renderSession(h, argv[2], top, argv[3]);
        h.conversation.clear();
        prTextFontRelease(h.font);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
        return ok ? 0 : 1;
    }
    if (argc > 1 && PrStringView(argv[1]) == "--stats") {
        PrTextShaper* shaper = prTextShaperCreate(PrTextShaperBackend_Default);
        PrTextFontDesc font{.set = fonts, .style = prFontStyleDefault()};
        h.font = prTextFontCreate(&font);
        PrKitTheme theme = prKitThemeStudio(PrKitThemeVariant_Dark, fonts);
        h.conversation.setText(h.font, shaper, nullptr);
        h.conversation.setTheme(theme);
        printStats(h, listing);
        h.conversation.clear();
        prTextFontRelease(h.font);
        prTextShaperDestroy(shaper);
        prFontSetDestroy(fonts);
        return 0;
    }
    PrStudioWindowDesc desc{
        .title = "Harness",
        .extent = {1440, 900},
        .open = open,
        .beginFrame = beginFrame,
        .event = windowEvent,
        .build = build,
        .laidOut = laidOut,
        .close = close,
        .context = &h,
    };
    PrStudioAppDesc app{.fonts = fonts, .window = &desc, .start = start, .event = event, .context = &h};
    PrStatus status = prStudioAppRun(&app);
    prFontSetDestroy(fonts);
    if (status == PrStatus_NotSupported)
        printError("agent_harness: no GPU device to render with\n");
    return status == PrStatus_Ok ? 0 : 1;
}
