// HTML and XHTML pages as sources (documents.h): how they load and draw, for the switches in documents.cpp. Nothing
// outside those switches calls them. A loader returns null for a file it cannot read.

#pragma once

#include "pr/app.h"
#include "pr/app_display.h"
#include "pr/app_user_preferences.h"
#include "pr/draw_list.h"
#include "pr/font.h"
#include "pr/text.h"

#include <filesystem>
#include <memory>

struct PrGpuDevice;

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// Loads the WPT checkout's pages afterward from WPT's server on the loopback at the HTTP port `http` and the HTTPS port
// `https`, and returns whether a server serves them.
void serveWptFrom(uint16_t http, uint16_t https);
bool wptServed();

// An HTML or XHTML page, loaded in its own page host and run until it settles, with its text shaped by `shaper`, which
// outlives it, and its fonts hinted as `hinting` says. A page of the WPT checkout loads from WPT's server where one
// serves it (serveWptFrom), and any other page loads at its file: URL. The page runs on a virtual clock whose time zero
// is the settled page, so that its frame at a time is the same on every run. Bringing it to a time runs its event loop
// and animations to that time, and an earlier time runs the page again from its start. The page is laid out in a
// `width` by `height` viewport and painted for the display and the user's preferences, at the display's pixel ratio.
// The page is presented by a renderer on `device`, or by a renderer in memory if `device` is null.
struct HtmlSource;
std::shared_ptr<HtmlSource> loadHtml(const std::filesystem::path& path, PrTextShaper* shaper,
    PrFontHinting hinting, PrFontHinter* PR_NULLABLE hinter, PrGpuDevice* PR_NULLABLE device);
void htmlAt(HtmlSource& html, double time);
void drawHtml(HtmlSource& html, PrDrawList* list, float width, float height, const PrAppDisplay& display,
    const PrAppUserPreferences& preferences);

// Sends the page a host's input through its view, as a user's input reaches a page (PageHost::input). A position is in
// the page's viewport, in CSS pixels.
void htmlInput(HtmlSource& html, const PrAppEvent& event);

// Returns what the page's view reports to the window, in CSS pixels of the page's viewport, at `time` of the window's
// clock.
PrAppOutput htmlOutput(HtmlSource& html, double time);

// Gives the page the application whose clipboard its text controls use.
void htmlSetApp(HtmlSource& html, PrApp* app);

} // namespace viewer

PR_ASSUME_NONNULL_END
