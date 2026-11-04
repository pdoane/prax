// The browser: its tabs, the chrome around them, and what every tab's page shares.
//
// The browser runs on three threads:
//
//   - The window's thread takes the window's events and frames. It builds the chrome with Studio Kit, posts input and
//     frames to the active tab's view, and presents the active tab's compositor with the chrome over it.
//   - The page's thread runs the agent's event loop (prDomAgentRun), which runs every tab's page. It opens and closes
//     the tabs' pages, and after each run it publishes what the chrome shows of each tab.
//   - The agent's layout thread lays out and records the pages in jobs of about a frame's work. As each job finishes,
//     the tab's view commits its scene to the tab's compositor, which asks the window for a frame.

#pragma once

#include "pr/app.h"
#include "pr/array.h"
#include "pr/atomic.h"
#include "pr/audio_mixer.h"
#include "pr/base.h"
#include "pr/cache.h"
#include "pr/command.h"
#include "pr/command_keymap.h"
#include "pr/compress.h"
#include "pr/draw.h"
#include "pr/draw_display.h"
#include "pr/draw_image.h"
#include "pr/draw_renderer.h"
#include "pr/fetch.h"
#include "pr/font_set.h"
#include "pr/gpu.h"
#include "pr/html_page.h"
#include "pr/html_quickjs.h"
#include "pr/html_storage.h"
#include "pr/html_view.h"
#include "pr/http.h"
#include "pr/image.h"
#include "pr/kit.h"
#include "pr/layout_compositor.h"
#include "pr/layout_scene.h"
#include "pr/media.h"
#include "pr/mutex.h"
#include "pr/net.h"
#include "pr/set.h"
#include "pr/storage.h"
#include "pr/string.h"
#include "pr/task.h"
#include "pr/text.h"
#include "pr/thread.h"
#include "pr/tls.h"
#include "pr/ui.h"

#include "examples/browser/favorites.h"
#include "examples/browser/history.h"

#include <stdint.h>

PR_ASSUME_NONNULL_BEGIN

namespace browser {

using namespace pr;

// ---------------------------------------------------------------------------------------------------------------------
// Tabs

// What the chrome shows of a tab's page.
struct TabInfo {
    String url;                // the URL of the page's document
    String title;              // the document's title, or empty if it has none
    String iconUrl;            // the URL of the icon of the last document that loaded, or empty if it has none
    bool loading = false;      // true while the page fetches its document or the resources that it needs
    bool canGoBack = false;    // true if the session history has entries before the current one
    bool canGoForward = false; // true if the session history has entries after the current one

    // What the document reports to the window (prHtmlViewOutput), in the page's px.
    PrAppOutput output{};
};

struct Browser;

// A command of the browser's table (commands.cpp).
struct CommandSpec;

// The context that the registry passes to one of the browser's commands: the browser and the command.
struct CommandContext {
    Browser* browser;
    const CommandSpec* spec;
};

// A page with a session history of its own, and the compositor that presents it. The window's thread creates a tab and
// posts the opening of its page to the page's thread. From then on, the page's thread owns the page, and it frees the
// tab once the page closes.
struct Tab {
    Browser* browser = nullptr;
    uint64_t key = 0;               // the tab's identity in the tab strip, unique for the run
    PrLayoutCompositor* compositor = nullptr;

    // The view of the tab's page, which is null until the page's thread opens the page. The window's thread posts input
    // and frames to it.
    Atomic<PrHtmlView*> view;

    // True while the tab is the active one. Only the active tab's compositor asks the window for frames.
    Atomic<bool> shown;

    // The window's thread's state: the factor that the page is zoomed by, and what the chrome shows of the page.
    float zoom = 1;
    TabInfo info;

    // The URL of the tab's last visit that the history recorded, so that a document that publishes again is not visited
    // again.
    String recorded;

    // What the page's thread last published, under the mutex. `changed` is true from a publication until the window's
    // thread takes it.
    PrMutex mutex{};
    TabInfo published;
    Array<String> opened;    // the URLs that the page's hyperlinks asked to show in new tabs
    bool changed = false;

    // The page's thread's state. `pending` is the URL of the navigation that the window asked for, which the chrome
    // shows until the page's document is the new one. `loadStarted` is when that navigation started, and 0 once its
    // load has been reported.
    PrHtmlSessionHistory* PR_NULLABLE history = nullptr;
    PrHtmlPage* PR_NULLABLE page = nullptr;
    String pending;
    double loadStarted = 0;

