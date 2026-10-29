// What the viewer shows: documents, each a source that the engine draws beside the references that it is compared
// against. Collections list the documents, and the viewer opens one at a time.
//
// A source is a file of a type that the engine draws, or a drawing made in code. How a source loads, updates to a time,
// draws, and opens in the system's browser depends on its type, with one switch for each (documents.cpp). Everything
// else treats every type alike: a reference that the engine draws is also a source, opened in the same way as the
// document.

#pragma once

#include "pr/app.h"
#include "pr/app_display.h"
#include "pr/app_user_preferences.h"
#include "pr/color.h"
#include "pr/draw_list.h"
#include "pr/font.h"
#include "pr/text.h"
#include "pr/vec.h"

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

// A document drawn in code (drawing.h).
struct Drawing;

// The text renderers of a document: the shaper that sets its text, and how its fonts are hinted.
struct TextBackends {
    PrTextShaperBackend shaper = PrTextShaperBackend_Default;
    PrFontHinting hinting = PrFontHinting_None;

    bool operator==(const TextBackends&) const = default;
};

// The types of source, each of which documents.cpp loads, updates and draws in its own way.
enum {
    SourceType_Drawing,       // drawn in code (drawing.h)
    SourceType_Svg,           // an SVG document, rendered by lib/svg
    SourceType_Html,          // an HTML, XHTML or Markdown page, loaded, run and rendered by lib/html and lib/layout
    SourceType_Lottie,        // a Lottie animation, drawn by lib/lottie
    SourceType_Rlottie,       // a Lottie animation, played by rlottie
    SourceType_Image,         // a PNG
    SourceType_Snapshots,     // PNGs of an animation at times, which another player drew
};
typedef uint8_t SourceType;

// What draws: a file of a type, or a drawing.
struct Source {
    SourceType type = SourceType_Drawing;
    std::filesystem::path path{};               // every type except Drawing
    const Drawing* PR_NULLABLE drawing = nullptr;

    // Svg and Lottie: the suite's render settings that it draws with (tools/suite/suite_render.h), or empty for the
    // defaults.
    std::filesystem::path settings{};

    // Snapshots: each PNG and the time in seconds that it shows, in order of time, and the animation's length in
    // seconds, over which the clock loops. The snapshot nearest the time is shown.
    std::vector<std::pair<double, std::filesystem::path>> snapshots{};
    double length = 0;
};

// Returns the source for a file, by its extension: an SVG document, a PNG, a Lottie animation for JSON, or otherwise a
// page, which is HTML, XHTML or Markdown by the type that the page host serves it as. An Svg source draws with
// `settings`.
Source fileSource(const std::filesystem::path& path, const std::filesystem::path& settings = {});

// A named render that a document is compared against: a source the engine draws, or, if `url` is set, a page that the
// system's browser loads.
struct Reference {
    std::string name;
    Source source{};
    std::string url{};
};

// A document, as a collection lists it.
struct Document {
    std::string name;
    std::string description;
    Source source;

    // The references that it is compared against, after the browser that shows its source (browserReference). Every
    // document whose source has a page gets that reference first.
    std::vector<Reference> references{};

    float width = 0;                    // the size that the document lays out for, if the listing knows it
    float height = 0;

    // The directories that the tree lists it under, outermost first, if they are not the ones that its name's slashes
    // separate.
    std::vector<std::string> folders{};
};

// Returns the system's browser showing the source, or a reference with no url for a source that the browser does not
// show.
Reference browserReference(const Source& source);

// Returns the document's references: the browser's, then its own.
std::vector<Reference> referencesOf(const Document& document);

// A loaded source: what its type read, ready to draw. It is drawn from one thread at a time, which its calls ensure.
struct Loaded;

// Loads the source with its own shaper, of the kind that `text` names, which sets its text, and with its fonts hinted
// as `text` says. The source is presented by a renderer on `device`, or by a renderer in memory if `device` is null,
// and `device` must outlive it. A source that does not load draws nothing.
std::shared_ptr<Loaded> load(const Source& source, TextBackends text, PrGpuDevice* PR_NULLABLE device);

// Return the size that the source lays out for, or zero if it takes the size it is drawn at, and the color that the
// host clears to beneath it.
PrVec2 naturalSize(const Loaded& loaded);
PrColor background(const Loaded& loaded);

// Brings the source to `time` in seconds, and returns its frame at that time, as its format numbers frames. Returns a
// negative value for a source whose format has no frames. A still source ignores the time.
double update(Loaded& loaded, double time);

// For a source that renders its own pixels rather than drawing through a draw list, writes those pixels, `width` by
// `height`, at the time that the source was brought to, as premultiplied BGRA over white. Returns false for any other
// source, or if the source has no pixels of that size. The source is then drawn instead.
bool pixels(Loaded& loaded, uint32_t width, uint32_t height, std::vector<uint8_t>& bgra);

// Returns true if the source renders only its own pixels, of a whole document, and draws nothing into a list. Such a
// source is a player other than the engine.
bool pixelsOnly(const Loaded& loaded);

// Draws the source into the list at `width` by `height` logical pixels, at the time it was brought to, for `display`
// and the user's `preferences`. The list's transform applies the display's pixel ratio. A source that snaps to device
// pixels, as a page's layout does, lays out for the pixel ratio. A page resolves its colors for the display, and
// follows the preferences.
void draw(Loaded& loaded, PrDrawList* list, float width, float height, const PrAppDisplay& display,
          const PrAppUserPreferences& preferences);

// Returns true if the source takes a host's input, as an HTML page does.
bool takesInput(const Loaded& loaded);

// Sends the source a host's input, if it takes input. A position is in the document's units. The source draws the
// input's effect from its next draw on.
void input(Loaded& loaded, const PrAppEvent& event);

// A named, ordered set of documents, listed on first access. If a view fits the documents to a pane rather than giving
// them the pane's size, they lay out for `width` by `height` logical pixels.
struct Collection {
    Collection(std::string name, float width, float height, float pixelRatio,
               std::function<std::vector<Document>()> list)
        : name(std::move(name)), width(width), height(height), pixelRatio(pixelRatio), list(std::move(list)) {}

    std::string name;
    float width;
    float height;

    // The pixels per unit of the documents' image references, at which a comparison with one is measured, in the same
    // way that the suite's runner measures it. 0 uses each image's own density.
    float pixelRatio;

    std::function<std::vector<Document>()> list;    // lists the documents

    // Returns the documents, which `list` lists on first access.
    const std::vector<Document>& documents() const;

private:
    mutable std::shared_ptr<const std::vector<Document>> listed_;
};

// Returns every collection, in the order that the viewer offers them: the drawings' collections, then the collection of
// each suite that dev.sh has fetched (suites.h).
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
