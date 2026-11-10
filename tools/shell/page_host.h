// A host for HTML pages: loads them through lib/html's host callbacks, runs their scripts with the engine's JavaScript
// APIs until they settle, and paints their rendering.
//
// A page loads by URL: its page navigates to it through the host's fetcher, as a browser loads a document. A path loads
// at its file: URL. Unless the host uses sockets (useNetwork), the pages' network runs on the host's thread before each
// task, so that every run answers its fetches in the same order. A page that does not load is written to stderr. Each
// page runs its scripts in its own context, and an iframe's page in another context, all on one event loop. Pages
// render in the checkout's fonts, which are metric-compatible with the faces that pages expect for the generic
// families, and in the fonts added to the host, which act as fonts installed on the system. Text is shaped by the
// shaper that the host is created with.
//
// A host and its pages are used from one thread at a time.

#pragma once

#include "pr/app.h"
#include "pr/app_display.h"
#include "pr/app_user_preferences.h"
#include "pr/audio_mixer.h"
#include "pr/draw_list.h"
#include "pr/draw_structure.h"
#include "pr/draw_writer.h"
#include "pr/font.h"
#include "pr/layout_page.h"
#include "pr/media.h"
#include "pr/task.h"
#include "pr/vec.h"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

struct JSContext;

struct PrGpuDevice;
struct PrHtmlPage;
struct PrHtmlView;
struct PrTextShaper;

PR_ASSUME_NONNULL_BEGIN

namespace page_host {

// A page that a host loaded. It lives as long as its host.
struct Page;

// The state of a host: its script runtime, event loop, network, fonts and pages.
struct HostState;

// Whether a page is loaded to be captured, as a reftest or its reference.
enum {
    Capture_None,
    Capture_Wait,       // the page runs the capture's wait (setCaptureWait), which stops the run (stopRun())
};
typedef uint8_t Capture;

// How a host's settle ended.
enum {
    Settled_Idle,       // nothing was left to run, only a page's animations wanted frames, or every page that load
                        // loaded stopped the run
    Settled_TimedOut,   // the timeout passed first
    Settled_Failed,     // a job failed outside any page
};
typedef uint8_t Settled;

// The media type that a page renders for (mediaqueries-4/#media-types).
enum {
    Media_Screen,   // as it renders in the viewport (PageHost::paint)
    Media_Print,    // printed, in the pages of its page boxes (PageHost::print)
};
typedef uint8_t Media;

// A host of pages, as this header describes.
class PageHost {
public:
    // Creates a host whose pages' text is shaped by `shaper`, whose media resources `media` reads, or none plays if it
    // is null, and whose media elements' audio `mixer` plays, or they play without sound if it is null. Each must
    // outlive the host.
    PageHost(PrTextShaper* shaper, PrMediaCodec* PR_NULLABLE media, PrAudioMixer* PR_NULLABLE mixer);
    ~PageHost();
    PageHost(const PageHost&) = delete;
    PageHost& operator=(const PageHost&) = delete;

    // Closes every page, and gives the pages loaded afterward a new script runtime, event loop and clock, as a new host
    // would. Everything that serves and renders them (the settings below, the network, fonts, layout and codecs) is
    // kept, which is the saving that a run of many pages in one host makes.
    void reset();

    // Serves the resources that a captured page loads, as tools/pages/pages.py saves them. `capture` is a JSON file
    // that holds the page's URL ("url") and the Content-Type it was served with ("type"), each resource that was
    // captured ("resources": an array of {"url", "file", "type"}, each file relative to the JSON's directory), and each
    // that was left out ("omitted": an array of {"url", "reason"}). A request for a captured URL is answered from its
    // file with its type, whatever its host, and the page loads at its URL with its type. A request for an omitted URL
    // is a network error, so that a capture never reaches the network. A captured response is the local copy of what
    // the page loads, so it is answered as a local one is: as from the request's own origin
    // (PrFetchResponseType_Basic), with no CORS check. The page's URL is answered from `page`, or from the HTML file
    // beside the JSON if `page` is empty. Writes the page's URL to `pageUrl`. Returns false, saying why on stderr, if
    // the file cannot be read.
    bool addCapture(const std::filesystem::path& capture, std::string* pageUrl, const std::filesystem::path& page = {});