    // The page's thread's account of a viewport's changes under --trace: the viewport last seen, when the first of the
    // changes that have not settled arrived, and the jobs that they took and those laid out at a viewport already
    // replaced.
    PrVec2 traceExtent{};
    double traceResized = 0;
    uint32_t traceJobs = 0;
    uint32_t traceStale = 0;
};

// ---------------------------------------------------------------------------------------------------------------------
// The browser

// The chrome's state between rounds, on the window's thread.
struct Chrome {
    // The address bar. While the bar has focus, its text is the user's. Otherwise it shows the URL of the active tab.
    // `typed` is the text that the user last typed, which the suggestions under the bar match. `addressFocused` is true
    // if the bar had focus at the end of the last round.
    String address;
    uint64_t addressTab = 0;        // the key of the tab whose URL the bar shows
    String typed;
    bool addressFocused = false;

    bool focusAddress = false;      // true to move focus to the address bar in the next round
    bool releaseFocus = false;      // true to take focus from the chrome in the next round, as a press in the page does
    bool favoritesBar = true;       // true while the favorites bar shows

    // The history page: the text that its search matches, and what its dialog deletes of the browsing data.
    // `openDelete` is true to open the dialog in the next round.
    String historyQuery;
    bool openDelete = false;
    bool deleteHistory = true;      // the history, which also makes every link unvisited
    bool deleteCache = true;        // the HTTP cache's responses
    bool deleteSiteData = false;    // every origin's local storage

    // The box that the active tab's page fills, or none while the tab shows the start page. Its bounds come from the
    // layout of the last frame, in px, and are empty while there is no box.
    PrLayoutNode slot = kPrLayoutNodeNone;
    PrRect page{};
};

// The browser's state, which main() composes.
struct Browser {
    // The options of the command line.
    Array<String> addresses;                // the URLs that open first, a tab each
    String cacheDirectory;                  // the HTTP cache's directory, or empty for the platform's cache directory
    bool stats = false;                     // true to print the frames' statistics once a second
    bool trace = false;                     // true to print what the views do with their layout jobs

    // What every page shares: text, images, media, storage, the network, the agent whose event loop runs the pages, and
    // the script engine of the agent.
    PrFontSet* PR_NULLABLE fonts = nullptr;
    PrTextShaper* PR_NULLABLE shaper = nullptr;
    PrTextFont* PR_NULLABLE font = nullptr;
    PrCompressCodec* PR_NULLABLE compress = nullptr;
    PrImageCodec* PR_NULLABLE images = nullptr;
    PrMediaCodec* PR_NULLABLE media = nullptr;
    PrAudioOutput* PR_NULLABLE audioOutput = nullptr;
    PrAudioMixer* PR_NULLABLE mixer = nullptr;
    PrTaskPool* PR_NULLABLE io = nullptr;      // writes the files of the cache and of local storage
    PrTaskPool* PR_NULLABLE workers = nullptr; // threads that share the agent's work with the page's thread, such as
                                               // restyles
    PrStorage* PR_NULLABLE cache = nullptr;
    PrHtmlStorageProvider* PR_NULLABLE localStorage = nullptr;
    PrHtmlStorageProvider* PR_NULLABLE sessionStorage = nullptr;
    PrNet* PR_NULLABLE net = nullptr;
    PrTls* PR_NULLABLE tls = nullptr;
    PrHttp* PR_NULLABLE http = nullptr;
    PrFetchNetwork* PR_NULLABLE network = nullptr;  // fetches from the network, with the HTTP cache
    PrFetchNetwork* PR_NULLABLE fetcher = nullptr;  // answers the browser's own pages, and passes the rest to `network`
    PrDomAgent* PR_NULLABLE agent = nullptr;
    PrJsHtmlEngine* PR_NULLABLE engine = nullptr;

    // The page's thread, and the tabs whose pages are open on it. Only the page's thread reads `open`.
    PrThread thread{};
    bool started = false;
    Array<Tab*> open;

    // The URLs that the history holds, which make links to them visited (html/#selector-visited). The pages' restyles
    // read the set, on the page's thread and its workers. Tasks on the page's thread change it, which run between
    // restyles. main() fills it from the history before the thread starts.
    Set<String> visitedUrls;

