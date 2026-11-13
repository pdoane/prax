// Tabs: each tab's page on the page's thread, and what the window's thread asks of it.
//
// The window's thread never calls a page. It posts tasks to the agent (prDomAgentPost), which run on the page's thread
// in the order they were posted: a tab's opening first, its closing last. Input, frames and traversals go to the tab's
// view, which posts them itself.

#include "apps/browser/tab.h"

#include "pr/array.h"
#include "pr/css_presentation.h"
#include "pr/dom.h"
#include "pr/html.h"
#include "pr/min_max.h"
#include "pr/print.h"
#include "pr/string.h"
#include "pr/time.h"

#include "apps/browser/address.h"

namespace browser {

namespace {

// How long the page's thread runs the pages' due tasks at a time, in milliseconds, so that a long load renders as it
// goes.
constexpr double kTaskBudget = 8;

// The approximate duration of each job of the layout thread, in milliseconds.
constexpr double kJobBudget = 10;

// The most closed tabs that can reopen.
constexpr size_t kMaxClosed = 20;

// ---------------------------------------------------------------------------------------------------------------------
// The page's thread

PrHtmlHost pageHost(Tab& tab);

// Gives the page of an iframe the host that its tab's page has.
void frameHost(void* context, PrHtmlPage*, PrHtmlHost* child) {
    *child = pageHost(*static_cast<Tab*>(context));
}

// Reports a link visited if the history holds the URL that it points to. A restyle calls it, on the page's thread or
// one of its workers.
PrDomHostState linkState(void* context, PrHtmlPage*, const PrDomElement* element, PrDomHostState wanted) {
    PrDomEventTarget* target = prDomNodeAsEventTarget(prDomElementAsNode(const_cast<PrDomElement*>(element)));
    bool hyperlink = prHtmlAnchorElementQuery(target) != nullptr || prHtmlAreaElementQuery(target) != nullptr;
    if ((wanted & PrDomHostState_Visited) == 0 || !hyperlink)
        return 0;
    const Browser& b = *static_cast<Tab*>(context)->browser;
    PrRawString url = prHtmlHyperlinkComponent(element, PrUrlComponent_Href);
    PrStringView view = prRawStringView(&url);
    bool visited = b.visitedUrls.contains(view);
    prRawStringReset(&url);
    return visited ? PrDomHostState_Visited : PrDomHostState(0);
}

// Asks the window's thread to show a hyperlink's URL in a new tab.
void openWindow(void* context, PrHtmlPage*, PrStringView url) {
    Tab& tab = *static_cast<Tab*>(context);
    prMutexLock(&tab.mutex);
    tab.opened.emplace(url);
    prMutexUnlock(&tab.mutex);
    browserWantFrame(*tab.browser);
}

// Returns the host of a tab's page, before the script engine takes it. The tab's iframes share its session history.
PrHtmlHost pageHost(Tab& tab) {
    Browser& b = *tab.browser;
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &tab;
    host.fetcher = b.fetcher;
    host.frame = frameHost;
    host.openWindow = openWindow;
    host.elementState = linkState;
    host.agent = b.agent;
    host.font = b.font;
    host.shaper = b.shaper;
    host.images = b.images;
    host.media = b.media;
    host.mixer = b.mixer;
    host.device = b.device;
    host.compress = b.compress;
    host.localStorage = b.localStorage;
    host.sessionStorage = b.sessionStorage;
    host.sessionHistory = tab.history;
    return host;
}

PrHtmlVisibility visibilityOf(bool shown) {
    return shown ? PrHtmlVisibility_Visible : PrHtmlVisibility_Hidden;
}

void navigate(Tab& tab, const String& url) {
    tab.pending = url;
    tab.loadStarted = prTimePerfNow();
    if (prHtmlPageNavigate(tab.page, url) != PrStatus_Ok)
        printError("browser: cannot navigate to {}\n", url);
}

// Prints, under --trace, what a tab's view did with its jobs, and how long a viewport's changes took to settle.
void traceUpdate(void* PR_NULLABLE context, const PrHtmlViewUpdate* update) {
    Tab& tab = *static_cast<Tab*>(context);
    double now = prTimeMonotonicMilliseconds();
    if (update->extent.x != tab.traceExtent.x || update->extent.y != tab.traceExtent.y) {
        printError("[{:.3f}] page sees viewport {:.0f}x{:.0f}\n", now / 1000, update->extent.x, update->extent.y);
        tab.traceExtent = update->extent;
        if (tab.traceResized == 0)
            tab.traceResized = now;
    }
    if (update->committed) {
        bool stale = update->committedExtent.x != update->extent.x || update->committedExtent.y != update->extent.y;
        printError("[{:.3f}] job done at {:.0f}x{:.0f}{} in {:.1f} ms: {} blocks laid out, {} reused, {} lines broken; "
            "frames {} blocks\n", now / 1000, update->committedExtent.x, update->committedExtent.y,
            stale ? " (stale)" : "", update->committedMillis, update->layout.blocksLaidOut, update->layout.blocksReused,
            update->layout.linesBroken, update->framesBlocks);
        if (tab.traceResized != 0 && stale)
            ++tab.traceStale;
    }
    if (update->started) {
        PrStringView view = update->startedForView ? " view" : "";
        PrStringView page = update->startedForPage ? " page" : "";
        PrStringView frames = update->startedForFrames != 0 ? " frames" : "";
        printError("[{:.3f}] job start at {:.0f}x{:.0f} for{}{}{} ({}), prepared in {:.1f} ms: {} elements styled, {} "
            "animated; frames {} styled, {} animated\n", now / 1000, update->extent.x, update->extent.y, view, page,
            frames, update->startedForFrames, update->startMillis, update->style.elementsStyled,
            update->style.elementsAnimated, update->framesStyled, update->framesAnimated);
        if (tab.traceResized != 0)
            ++tab.traceJobs;
    }
    if (update->current && tab.traceResized != 0) {
        printError("[{:.3f}] viewport settled {:.1f} ms after it changed: {} jobs, {} stale\n", now / 1000,
            now - tab.traceResized, tab.traceJobs, tab.traceStale);
        tab.traceResized = 0;
        tab.traceJobs = 0;
        tab.traceStale = 0;
    }
}

// Opens a tab's page at the extent of `window`, with its view, and navigates it to `url` unless it is empty.
void openPage(Browser& b, Tab& tab, const PrAppFrame& window, bool shown, const String& url) {
    tab.history = prHtmlSessionHistoryCreate();
    PrHtmlHost host = pageHost(tab);
    PrHtmlHost scripted = prJsHtmlEngineHost(b.engine, &host, PrJsHtmlScripting_Enabled);
    tab.page = prHtmlPageCreate(&scripted);
    prHtmlPageSetViewport(tab.page, window.extent);
    prHtmlPageSetDisplay(tab.page, &window.display);
    prHtmlPageSetUserPreferences(tab.page, &window.preferences);
    prHtmlPageSetVisibility(tab.page, visibilityOf(shown));
    PrHtmlViewDesc view{
        .page = tab.page,
        .compositor = tab.compositor,
        .jobBudget = kJobBudget,
        .updated = b.trace ? traceUpdate : nullptr,
        .updatedContext = &tab,
    };
    PrHtmlView* made = prHtmlViewCreate(&view);
    prHtmlViewSetApp(made, b.app);
    tab.view.store(made);
    b.open.push(&tab);
    if (!url.isEmpty())
        navigate(tab, url);
}

// Frees a tab's view, page and session history. The engine's realm for the page goes with the page.
void closePage(Tab& tab) {
    prHtmlViewDestroy(tab.view.exchange(nullptr));
    prHtmlPageDestroy(tab.page);
    prHtmlSessionHistoryDestroy(tab.history);
    tab.page = nullptr;
    tab.history = nullptr;
}

// Frees a tab whose page is closed. Its compositor goes after its view, which commits to it.
void freeTab(Tab* tab) {
    prLayoutCompositorDestroy(tab->compositor);
    prMutexReset(&tab->mutex);
    delete tab;
}

// Restyles the links of every open page, after the URLs that the history holds changed.
void linksChanged(Browser& b) {
    for (Tab* tab : b.open)
        prHtmlPageInvalidateHostStates(tab->page, PrDomHostState_Visited);
}

// What the window's thread asks of the page's thread.
enum {
    TaskKind_Open,
    TaskKind_Navigate,
    TaskKind_Show,
    TaskKind_Close,
    TaskKind_Visit,
    TaskKind_Forget,
    TaskKind_Shutdown,
};
typedef uint8_t TaskKind;

// A task for the page's thread, which frees it once it has run.
struct TabTask {
    Browser* browser;
    Tab* PR_NULLABLE tab;
    TaskKind kind;
    String url;                     // Open, Navigate: the URL to load, or empty for none. Visit: the URL that the
                                    // history recorded. Forget: the URL that it removed, or empty if it removed all
    PrAppFrame window{};            // Open: the window as the page first sees it
    bool shown = false;             // Open, Show: true if the tab is the active one
};

void runTask(void* PR_NULLABLE context) {
    TabTask* task = static_cast<TabTask*>(context);
    Browser& b = *task->browser;
    switch (task->kind) {
        case TaskKind_Open:
            openPage(b, *task->tab, task->window, task->shown, task->url);
            break;
        case TaskKind_Navigate:
            navigate(*task->tab, task->url);
            break;
        case TaskKind_Show:
            prHtmlPageSetVisibility(task->tab->page, visibilityOf(task->shown));
            break;
        case TaskKind_Close:
            b.open.removeFirst(task->tab);
            closePage(*task->tab);
            freeTab(task->tab);
            break;
        case TaskKind_Visit:
            if (b.visitedUrls.add(task->url))
                linksChanged(b);
            break;
        case TaskKind_Forget:
            if (task->url.isEmpty())
                b.visitedUrls.clear();
            else
                b.visitedUrls.erase(task->url);
            linksChanged(b);
            break;
        case TaskKind_Shutdown:
            // The engine goes before the pages, since its objects' finalizers reach the pages' windows.
            for (Tab* tab : b.open)
                prHtmlViewDestroy(tab->view.exchange(nullptr));
            prJsHtmlEngineDestroy(b.engine);
            b.engine = nullptr;
            for (Tab* tab : b.open) {
                closePage(*tab);
                freeTab(tab);
            }
            b.open.clear();
            prDomAgentStop(b.agent);
            break;
    }
    delete task;
}

void post(Browser& b, TabTask* task) {
    prDomAgentPost(b.agent, runTask, task);
}

// Returns a copy of `string`, and releases it.
String taken(PrRawString string) {
    String copy = prRawStringView(&string);
    prRawStringReset(&string);
    return copy;
}

// Returns what the chrome shows of a tab's page now. While the navigation that the window asked for waits for its
// response, the page's URL is the navigation's.
TabInfo infoOf(Tab& tab) {
    TabInfo info;
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(tab.page));
    info.url = taken(prDomDocumentUrl(document));
    info.title = taken(prDomDocumentTitle(document));
    bool navigating = prHtmlPageIsNavigating(tab.page);
    if (!navigating)
        tab.pending.clear();
    if (!tab.pending.isEmpty())
        info.url = tab.pending;
    info.loading = navigating || prHtmlPageIsFetching(tab.page);
    uint32_t length = prHtmlSessionHistoryLength(tab.history);
    uint32_t index = prHtmlSessionHistoryIndex(tab.history);
    info.canGoBack = length != 0 && index > 0;
    info.canGoForward = index + 1 < length;
    if (const PrHtmlView* view = tab.view.load())
        info.output = prHtmlViewOutput(view);
    return info;
}

// Returns true if two publications of a tab show the same document in the same state.
bool sameDocument(const TabInfo& a, const TabInfo& b) {
    return a.url == b.url && a.title == b.title && a.iconUrl == b.iconUrl && a.loading == b.loading &&
        a.canGoBack == b.canGoBack && a.canGoForward == b.canGoForward;
}

// Prints how long a navigation took to settle, once its document is complete and the last scene shows all of it. Until
// then, background work takes turns with input on the page's thread and the layout thread.
void reportLoad(Tab& tab, const TabInfo& info) {
    if (tab.loadStarted.ticks == 0 || info.loading)
        return;
    PrDomDocument* document = prHtmlWindowDocument(prHtmlPageWindow(tab.page));
    const PrCssPresentation* presentation = prHtmlPagePresentation(tab.page);
    if (prHtmlDocumentReadyState(document) != PrHtmlDocumentReadyState_Complete || presentation == nullptr ||
            prCssPresentationIsBusy(presentation) || !prCssPresentationIsCurrent(presentation))
        return;
    printError("browser: {} settled in {:.2f} s\n", info.url, prTimePerfSeconds(tab.loadStarted, prTimePerfNow()));
    tab.loadStarted = PrTimePerf{};
}

// Publishes what changed of each open tab since the last run, and asks the window for a frame to show it.
void publish(Browser& b) {
    bool changed = false;
    for (Tab* tab : b.open) {
        // A document names its icon in its head, so the icon is looked for once the document has loaded. The chrome
        // shows the last document's icon until then. Only this thread writes what is published, so it reads it without
        // the mutex.
        TabInfo info = infoOf(*tab);
        reportLoad(*tab, info);
        info.iconUrl = tab->published.iconUrl;
        if (!info.loading && !sameDocument(info, tab->published))
            info.iconUrl = taken(prHtmlPageIconUrl(tab->page));
        const PrAppOutput& now = info.output;
        const PrAppOutput& was = tab->published.output;
        bool sameInput = now.hasTextInput == was.hasTextInput && now.textInput.left == was.textInput.left &&
            now.textInput.top == was.textInput.top;
        if (sameDocument(info, tab->published) && now.cursor == was.cursor && sameInput &&
                now.frameTime == was.frameTime)
            continue;
        prMutexLock(&tab->mutex);
        tab->published = info;
        tab->changed = true;
        prMutexUnlock(&tab->mutex);
        changed = true;
    }
    if (changed)
        browserWantFrame(b);
}

// ---------------------------------------------------------------------------------------------------------------------
// The window's thread

// The clock of the compositors, which is the pages' clock: the system's monotonic clock.
double clock(void* PR_NULLABLE) {
    return prTimeMonotonicMilliseconds();
}

void present(void* PR_NULLABLE context, PrDrawList* list, PrColor16 canvas) {
    Tab& tab = *static_cast<Tab*>(context);
    browserPresent(*tab.browser, list, canvas);
}

// Asks the window for a frame if the tab is the active one. A hidden tab's commits show in no frame.
void wantFrame(void* PR_NULLABLE context) {
    Tab& tab = *static_cast<Tab*>(context);
    if (tab.shown.load())
        browserWantFrame(*tab.browser);
}

// Writes the history to its file, if the platform names a data directory to keep it in.
void saveHistory(const Browser& b) {
    if (!b.historyPath.isEmpty() && !historySave(b.historyPath, b.history))
        printError("browser: cannot save the history to {}\n", b.historyPath);
}

// Records the tab's document in the history once it has loaded. A URL that the tab did not show last is a visit, which
// the page's thread learns of, so that links to it are visited. The title that the document names later goes to the
// visit.
void record(Browser& b, Tab& tab) {
    const TabInfo& info = tab.info;
    if (info.loading || addressIsBlank(info.url) || addressIsBrowserPage(info.url))
        return;
    if (info.url != tab.recorded) {
        tab.recorded = info.url;
        historyRecord(&b.history, info.url, info.title, prTimeSystemNow());
        post(b, new TabTask{.browser = &b, .tab = nullptr, .kind = TaskKind_Visit, .url = info.url});
    } else if (!historySetTitle(&b.history, info.url, info.title)) {
        return;
    }
    saveHistory(b);
}

} // namespace

