// The conversation that the window shows: one HTML page, which the harness builds and updates from C through the DOM.
// The page runs no script. Each visual that the model made is a sandboxed iframe with an opaque origin, whose scripts
// an engine runs, and which reaches the page only through postMessage. Each response's Markdown is in its own shadow
// root, so that its HTML cannot style the page.
//
// The page fills a replaced node of the window's tree as its viewport, and scrolls itself. It is styled with the kit's
// theme (prKitThemeCss).

#pragma once

#include "pr/app.h"
#include "pr/array.h"
#include "pr/css.h"
#include "pr/dom.h"
#include "pr/fetch.h"
#include "pr/gpu.h"
#include "pr/html_page.h"
#include "pr/html_quickjs.h"
#include "pr/html_view.h"
#include "pr/http.h"
#include "pr/json.h"
#include "pr/kit.h"
#include "pr/kit_theme.h"
#include "pr/layout_tree.h"
#include "pr/map.h"
#include "pr/net.h"
#include "pr/set.h"
#include "pr/storage.h"
#include "pr/string.h"
#include "pr/string_view.h"
#include "pr/text.h"
#include "pr/text_font.h"
#include "pr/tls.h"

#include "examples/agent_harness/transcript.h"

#include <stdint.h>

PR_ASSUME_NONNULL_BEGIN

namespace harness {

using namespace pr;

// What the conversation shows. The harness owns each member, and keeps it alive while the conversation shows it.
struct ConversationModel {
    const Transcript* transcript = nullptr;
    bool events = false;                        // the harness's records show
    const Array<String>* unsent = nullptr;      // replies that the user sent, which go no further
};

// Called on any thread when the page has a task to run, which the host runs at its next frame (runTasks).
typedef void (*ConversationWakeFn)(void* PR_NULLABLE context);

// Called with a reply from the page: an answer that a turn's wrap-up offers, or a visual's sendPrompt.
typedef void (*ConversationReplyFn)(void* PR_NULLABLE context, PrStringView text);

// The page of the conversation, its visuals' script engine, and the network that the visuals fetch from.
struct Conversation {
    Conversation();
    ~Conversation();
    Conversation(const Conversation&) = delete;
    Conversation& operator=(const Conversation&) = delete;

    // Sets the font and shaper of the page's text, and the device that the visuals' canvases draw on, each of which
    // outlives the conversation. Makes the page again.
    void setText(PrTextFont* font, PrTextShaper* shaper, PrGpuDevice* PR_NULLABLE device);

    // Sets the window whose clipboard the page copies to, or none.
    void setWindow(PrAppWindow* PR_NULLABLE window);

    // Sets what wakes the host for the page's tasks, and what receives the conversation's replies.
    void setHost(ConversationWakeFn wake, ConversationReplyFn reply, void* PR_NULLABLE context);

    // Sets the theme that styles the page. The page restyles in place, and the visuals reload.
    void setTheme(const PrKitTheme& theme);

    // Sets the display and the preferences that the page shows for.
    void setDisplay(const PrAppDisplay& display, const PrAppUserPreferences& preferences);

    // Updates the page to `model`, rebuilding only the parts that changed. A page scrolled to its end stays there as
    // the conversation grows.
    void show(const ConversationModel& model);

    // Empties the page and scrolls it to the top, for another transcript.
    void clear();

    // Creates the replaced node `key` under `parent` that the page shows in, filling `parent`.
    PrLayoutNode place(PrKit* kit, PrLayoutNode parent, uint64_t key);

    // Routes a window event to the page: the pointer while over the page or while a button pressed there is down, the
    // wheel over the page, and the keys after a press in the page until a press elsewhere. Positions are placed by
    // where `tree` last laid the page out. Returns true for a wheel or a key that the page took, which the kit should
    // not see.
    bool event(PrLayoutTree* tree, const PrAppEvent& event);

    // Runs the page's tasks that are due, and the microtasks that they queue.
    void runTasks();

    // Returns the milliseconds until a task of the page is due, zero if one is, or -1 if there is none.
    double nextTask();

    // Lays the page out at `width` px wide and returns how tall its content is, for a host that measures it.
    float measure(float width);

    // Records the page into `list` as a viewport of `extent` px shows it, for a host that renders it offscreen.
    void paint(PrVec2 extent, PrDrawList* list);

    // Scrolls the page to `top` px, or to its end if `top` is negative.
    void scrollTo(double top);

    // The number of DOM subtrees that the page keeps for the parts of the conversation.
    uint32_t itemCount() const { return items.count(); }

private:
    // A part of the conversation: the subtree that shows it, which the conversation holds a reference to, a hash of
    // what it was built from, and the last show that kept it.
    struct Item {
        PrDomElement* element;
        uint64_t signature;
        uint64_t generation;
    };