    // Runs `script` in each window that the pages loaded afterward open, before its document's own scripts.
    void addPrelude(const std::filesystem::path& script);

    // Adds the font of the file `file` to the host's set. Returns false, saying why on stderr, if the file does not
    // load.
    bool addFont(const std::filesystem::path& file);

    // Returns the host's font set, which the pages set their text in, to add faces and families to. The host owns it.
    PrFontSet* fonts();

    // Hints the fonts of the pages loaded afterward, the host's and their own, by `hinter` in the mode `hinting`, or
    // draws them as designed with a null hinter, as the host does until this is called (prFontSetSetHinting). The
    // hinter must outlive the host.
    void setHinting(PrFontHinting hinting, PrFontHinter* PR_NULLABLE hinter);

    // Presents the pages loaded afterward with a renderer on `device`, or with a renderer in memory if `device` is
    // null, as the host does until this is called (PrHtmlHost::device). The device must outlive the pages.
    void setDevice(PrGpuDevice* PR_NULLABLE device);

    // Sets the gap between printed pages, in CSS pixels, as a paint for print media stacks them down the viewport
    // (prHtmlViewSetPaged). The gap is zero until this is called.
    void setPageGap(float gap);

    // Loads the pages afterward without running their scripts, as a browser with scripting disabled does. Scripts that
    // the host runs (runFile) still run.
    void disableScripts();

    // Loads the pages afterward for a user with `preferences`, or returns the preferences that they load with. They
    // load with neutral preferences until this is called.
    void setUserPreferences(const PrAppUserPreferences& preferences);
    PrAppUserPreferences userPreferences() const;

    // Loads the pages afterward in a viewport of `viewport` CSS pixels, so that their scripts measure the layout that a
    // paint at that viewport renders. They load in WPT's 800x600 until this is called.
    void setViewport(PrVec2 viewport);

    // Restyles on `threads` threads, including the event loop's own thread, as PrDomAgentOptions::restyleThreads
    // describes: 0 means one thread per core. Call it before any page loads.
    void setRestyleThreads(uint32_t threads);

    // Runs the pages on a virtual clock, in milliseconds, which starts at the same time in every run and advances only
    // when the pages wait. A wait for a timer or a frame takes no time, and the animation frames that nothing observes
    // are skipped (PrHtmlAnimationFrames_Observed). Without this call, pages run on the system's clock. The clock
    // stands while a fetch waits for the network.
    void useVirtualTime();

    // Renders `output`, a memory output that its host paces (PrAudioMemoryPace_Host), in step with the virtual clock:
    // each time the clock moves, the output renders the frames up to it, on the host's thread. So what the pages play
    // on it depends only on the virtual clock. The output must outlive the host.
    void paceAudio(PrAudioOutput* output);

    // Has painting wait for no more responses, for a run that has passed its timeout and answers with what it reached.
    void stopWaiting();

    // Runs every animation frame that an animation wants (PrHtmlAnimationFrames_All), on a virtual clock too, for a
    // host that shows the pages' animations. The pages loaded afterward run this way.
    void runAllAnimationFrames();

    // Returns the time of the pages' clock, in milliseconds.
    double time() const;

    // Runs the pages on the virtual clock until `until`, in milliseconds: every job, task, timer and animation frame
    // that is due by then runs, each at its own time, and the clock then stands at `until`. A time before the clock's
    // runs only what is due now. Does nothing on the system's clock.
    void advance(double until);

    // Checks each page's presentation after every rendering update, and once more when verify() is called, against a
    // full recomputation (prCssPresentationVerify). Writes each page's first failure to stderr as "<page>: presentation
    // verify: <what differs>". Checks the layout of each print's pages too (prLayoutTreeVerify), and writes its failure
    // as "<page>: print verify: <what differs>". The pages loaded afterward are checked.
    void verifyRendering();

    // Brings each page's presentation up to date in a viewport of `viewport` CSS pixels and checks it, as
    // verifyRendering does after a rendering update. Returns the number of pages whose presentation has failed a check
    // so far.
    uint32_t verify(PrVec2 viewport);

    // Fetches over sockets, as a browser does, rather than on the host's thread. Call it before any page loads. Returns
    // false, saying why on stderr, if the build has no network or a page has loaded.
    bool useNetwork();