Tab* tabOpen(Browser& b, PrStringView url, uint32_t index) {
    Tab* tab = new Tab;
    tab->browser = &b;
    tab->key = b.nextKey++;
    tab->view.store(nullptr);
    tab->shown.store(false);
    prMutexInit(&tab->mutex);
    // The compositor has no thread of its own: the window's frames make its frames while the tab is active.
    PrLayoutCompositorDesc compositor{
        .context = tab,
        .now = clock,
        .present = present,
        .wantFrame = wantFrame,
        .canvasFilled = true,
    };
    tab->compositor = prLayoutCompositorCreate(&compositor);
    tab->info.url = url;
    tab->info.loading = !url.isEmpty();
    b.tabs.insert(min(index, b.tabs.count()), tab);
    post(b,
        new TabTask{.browser = &b, .tab = tab, .kind = TaskKind_Open, .url = tab->info.url, .window = b.pageWindow});
    return tab;
}

void tabOpenNext(Browser& b, PrStringView url) {
    uint32_t at = b.tabs.isEmpty() ? 0 : b.active + 1;
    tabOpen(b, url, at);
    tabActivate(b, at);
}

void tabsLoad(Browser& b, PrStringView url, TabTarget target) {
    Tab* tab = browserActiveTab(b);
    if (target == TabTarget_New || tab == nullptr)
        tabOpenNext(b, url);
    else
        tabNavigate(b, *tab, url);
}

