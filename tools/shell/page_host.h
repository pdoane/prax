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
#include "pr/draw_list.h"
#include "pr/font.h"
#include "pr/media.h"
#include "pr/vec.h"

#include <filesystem>
#include <memory>
#include <string>

struct JSContext;

struct PrGpuDevice;
struct PrHtmlPage;
struct PrTextShaper;

PR_ASSUME_NONNULL_BEGIN

namespace page_host {

// A page that a host loaded. It lives as long as its host.
struct Page;

// The state of a host: its script runtime, event loop, network, fonts and pages.
struct HostState;

// A host of pages, as this header describes.
class PageHost {
public:
    // Creates a host whose pages' text is shaped by `shaper`, and whose media resources `media` reads, or none plays if
    // it is null. Each must outlive the host.
    PageHost(PrTextShaper* shaper, PrMediaCodec* PR_NULLABLE media);
    ~PageHost();
    PageHost(const PageHost&) = delete;
    PageHost& operator=(const PageHost&) = delete;

    // Closes every page, and gives the pages loaded afterward a new script runtime, event loop and clock, as a new host
    // would. Everything that serves and renders them (the settings below, the network, fonts, layout and codecs) is
    // kept, which is the saving that a run of many pages in one host makes.
    void reset();

    // Serves the resources that a captured page loads, as tools/pages/pages.py saves them. `capture` is a JSON file
    // that holds the page's URL ("url") and the Content-Type it was served with ("type"), and each resource that was
    // captured ("resources": an array of {"url", "file", "type"}, each file relative to the JSON's directory). A
    // request for a captured URL is answered from its file with its type, whatever its host, and the page loads at its
    // URL with its type. The page's URL is answered from `page`, or from the HTML file beside the JSON if `page` is
    // empty. Writes the page's URL to `pageUrl`. Returns false, saying why on stderr, if the file cannot be read.
    bool addCapture(const std::filesystem::path& capture, std::string* pageUrl, const std::filesystem::path& page = {});

    // Runs `script` in each window that the pages loaded afterward open, before its document's own scripts.
    void addPrelude(const std::filesystem::path& script);

    // Adds the font of the file `file` to the host's set. Returns false, saying why on stderr, if the file does not
    // load.
    bool addFont(const std::filesystem::path& file);

    // Hints the fonts of the pages loaded afterward, the host's and their own, by `hinter` in the mode `hinting`, or
    // draws them as designed with a null hinter, as the host does until this is called (prFontSetSetHinting). The
    // hinter must outlive the host.
    void setHinting(PrFontHinting hinting, PrFontHinter* PR_NULLABLE hinter);

    // Presents the pages loaded afterward with a renderer on `device`, or with a renderer in memory if `device` is
    // null, as the host does until this is called (PrHtmlHost::device). The device must outlive the pages.
    void setDevice(PrGpuDevice* PR_NULLABLE device);

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

    // Runs the pages on a virtual clock, in milliseconds starting from the system's time, which advances only when the
    // pages wait. A wait for a timer or a frame takes no time, and the animation frames that nothing observes are
    // skipped (PrHtmlAnimationFrames_Observed). Without this call, pages run on the system's clock. The clock stands
    // while a fetch waits for the network.
    void useVirtualTime();

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
    // full recomputation (prCssPresentationVerify). Writes each page's first failure to stderr as "<page>: render
    // verify: <what differs>". The pages loaded afterward are checked.
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

    // Runs the script file `script` in each page loaded with Capture::Wait once it has loaded, as the body of a
    // function called on the window with the page's URL and the function that stops the run (stopRun()). The script
    // stops the run once the page is ready to be captured. Returns false, saying why on stderr, if the file cannot be
    // read.
    bool setCaptureWait(const std::filesystem::path& script);

    // Returns the context that runs scripts outside any page, with the shell's functions: print(), readFile(path),
    // counters(), loadResource(url), stopRun() and sendInput(events). A page's context has them too, and sendInput
    // sends a page input through the view that presents it.
    JSContext* scriptContext();

    // Runs the script file in the script context, and the jobs that it queues. Returns false, saying why on stderr, if
    // the file cannot be read or the script throws.
    bool runFile(const char* path);

    // Whether a page is loaded to be captured, as a reftest or its reference.
    enum class Capture {
        None,
        Wait,           // the page runs the capture's wait (setCaptureWait), which stops the run (stopRun())
    };

    // Starts loading the page at `location`, an absolute URL or a path, which loads at its file: URL. The page loads as
    // the host runs its tasks (settle). Returns null, saying why on stderr, if a path names no file. A page that fails
    // to load is returned, and pageOk reports the failure once it arrives.
    Page* PR_NULLABLE load(const std::string& location, Capture capture = Capture::None);

    // Returns true if the page has loaded and run without an error so far.
    static bool pageOk(const Page* page);

    // Returns what the page was loaded from, its path or its URL as load was given it, which names the page in what the
    // host writes to stderr.
    static const std::string& pageName(const Page* page);

    // Returns the browsing context that the page runs in, or null once it is destroyed.
    static PrHtmlPage* PR_NULLABLE htmlPage(const Page* page);

    // How settle ended.
    enum class Settled {
        Idle,           // nothing was left to run, only a page's animations wanted frames, or every page that load
                        // loaded stopped the run
        TimedOut,       // the timeout passed first
        Failed,         // a job failed outside any page
    };

    // Runs the pages' due jobs, tasks and timers until none is left, only a page's animations want frames, or every
    // page that load loaded has stopped the run (stopRun()). With a `timeout` of zero or more, it stops once that many
    // milliseconds of the system's time have passed.
    Settled settle(double timeout = -1);

    // Sends the page a host's input through the view that presents it, at once, and runs the jobs that it queues. The
    // page's layout is brought up to date first, as a browser does before it hit tests an event. A position is in the
    // page's viewport, in CSS pixels.
    void input(Page* page, const PrAppEvent& event);

    // Returns true if the page is done in the sense that a reftest uses: its root element no longer has the class
    // `reftest-wait`.
    static bool finished(const Page* page);

    // Renders each page that has not finished in a viewport of `viewport` CSS pixels, and fires TestRendered at its
    // root element. TestRendered is the event of WPT's reftest protocol that tells a waiting page it has rendered, so
    // that it can go on to its final state. Returns true if it fired at any page. The pages then need to settle again.
    bool signalRendered(PrVec2 viewport);

    // Paints the page into `list`, in CSS pixels, as it renders in a viewport of `viewport` CSS pixels on `display` for
    // a user with `preferences`, over the canvas's color. The caller renders the list at the display's pixel ratio. A
    // paint that starts fetches, as the first style that names an image does, waits for them and paints again. Returns
    // false if the page has no rendering.
    bool paint(Page* page, PrVec2 viewport, const PrAppDisplay& display, const PrAppUserPreferences& preferences,
        PrDrawList* list);

private:
    std::unique_ptr<HostState> state_;
};

} // namespace page_host

PR_ASSUME_NONNULL_END
