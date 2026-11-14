// What the viewer shows: documents, each a source that the engine draws beside the references that it is compared
// against. Collections list the documents, and the viewer opens one at a time.
//
// Every source is the document of a viewport: it is laid out in the viewport that its collection declares, drawn from
// the viewport's top left, and scrolled through its own overflow. A source that has an extent of its own and no
// viewport is fitted to the viewport and centered (fittedIn). A source is a page wherever the engine can host one: an
// HTML, XHTML or Markdown page, an SVG document, and an image or a series of images, which a page made for it shows
// (imagePage, snapshotPage). Only what no page hosts has a type of its own: a Lottie animation, which lib/lottie draws
// and no page element plays; rlottie, a player other than the engine; and a PDF, which the platform rasterizes. A
// reference that the engine draws is also a source, opened in the same way as the document.

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

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

struct PrGpuDevice;

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// The text renderers of a document: the shaper that sets its text, and how its fonts are hinted.
struct TextBackends {
    PrTextShaperBackend shaper = PrTextShaperBackend_Default;
    PrFontHinting hinting = PrFontHinting_None;

    bool operator==(const TextBackends&) const = default;
};

// The types of source: a page, or what no page hosts.
enum {
    SourceType_Page,          // a page that the engine loads and runs (source_html.h)
    SourceType_Lottie,        // a Lottie animation, drawn by lib/lottie
    SourceType_Rlottie,       // a Lottie animation, played by rlottie
    SourceType_Pdf,           // a PDF, whose pages the platform rasterizes
};
typedef uint8_t SourceType;

// What draws: a file of a type.
struct Source {
    SourceType type = SourceType_Page;
    std::filesystem::path path{};

    // Page and Lottie: the suite's render settings that its text is set with (tools/suite/suite_render.h), or empty for
    // the defaults.
    std::filesystem::path settings{};

    // Page: the capture that answers what the page loads from other origins (PageHost::addCapture), or empty for none.
    // A fetch that leaves the machine fails, and only the page's files, its capture and WPT's server answer.
    std::filesystem::path capture{};

    bool scripts = true;                        // Page: false loads the page without running its scripts

    std::string query{};                        // Page: the query of its URL, without its `?`, or empty for none
};

// The space between the pages of a printed document wherever the viewer shows them, in CSS pixels: a printed page's
// paged view, and a PDF's pages.
constexpr float kPageGap = 12;

// A loaded source: what its type read, ready to draw. It is drawn from one thread at a time, which its calls ensure.
struct Loaded;

// Returns true if the engine prints the source, which a pane's media applies to: a page.
bool prints(const Source& source);

// Returns the directory that the viewer writes the documents that it prints and the pages that it makes into, which
// outlives the process.
std::filesystem::path viewerCache();

// Returns the source for a file, by its extension: a Lottie animation for JSON, a page that shows the image for a PNG
// (imagePage), and otherwise a page of the file, which is HTML, XHTML, Markdown or SVG by the type that the page host
// serves it as. A page or an animation sets its text with `settings`.
Source fileSource(const std::filesystem::path& path, const std::filesystem::path& settings = {});

// Returns a page, made in the viewer's cache, that shows the image at `path` as its document, fitted to the viewport
// and centered, as an SVG document without a size of its own fills it.
std::filesystem::path imagePage(const std::filesystem::path& path);

// Returns a page, made in the viewer's cache, that prints each SVG document of `pages` as a page of its own size, in
// order, so that print media shows them as the engine's paged view shows its own pages.
std::filesystem::path pagesPage(const std::vector<std::filesystem::path>& pages);

// Returns a page, made in the viewer's cache, that shows each image of `snapshots` at the time in seconds that it
// holds, nearest first, looping over `length` seconds on the page's clock. Each image is fitted to the viewport and
// centered, as imagePage shows an image.
std::filesystem::path snapshotPage(const std::vector<std::pair<double, std::filesystem::path>>& snapshots, double length);

// Returns where a document that has an extent of its own and no viewport, an image or an animation, shows in a viewport
// of `viewport`: fitted to it and centered, as an image page shows its image (imagePage) and the Lottie players place
// an animation. Both extents are in CSS pixels.
PrRect fittedIn(PrVec2 extent, PrVec2 viewport);

// A document that the engine prints of a page, as `./dev.sh document` prints one (PageHost::writeDocument): paginated
// by its @page rules, its content marked, and written by the writer `format`, as a PDF tagged with its document's
// structure or as an SVG.
struct Printed {
    std::filesystem::path path;         // where the document is written; empty for none
    PrDrawWriterBackend format = PrDrawWriterBackend_Pdf;
};

// A named render that a document is compared against: a source the engine draws, a page that the system's browser loads
// from `url`, or a document that the engine prints of the source (`printed`), which shows once it is printed
// (loadPrinted). A reference's name says what it is and, for a document, its format, such as "Prince PDF", in title
// case.
struct Reference {
    std::string name;
    Source source{};
    std::string url{};
    Printed printed{};

    // True for the system's browser showing the document's page (browserReference). For print media, the browser prints
    // the page itself, and its PDF is shown instead.
    bool browsed = false;
};

// A document, as a collection lists it.
struct Document {
    std::string name;
    std::string description;
    Source source;

    // The references that it is compared against, after the browser that shows its source (browserReference). Every
    // document whose source has a page gets that reference first.
    std::vector<Reference> references{};

    // The directories that the tree lists it under, outermost first, if they are not the ones that its name's slashes
    // separate.
    std::vector<std::string> folders{};
};

// Returns the system's browser showing the source, or a reference with no url for a source that the browser does not
// show.
Reference browserReference(const Source& source);

// Returns true if the platform rasterizes PDFs, as Core Graphics does on macOS.
bool pdfDrawn();

