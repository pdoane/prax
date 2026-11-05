// Pages as sources (source_html.h).

#include "tools/viewer/source_html.h"

#include "pr/css_presentation.h"
#include "pr/css_view.h"
#include "pr/html.h"
#include "pr/html_page.h"
#include "pr/layout_query.h"

#include "tools/shell/page_host.h"
#include "tools/suite/suite_render.h"
#include "tools/viewer/documents.h"

#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace viewer {

namespace fs = std::filesystem;

// A page in its own host, on a virtual clock that the viewer's clock drives. `origin` is the clock's time once the page
// settled, in milliseconds, which is time zero of the viewer's clock.
struct HtmlSource {
    HtmlSource(fs::path path, std::string location, page_host::Media media, PrTextShaper* shaper) :
            path(std::move(path)), location(std::move(location)), media(media), host(shaper, nullptr, nullptr) {}

    fs::path path;
    std::string location;       // the URL or path that the page loads from
    page_host::Media media;
    page_host::PageHost host;
    page_host::Page* PR_NULLABLE page = nullptr;
    double origin = 0;
};

namespace {

constexpr const char* kThirdParty = PR_THIRD_PARTY_DIR;

// How long a page runs before the viewer stops waiting for it to settle, in milliseconds of the system's time. WPT's
// runner gives a page the same time before it times out.
constexpr double kSettleTimeout = 10000;

// Loads the page into the host and runs it until it settles.
bool start(HtmlSource& html) {
    html.page = html.host.load(html.location);
    if (html.page == nullptr)
        return false;
    html.host.settle(kSettleTimeout);
    if (!page_host::PageHost::finished(html.page))
        std::fprintf(stderr, "%s: reftest-wait was not removed\n", html.path.string().c_str());
    html.origin = html.host.time();
    return true;
}

// The ports of WPT's server, or zero without one.
uint16_t wptHttp = 0;
uint16_t wptHttps = 0;

fs::path wptCheckout() {
    return fs::path(kThirdParty) / "wpt";
}

// Returns the URL that WPT's server serves the page at `path` at, over HTTPS if its name has `.https.`, or empty if the
// page is outside the checkout or no server serves it.
std::string wptUrlOf(const fs::path& path) {
    if (!wptServed())
        return {};
    fs::path relative = fs::absolute(path).lexically_normal().lexically_relative(
        fs::absolute(wptCheckout()).lexically_normal());
    if (relative.empty() || *relative.begin() == "..")
        return {};
    bool secure = path.filename().string().find(".https.") != std::string::npos;
    std::string origin;
    if (secure)
        origin = "https://web-platform.test:" + std::to_string(wptHttps);
    else
        origin = "http://web-platform.test:" + std::to_string(wptHttp);
    return origin + "/" + relative.generic_string();
}

// Reaches WPT's server as tools/suite/wpt/wpt.py has pr_shell reach it: WPT's hosts resolve to the loopback, its CA is
// trusted, and Ahem, which WPT requires a browser to have installed, is installed.
bool reachWpt(page_host::PageHost& host) {
    if (!host.useNetwork() || !host.addTrustAnchor(wptCheckout() / "tools" / "certs" / "cacert.pem"))
        return false;
    for (const char* name : {"web-platform.test", "*.web-platform.test", "not-web-platform.test",
        "*.not-web-platform.test"})
        host.addHostRule(name, "127.0.0.1");
    return host.addFont(wptCheckout() / "fonts" / "Ahem.ttf");
}

} // namespace

void serveWptFrom(uint16_t http, uint16_t https) {
    wptHttp = http;
    wptHttps = https;
}

bool wptServed() {
    return wptHttp != 0;
}

std::shared_ptr<HtmlSource> loadHtml(const fs::path& path, const fs::path& settings, const fs::path& capture, bool scripts,
        page_host::Media media, PrTextShaper* shaper, PrFontHinting hinting, PrFontHinter* PR_NULLABLE hinter,
        PrGpuDevice* PR_NULLABLE device) {
    std::string url = wptUrlOf(path);
    std::shared_ptr<HtmlSource> html = std::make_shared<HtmlSource>(path, url.empty() ? path.string() : url, media, shaper);
    if (!settings.empty())
        suite::suiteRenderReadFonts(html->host.fonts(), settings.string(),
            fs::path(kThirdParty).parent_path().string());
    html->host.setHinting(hinting, hinter);
    html->host.setPageGap(kPageGap);
    html->host.setDevice(device);
    if (!url.empty() && !reachWpt(html->host))
        return nullptr;
    std::string captured;
    if (!capture.empty() && !html->host.addCapture(capture, &captured))
        return nullptr;
    if (!scripts)
        html->host.disableScripts();
    html->host.useVirtualTime();
    html->host.runAllAnimationFrames();
    if (!start(*html))
        return nullptr;
    return html;
}