    // Where the next child of a parent goes, as show() places the parent's children in order.
    struct Cursor {
        PrDomNode* parent;
        PrDomNode* PR_NULLABLE next;
    };

    void makePage();
    void destroyPage();
    PrDomDocument* document();
    PrDomElement* make(PrStringView tag, PrStringView className = {}, PrStringView text = {});
    PrDomElement* add(PrDomNode* parent, PrStringView tag, PrStringView className = {}, PrStringView text = {});
    PrDomElement* add(PrDomElement* parent, PrStringView tag, PrStringView className = {}, PrStringView text = {});
    template <typename Build>
    PrDomElement* put(Cursor& cursor, uint64_t key, uint64_t signature, Build build);
    void forget(uint64_t key);
    PrDomElement* disclosure(uint64_t key, PrStringView className, PrDomElement* PR_NULLABLE* PR_NONNULL summary);
    void fill(uint64_t key, PrDomElement* details);
    void preformatted(PrDomElement* parent, PrStringView text, PrStringView className);
    void markdown(PrDomElement* host, PrStringView text);
    PrDomElement* widget(PrDomElement* parent, PrStringView code);
    static void frameHost(void* PR_NULLABLE context, PrHtmlPage* parent, PrHtmlHost* child);
    void showEntry(Cursor& cursor, uint32_t entry);
    void showAssistant(PrDomElement* message, uint32_t entry);
    PrDomElement* buildCall(uint32_t entry, uint32_t index);
    void opened(PrDomEvent* event);
    void clicked(PrDomEvent* event);
    void messaged(PrDomEvent* event);
    void fitWidgets();
    void followEnd(bool wasAtEnd);
    bool atEnd();

    PrDomAgent* agent;
    PrJsHtmlEngine* engine;
    PrNet* PR_NULLABLE net;         // the network that the visuals fetch from, and the services over it
    PrTls* PR_NULLABLE tls;
    PrHttp* PR_NULLABLE http;
    PrStorage* cache;
    PrFetchNetwork* PR_NULLABLE network;
    PrTextFont* PR_NULLABLE font = nullptr;
    PrTextShaper* PR_NULLABLE shaper = nullptr;
    PrGpuDevice* PR_NULLABLE device = nullptr;
    PrAppWindow* PR_NULLABLE window = nullptr;
    PrAppDisplay display;
    PrAppUserPreferences preferences;
    ConversationWakeFn PR_NULLABLE wake = nullptr;
    ConversationReplyFn PR_NULLABLE reply = nullptr;
    void* PR_NULLABLE hostContext = nullptr;

    PrHtmlPage* PR_NULLABLE page = nullptr;
    PrHtmlView* PR_NULLABLE view = nullptr;
    PrDomElement* PR_NULLABLE column = nullptr;           // the element that holds the conversation's parts
    PrDomElement* PR_NULLABLE themeStyle = nullptr;       // the style element of the theme
    PrCssStyleSheet* PR_NULLABLE markdownSheet = nullptr; // the sheet that every response's shadow root adopts
    String themeCss;                                      // the theme as CSS, which the visuals load with too
    bool dark = true;                                     // the theme's palette is dark
    bool themed = false;                                  // a theme was set, whose colors and type follow
    PrKitPalette palette{};
    PrKitType types[PrKitTextRole_Count]{};
    uint64_t themeGeneration = 0;                   // changes with the theme, which the visuals are built in
    uint64_t revision = 0;                          // changes each time that the page may draw something else

    // An item of the plan of a turn: what it is, and whether it is pending, in progress or completed.
    struct PlanItem {
        String content;
        String status;
    };

    ConversationModel model;
    Array<PlanItem> planItems;            // the plan as its calls up to the newest have left it
    BlockRef plan;                        // the newest call that sets the plan, which shows the plan
    uint32_t typed = kNone;               // the newest entry of the conversation that the user typed
    uint64_t shownKey = ~uint64_t(0);     // what the last show was given, which show() skips the work of if it is the
                                          // same
    Map<uint64_t, Item> items;            // the parts of the conversation, by key
    uint64_t generation = 0;              // the number of shows that changed the page
    Set<uint64_t> open;                   // the disclosures that are open, by key
    Map<uint64_t, PrDomElement*> widgets; // the iframes of the visuals, which size to their documents, by part
    PrJson* json;                         // a call's input, and a visual's message

    PrLayoutNode node = kPrLayoutNodeNone; // the node of the window's tree that last showed the page
    bool pressed = false;                  // a press in the page holds the pointer
    uint32_t buttons = 0;                  // the buttons that are down in the page, by bit
    bool hovered = false;                  // the pointer is over the page
    bool focused = false;                  // the page has the keys
};

} // namespace harness

PR_ASSUME_NONNULL_END