// Returns a reference named `name` to the PDF at `path`: a PDF source if the platform draws PDFs (pdfDrawn), and
// otherwise the system's browser showing the PDF.
Reference pdfReference(const std::string& name, const std::filesystem::path& path);

// Loads the source that shows the printed document at `path` in `format`: a PDF source where the platform rasterizes
// PDFs, and for SVG, a page of the document's files (PageHost::documentFiles) printed as pages (pagesPage). Returns
// null for a PDF that the system's browser shows instead.
std::shared_ptr<Loaded> loadPrinted(const std::filesystem::path& path, PrDrawWriterBackend format);

// Returns the document's references: the browser's, then its own.
std::vector<Reference> referencesOf(const Document& document);

// Prints the reference's source to its document (Printed), unless this process printed it after the last change to the
// files of the source's directory, which are what a page loads. A change to the engine therefore shows in the document
// once the viewer runs again, and a change to the page shows once the document opens again. Returns true if the
// document is up to date. Returns false, saying why on stderr, if the source is not a page or the page does not print.
// A print may take seconds. One print runs at a time, and a call from another thread waits for it.
bool print(const Reference& reference);

// Loads the source with its own shaper, of the kind that `text` names, which sets its text, and with its fonts hinted
// as `text` says. A source that the engine prints (prints) renders for `media`, and any other ignores it. The source is
// presented by a renderer on `device`, or by a renderer in memory if `device` is null, and `device` must outlive it. A
// source that does not load draws nothing.
std::shared_ptr<Loaded> load(const Source& source, TextBackends text, page_host::Media media, PrGpuDevice* PR_NULLABLE device);

// Brings the source to `time` in seconds, and returns its frame at that time, as its format numbers frames. Returns a
// negative value for a source whose format has no frames. A still source ignores the time.
double update(Loaded& loaded, double time);

// For a source that renders its own pixels rather than drawing through a draw list, writes those pixels as its viewport
// of `viewport` CSS pixels shows it, at the time that the source was brought to, as premultiplied BGRA over white:
// `width` by `height` from the viewport's top left, at `pixelRatio` target pixels per CSS pixel. Returns false for any
// other source. A source that renders only its own pixels (pixelsOnly) is a player other than the engine.
bool pixels(Loaded& loaded, PrVec2 viewport, uint32_t width, uint32_t height, float pixelRatio, std::vector<uint8_t>& bgra);
bool pixelsOnly(const Loaded& loaded);

// Draws the source into the list as its viewport of `width` by `height` CSS pixels shows it, at the time it was brought
// to, for `display` and the user's `preferences`. The list's transform applies the display's pixel ratio. A source that
// snaps to target pixels, as a page's layout does, lays out for the pixel ratio. A page resolves its colors for the
// display, and follows the preferences.
void draw(Loaded& loaded, PrDrawList* list, float width, float height, const PrAppDisplay& display,
    const PrAppUserPreferences& preferences);

// Sends the source a host's input. A position is in the source's viewport, in CSS pixels. A page takes the pointer, the
// wheel and the keys as a browser's page does, and a PDF takes the wheel. The source draws the input's effect from its
// next draw on.
void input(Loaded& loaded, const PrAppEvent& event);

// Returns how far the source's viewport is scrolled, in CSS pixels, or scrolls it there, as far as it scrolls.
PrVec2 scroll(Loaded& loaded);
void scrollTo(Loaded& loaded, PrVec2 offset);

// Returns the width of what the source's viewport scrolls through, in CSS pixels, as its last draw laid it out: the
// wider of the viewport and the document's content. An animation fits its viewport, so it is as wide as the viewport.
float contentWidth(Loaded& loaded);

// Returns what a page reports to `window`, in its own px, at `time` of the window's clock, and gives it the window,
// whose clipboard it uses. Any other source reports nothing.
PrAppOutput output(Loaded& loaded, PrAppWindow* window, double time);

// A named, ordered set of documents, listed on first access. Every document of the collection, and every reference, is
// laid out in a viewport of `width` by `height` CSS pixels, the viewport that the collection's documents are written
// for, in a pane and in a render alike.
struct Collection {
    Collection(std::string name, float width, float height, std::function<std::vector<Document>()> list) :
            name(std::move(name)), width(width), height(height), list(std::move(list)) {}

    std::string name;
    float width;
    float height;
    std::function<std::vector<Document>()> list;    // lists the documents

    // The media that the engine renders the documents' pages for, until a pane chooses another.
    page_host::Media media = page_host::Media_Screen;

    // Returns the documents, which `list` lists on first access.
    const std::vector<Document>& documents() const;

private:
    mutable std::shared_ptr<const std::vector<Document>> listed_;
};

// Returns every collection, in the order that the viewer offers them: the collection of each suite that dev.sh has
// fetched.
const std::vector<Collection>& allCollections();

// Returns `text` escaped for use in a URL's query.
std::string queryEscaped(const std::string& text);

// Returns the file's URL, as a browser loads it.
std::string fileUrl(const std::filesystem::path& path);

// Returns the file's bytes, or empty if it cannot be read.
std::string readFile(const std::filesystem::path& path);

// Returns the text of the title element of the HTML file at `path`, or empty if it has none.
std::string htmlTitle(const std::filesystem::path& path);

// Returns the document's name as a file name: lowercase letters and digits, with dashes between words.
std::string fileNameOf(const std::string& name);

// Returns true if `which` names the document at `index`, either by its number from one or by part of its name. Names
// are compared as fileNameOf gives them. A null `which` names every document.
bool documentNamed(const Collection& collection, uint32_t index, const char* which);

} // namespace viewer

PR_ASSUME_NONNULL_END