void tabsShowHistory(Browser& b) {
    for (uint32_t i = 0; i < b.tabs.count(); ++i) {
        if (b.tabs[i]->info.url == kAddressHistory) {
            tabActivate(b, i);
            return;
        }
    }
    const Tab* tab = browserActiveTab(b);
    bool start = tab != nullptr && addressIsBlank(tab->info.url) && !tab->info.loading;
    tabsLoad(b, kAddressHistory, start ? TabTarget_Active : TabTarget_New);
}

void tabClose(Browser& b, uint32_t index) {
    Tab* tab = b.tabs[index];
    if (!addressIsBlank(tab->info.url)) {
        b.closed.push(tab->info.url);
        if (b.closed.count() > kMaxClosed)
            b.closed.erase(0);
    }
    b.tabs.erase(index);
    post(b, new TabTask{.browser = &b, .tab = tab, .kind = TaskKind_Close});
    // The window keeps a tab: closing the last one opens the start page.
    if (b.tabs.isEmpty())
        tabOpen(b, PrStringView{}, 0);
    uint32_t active = b.active;
    if (index < active || active >= b.tabs.count())
        active = active > 0 ? active - 1 : 0;
    tabActivate(b, active);
}

void tabActivate(Browser& b, uint32_t index) {
    b.active = index;
    for (uint32_t i = 0; i < b.tabs.count(); ++i) {
        Tab* tab = b.tabs[i];
        bool shown = i == index;
        if (tab->shown.exchange(shown) != shown) {
            post(b, new TabTask{.browser = &b, .tab = tab, .kind = TaskKind_Show, .shown = shown});
        }
    }
    b.chrome.addressTab = 0;
    browserWantFrame(b);
}