    // Fetches over sockets with an HTTP cache on disk in `directory`, which outlives the process, as a browser's does.
    // Call it before any page loads. Returns false, saying why on stderr, if the network cannot be used.
    bool useDiskCache(const std::filesystem::path& directory);

    // Resolves `host` to `target` over sockets, as a hosts file does. A host of the form `*.name` covers every
    // subdomain of the name (PrHttpHostRule). Call it before any page loads.
    void addHostRule(const std::string& host, const std::string& target);

    // Trusts the certificate in the PEM file `pem` over HTTPS, as well as the system's. Call it before any page loads.
    // Returns false, saying why on stderr, if the file holds no certificate.
    bool addTrustAnchor(const std::filesystem::path& pem);

    // Runs the script file `script` in each page loaded with Capture_Wait once it has loaded, as the body of a function
    // called on the window with the page's URL and the function that stops the run (stopRun()). The script stops the
    // run once the page is ready to be captured. Returns false, saying why on stderr, if the file cannot be read.
    bool setCaptureWait(const std::filesystem::path& script);

    // Returns the context that runs scripts outside any page, with the shell's functions: print(), readFile(path),
    // counters(), loadResource(url), stopRun() and sendInput(events). A page's context has them too, and sendInput
    // sends a page input through the view that presents it.
    JSContext* scriptContext();

    // Runs the script file in the script context, and the jobs that it queues. Returns false, saying why on stderr, if
    // the file cannot be read or the script throws.
    bool runFile(const char* path);

    // Starts loading the page at `location`, an absolute URL or a path, which loads at its file: URL. The page loads as
    // the host runs its tasks (settle). Returns null, saying why on stderr, if a path names no file. A page that fails
    // to load is returned, and pageOk reports the failure once it arrives.
    Page* PR_NULLABLE load(const std::string& location, Capture capture = Capture_None);

    // Returns true if the page has loaded and run without an error so far.
    static bool pageOk(const Page* page);

    // Returns what the page was loaded from, its path or its URL as load was given it, which names the page in what the
    // host writes to stderr.
    static const std::string& pageName(const Page* page);

    // Returns the browsing context that the page runs in, or null once it is destroyed.
    static PrHtmlPage* PR_NULLABLE htmlPage(const Page* page);

    // Runs the pages' due jobs, tasks and timers until none is left, only a page's animations want frames, or every
    // page that load loaded has stopped the run (stopRun()). With a `timeout` of zero or more, it stops once that many
    // milliseconds of the system's time have passed.
    Settled settle(double timeout = -1);

    // Sends the page a host's input through the view that presents it, at once, and runs the jobs that it queues. The
    // page's layout is brought up to date first, as a browser does before it hit tests an event. A position is in the
    // page's viewport, in CSS pixels.
    void input(Page* page, const PrAppEvent& event);

    // Returns what the view that presents the page reports to the window, in the page's px, at `time` of the window's
    // clock, which its caret blinks by.
    PrAppOutput output(Page* page, double time);

    // Readies the view that presents the page for a person to edit in: it shows its caret, which a host that compares
    // renders keeps out of them, and it uses the clipboard of `window`.
    void setInteractive(Page* page, PrAppWindow* window);

    // Returns true if the page is done in the sense that a reftest uses: its root element no longer has the class
    // `reftest-wait`.
    static bool finished(const Page* page);

    // Renders each page that has not finished in a viewport of `viewport` CSS pixels, and fires TestRendered at its
    // root element. TestRendered is the event of WPT's reftest protocol that tells a waiting page it has rendered, so
    // that it can go on to its final state. Returns true if it fired at any page. The pages then need to settle again.
    bool signalRendered(PrVec2 viewport);

    // Paints the page into `list`, in CSS pixels, as a viewport of `viewport` CSS pixels shows it in `media`, on
    // `display` for a user with `preferences`. Returns false if the page has no rendering.
    //
    // On the screen, the page paints over its canvas's color. In print, its pages are on US letter paper with no
    // margins until the document's @page rules change it. They stack down the viewport as a paged view shows them
    // (prHtmlViewSetPaged), the page gap apart (setPageGap), each on white paper, with nothing painted between them.
    //
    // Either media scrolls by the window's scroll offset and the user's input. The caller renders the list at the
    // display's pixel ratio. A paint that starts fetches, as the first style that names an image does, waits for them
    // and paints again. With a `structure`, the paint marks the page's content and appends the document's structure to
    // `structure`, for a writer of documents to join (pr/html_structure.h).
    bool paint(Page* page, Media media, PrVec2 viewport, const PrAppDisplay& display,
        const PrAppUserPreferences& preferences, PrDrawList* list, PrDrawStructure* PR_NULLABLE structure = nullptr);