void htmlAt(HtmlSource& html, double time) {
    double at = html.origin + time * 1000;
    // The clock never goes back, so an earlier time runs the page again from its start.
    if (at < html.host.time()) {
        html.host.reset();
        if (!start(html))
            return;
        at = html.origin + time * 1000;
    }
    html.host.advance(at);
}

void drawHtml(HtmlSource& html, PrDrawList* list, float width, float height, const PrAppDisplay& display,
        const PrAppUserPreferences& preferences) {
    if (html.page != nullptr)
        html.host.paint(html.page, html.media, PrVec2{width, height}, display, preferences, list);
}

void htmlInput(HtmlSource& html, const PrAppEvent& event) {
    if (html.page != nullptr)
        html.host.input(html.page, event);
}

PrAppOutput htmlOutput(HtmlSource& html, double time) {
    return html.page != nullptr ? html.host.output(html.page, time) : PrAppOutput{};
}

PrVec2 htmlScroll(HtmlSource& html) {
    PrHtmlPage* page = html.page != nullptr ? page_host::PageHost::htmlPage(html.page) : nullptr;
    if (page == nullptr)
        return PrVec2{0, 0};
    PrHtmlWindow* window = prHtmlPageWindow(page);
    return PrVec2{float(prHtmlWindowScrollX(window)), float(prHtmlWindowScrollY(window))};
}

void htmlScrollTo(HtmlSource& html, PrVec2 offset) {
    PrHtmlPage* page = html.page != nullptr ? page_host::PageHost::htmlPage(html.page) : nullptr;
    if (page == nullptr)
        return;
    PrCssScrollToOptions options{.hasLeft = true, .hasTop = true, .left = double(offset.x), .top = double(offset.y)};
    prHtmlWindowScroll(prHtmlPageWindow(page), &options);
}

float htmlContentWidth(HtmlSource& html) {
    PrHtmlPage* page = html.page != nullptr ? page_host::PageHost::htmlPage(html.page) : nullptr;
    PrCssPresentation* presentation = page != nullptr ? prHtmlPagePresentation(page) : nullptr;
    PrLayoutScrollInfo info{};
    if (presentation == nullptr || !prLayoutNodeScrollInfo(prCssPresentationTree(presentation), kPrLayoutNodeNone, &info))
        return 0;
    return prLayoutUnitToPx(info.content.right - info.content.left);
}

void htmlSetApp(HtmlSource& html, PrApp* app) {
    if (html.page != nullptr)
        html.host.setInteractive(html.page, app);
}

bool printHtml(const fs::path& path, const fs::path& capture, bool scripts, const fs::path& out, PrDrawWriterBackend format) {
    std::unique_ptr<PrTextShaper, decltype(&prTextShaperDestroy)> shaper(
        prTextShaperCreate(PrTextShaperBackend_Default), prTextShaperDestroy);
    page_host::PageHost host(shaper.get(), nullptr, nullptr);
    std::string captured;
    if (!capture.empty() && !host.addCapture(capture, &captured))
        return false;
    if (!scripts)
        host.disableScripts();
    page_host::Page* page = host.load(path.string());
    if (page == nullptr)
        return false;
    if (host.settle(kSettleTimeout) == page_host::Settled_TimedOut) {
        std::fprintf(stderr, "%s: did not settle in %g ms\n", path.string().c_str(), kSettleTimeout);
        host.stopWaiting();
    }
    if (!page_host::PageHost::pageOk(page))
        std::fprintf(stderr, "%s: printed after an error in its load or its scripts\n", path.string().c_str());
    std::error_code error;
    fs::create_directories(out.parent_path(), error);
    // A print lays out for its page boxes, so it takes no viewport.
    return host.writeDocument(page, page_host::Media_Print, prLayoutPageBoxDefault(), PrVec2{0, 0}, format, out);
}

} // namespace viewer