void tabNavigate(Browser& b, Tab& tab, PrStringView url) {
    tab.info.url = url;
    tab.info.loading = true;
    post(b, new TabTask{.browser = &b, .tab = &tab, .kind = TaskKind_Navigate, .url = tab.info.url});
}

void tabTraverse(Tab& tab, int32_t delta) {
    if (PrHtmlView* view = tab.view.load())
        prHtmlViewPostTraverse(view, delta);
}

bool tabsRefresh(Browser& b) {
    bool changed = false;
    Array<String> opened;
    for (Tab* tab : b.tabs) {
        prMutexLock(&tab->mutex);
        bool taken = tab->changed;
        if (taken)
            tab->info = tab->published;
        tab->changed = false;
        opened.append(tab->opened.span());
        tab->opened.clear();
        prMutexUnlock(&tab->mutex);
        if (taken)
            record(b, *tab);
        changed = changed || taken;
    }
    for (const String& url : opened)
        tabOpenNext(b, url);
    return changed || !opened.isEmpty();
}

void tabsForget(Browser& b, PrStringView url) {
    if (url.isEmpty())
        b.history.clear();
    else if (!historyRemove(&b.history, url))
        return;
    saveHistory(b);
    post(b, new TabTask{.browser = &b, .tab = nullptr, .kind = TaskKind_Forget, .url = url});
}

void tabsRun(Browser& b) {
    prJsHtmlEngineAttachThread(b.engine);
    while (prDomAgentWait(b.agent, -1)) {
        prDomAgentRun(b.agent, kTaskBudget);
        publish(b);
    }
}

void tabsShutdown(Browser& b) {
    if (b.started) {
        post(b, new TabTask{.browser = &b, .kind = TaskKind_Shutdown});
        prThreadJoin(&b.thread);
        b.started = false;
    } else {
        prJsHtmlEngineDestroy(b.engine);
        b.engine = nullptr;
        for (Tab* tab : b.tabs)
            freeTab(tab);
    }
    b.tabs.clear();
}

} // namespace browser