    // A page of a printed document: its paint, and its sheet as a writer takes it (PrDrawPage), in CSS pixels from the
    // sheet's top left. The caller releases the list.
    struct PrintedPage {
        PrDrawList* list;
        PrVec2 extent;
        PrRect bleed;
        PrRect trim;
    };

    // How long each phase of a print took, in milliseconds.
    struct PrintTimings {
        double paginate;    // style and layout into pages, with the fetches that they start
        double paint;       // the paint of every page
        double structure;   // the document's structure
    };

    // Prints the page's document for print media (css-page-3) on `display` for a user with `preferences`, and appends
    // its pages to `pages`. A page's box is `initial`, as a printer's paper and margins give it, until the document's
    // @page rules change it. The caller renders the pages at the display's pixel ratio. A print waits for the fetches
    // that it starts, as a paint does. Returns false if the page has no rendering. With a `structure`, the pages mark
    // their content and the document's structure is appended to `structure`, as for a paint. The view stays paged
    // afterwards, so that the next print in the same page box lays out only what changed, until a paint for the screen.
    // With `timings`, the print reports how long each of its phases took.
    bool print(Page* page, const PrLayoutPageBox& initial, const PrAppDisplay& display,
        const PrAppUserPreferences& preferences, std::vector<PrintedPage>& pages,
        PrDrawStructure* PR_NULLABLE structure = nullptr, PrintTimings* PR_NULLABLE timings = nullptr);

    // What writing a document made, and how long each phase took, in milliseconds.
    struct DocumentTimings {
        uint32_t pages;
        size_t bytes;
        PrintTimings print;     // zero for the screen
        double other;           // the rest of the paint and the structure, with the writer's setup
        double write;           // the writer
        double file;            // the file's write
    };

    // Writes the page as a document of the writer `backend` (pr/draw_writer.h) to `path`, in `media`. A printed page's
    // box is `pageBox`, as a printer's paper and margins give it, until the document's @page rules change it. The
    // screen is laid out in a viewport of `viewport` CSS pixels. Either renders on the default display for the host's
    // user preferences. The document is tagged with the document's structure (pr/html_structure.h) if its format tags
    // content, its streams are compressed, and the writer shares the host's workers. A format without pages writes a
    // file for each page, named as documentFiles names them, and removes the files of an earlier write that this one
    // does not replace. Returns false, saying why on stderr, if the page has no rendering or a file cannot be written.
    // With `timings`, the call reports what it made and how long each phase took.
    bool writeDocument(Page* page, Media media, const PrLayoutPageBox& pageBox, PrVec2 viewport,
        PrDrawWriterBackend backend, const std::filesystem::path& path, DocumentTimings* PR_NULLABLE timings = nullptr);

    // Returns the files of the document written to `path` (writeDocument), in order: `path` itself for a document of
    // one file, as a format with pages writes, and otherwise one file for each page, named for `path` with the page's
    // number from 1 before the extension, as page-1.svg and page-2.svg are for page.svg. Returns none if neither
    // exists.
    static std::vector<std::filesystem::path> documentFiles(const std::filesystem::path& path);

    // The threads that share the work of the host's pages with the thread that runs them, or null for none. Work that
    // the host runs between the pages' tasks, such as writing a document, may share them too.
    PrTaskPool* PR_NULLABLE workers() const;

    // Returns the title of the page's document, or empty.
    static std::string pageTitle(Page* page);

private:
    // Brings the view's document up to date with `prepare`, and again after the tasks that the fetches it started
    // deliver, until a pass runs none.
    void prepareWithFetches(PrHtmlView* view, void (*prepare)(PrHtmlView* view));

    std::unique_ptr<HostState> state_;
};

} // namespace page_host

PR_ASSUME_NONNULL_END