    // The window's thread's tabs, in the order of the tab strip, and the index of the active one. Each new tab takes
    // the next key. `closed` holds the URLs of closed tabs, the most recent last, which reopen in turn.
    Array<Tab*> tabs;
    uint32_t active = 0;
    uint64_t nextKey = 1;
    Array<String> closed;

    // The user's favorites and history, and the files that keep them, which are empty if the platform names no data
    // directory.
    Array<Favorite> favorites;
    String favoritesPath;
    Array<Visit> history;
    String historyPath;

    // The chrome: Studio Kit over a layout tree of its own, shaped on the window's thread, in the color scheme of the
    // user's preferences, and the images of the tabs' icons, by URL.
    PrTextShaper* PR_NULLABLE chromeShaper = nullptr;
    PrLayoutTree* PR_NULLABLE tree = nullptr;
    PrUi* PR_NULLABLE ui = nullptr;
    PrKit* PR_NULLABLE kit = nullptr;
    PrCache* PR_NULLABLE icons = nullptr;
    PrAppColorScheme scheme = PrAppColorScheme_Light;
    Chrome chrome;

    // The browser's commands and the chords bound to them (commands.h), and the context of each command, by its index
    // in the registry.
    PrCommandRegistry* PR_NULLABLE commands = nullptr;
    PrCommandKeymap* PR_NULLABLE keymap = nullptr;
    Array<CommandContext> commandContexts;

    // The chrome's scene from the last frame, which its list plays, in target pixels.
    const PrLayoutScene* PR_NULLABLE scene = nullptr;
    PrDrawList* PR_NULLABLE chromeList = nullptr;
    Array<PrLayoutSceneScroll> scrolls;

    // The pointer's last position in the window, in px, and the buttons pressed in the page. While a button is pressed
    // in the page, the pointer's events go to the page.
    PrVec2 pointer{};
    uint32_t pageButtons = 0;

    // What the window's thread presents with. A frame renders the active tab's page through `pageFrame` and the chrome
    // through `chromeFrame`, in one pass.
    PrApp* PR_NULLABLE app = nullptr;
    PrGpuDevice* PR_NULLABLE device = nullptr;
    PrGpuSwapChain* PR_NULLABLE swapChain = nullptr;
    PrDrawRenderer* PR_NULLABLE renderer = nullptr;
    PrDrawFrame* PR_NULLABLE pageFrame = nullptr;
    PrDrawFrame* PR_NULLABLE chromeFrame = nullptr;
    PrGpuTexture rendered{};        // the half floats that a frame renders into if the swap chain holds more than 8-bit
                                    // sRGB
    PrDrawDisplay* PR_NULLABLE display = nullptr;
    PrUVec2 extent{};               // the window's frame's extent in target pixels
    PrAppDisplay shownOn = prAppDisplayDefault();
    Atomic<bool> frameWanted;          // starts false, as the browser is static

    // The window's last frame as the pages see it: the extent of the page's box, at the active tab's zoom. A tab's page
    // opens at this extent.
    PrAppFrame pageWindow{};

    // The page's rectangle in the frame being made, in target pixels, and whether the active tab's compositor presented
    // the frame.
    PrIVec2 pageOrigin{};
    PrUVec2 pageExtent{};
    bool presented = false;

    // The frames' statistics since they were last printed. The GPU's time is added on the device's thread.
    double statsSince = 0;
    uint32_t statsFrames = 0;
    double statsCpu = 0;               // milliseconds on the window's thread, in present
    double statsWait = 0;              // milliseconds of that spent waiting for a drawable
    Atomic<uint64_t> statsGpu;         // microseconds on the GPU, from zero as the browser is static
    PrDrawStats statsLast{};
};

// Asks the window's thread for a frame. Any thread may call it.
void browserWantFrame(Browser& b);

// Returns the active tab, or null if there is none.
Tab* PR_NULLABLE browserActiveTab(const Browser& b);

// Presents the frame of the active tab's page, which its compositor made, with the chrome over it. `canvas` is the
// color of the page's canvas. Called on the window's thread (PrLayoutPresentFn).
void browserPresent(Browser& b, PrDrawList* list, PrColor16 canvas);

// Returns the handler that runs the browser's window (prAppRun), with `b` as its context.
PrAppHandler browserHandler(Browser& b);

} // namespace browser

PR_ASSUME_NONNULL_END
