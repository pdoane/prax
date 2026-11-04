// Pages as sources (documents.h): how they load and draw, for the switches in documents.cpp. Nothing outside those
// switches calls them. A loader returns null for a file it cannot read.

#pragma once

#include "pr/app.h"
#include "pr/app_display.h"
#include "pr/app_user_preferences.h"
#include "pr/draw_list.h"
#include "pr/draw_writer.h"
#include "pr/font.h"
#include "pr/text.h"
#include "pr/vec.h"

#include "tools/shell/page_host.h"

#include <filesystem>
#include <memory>
#include <string>

struct PrGpuDevice;

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// Loads the WPT checkout's pages afterward from WPT's server on the loopback, at the HTTP port `http` and the HTTPS
// port `https`.
void serveWptFrom(uint16_t http, uint16_t https);

// Returns true if WPT's server serves the WPT checkout's pages (serveWptFrom).
bool wptServed();

// Loads the pages of the third_party directory outside WPT's checkout afterward from an HTTP server on the loopback at
// the port `http`, which serves the directory at its root. A page that names its resources from the root, as MathFonts'
// tests do, needs the server.
void serveThirdPartyFrom(uint16_t http);

// Returns true if the third-party server serves the pages of third_party outside WPT's checkout (serveThirdPartyFrom).
bool thirdPartyServed();

// Returns the URL that the system's browser loads the page at `path` from, with `query`: the third-party server's URL
// where one serves the page, and otherwise its file: URL.
std::string htmlBrowserUrl(const std::filesystem::path& path, const std::string& query);

// A page, loaded with `query` in its own page host and run until it settles, with its text shaped by `shaper`, which
// outlives it, in the host's fonts and those of the suite's render settings `settings` if it names a file, hinted as
// `hinting` says. A `capture` that is not empty answers what the page loads from other origins (PageHost::addCapture),
// and without `scripts`, the page loads without running its scripts. A page of the WPT checkout loads from WPT's server
// where one serves it (serveWptFrom), another page of third_party from the third-party server where one serves it
// (serveThirdPartyFrom), and any other page at its file: URL. The page runs on a virtual clock whose time zero is the
// settled page, so that its frame at a time is the same on every run. Bringing it to a time runs its event loop and
// animations to that time, and an earlier time runs the page again from its start. The page is presented by a renderer
// on `device`, or by a renderer in memory if `device` is null.
//
// The page renders for `media` in a `width` by `height` viewport, painted for the display and the user's preferences,
// at the display's pixel ratio: on the screen, or printed in the pages that printHtml prints, stacked down the viewport
// (PageHost::paint). Either scrolls through its viewport's overflow.
struct HtmlSource;
std::shared_ptr<HtmlSource> loadHtml(const std::filesystem::path& path, const std::string& query,
    const std::filesystem::path& settings, const std::filesystem::path& capture, bool scripts, page_host::Media media,
    PrTextShaper* shaper, PrFontHinting hinting, PrFontHinter* PR_NULLABLE hinter, PrGpuDevice* PR_NULLABLE device);
void htmlAt(HtmlSource& html, double time);
void drawHtml(HtmlSource& html, PrDrawList* list, float width, float height, const PrAppDisplay& display,
    const PrAppUserPreferences& preferences);

// Sends the page a host's input through its view, as a user's input reaches a page (PageHost::input). A position is in
// the page's viewport, in CSS pixels.
void htmlInput(HtmlSource& html, const PrAppEvent& event);

// Returns what the page's view reports to the window, in CSS pixels of the page's viewport, at `time` of the window's
// clock.
PrAppOutput htmlOutput(HtmlSource& html, double time);

// Returns how far the page's viewport is scrolled, in CSS pixels, or scrolls it there, clamped to its range
// (cssom-view-1/#dom-window-scroll).
PrVec2 htmlScroll(HtmlSource& html);
void htmlScrollTo(HtmlSource& html, PrVec2 offset);

// Returns the width of what the page's viewport scrolls through as its last layout laid it out, in CSS pixels.
float htmlContentWidth(HtmlSource& html);

// Gives the page the application whose clipboard its text controls use.
void htmlSetApp(HtmlSource& html, PrApp* app);

// Loads the page at `path` in a host of its own, with what it loads from other origins answered by `capture` if it is
// not empty, and with or without its scripts. Runs it until it settles, and writes it to `out` as
// PageHost::writeDocument prints it, by the writer `format`. Returns false, saying why on stderr, if the page does not
// load or the document cannot be written.
bool printHtml(const std::filesystem::path& path, const std::filesystem::path& capture, bool scripts,
    const std::filesystem::path& out, PrDrawWriterBackend format);

} // namespace viewer

PR_ASSUME_NONNULL_END
