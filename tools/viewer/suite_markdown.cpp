// The examples of the Markdown suites (suite_markdown.h), read from their spec files as
// tools/suite/markdown/markdown_spec.py reads them. Each example is written to the viewer's cache as a Markdown file,
// which the engine loads as a text/markdown document, beside an HTML file of the HTML that the example expects, which
// is the example's reference. A headers file names the variant of Markdown that the example's extensions call for,
// which the load parses.

#include "tools/viewer/suite_markdown.h"

#include <fstream>
#include <sstream>

namespace viewer {

namespace {

namespace fs = std::filesystem;

constexpr const char* kThirdParty = PR_THIRD_PARTY_DIR;

// The line that opens an example, after which its extensions are named, and the line that closes it.
const std::string kFence(32, '`');

// The character that the spec files write for a tab, U+2192 RIGHTWARDS ARROW.
constexpr const char* kTabArrow = "\xe2\x86\x92";

// The styles of a reference, which are those that lib/html's user agent sheet gives a Markdown document, so that a
// reference draws as its example does wherever the HTML is the same. A change to that sheet is made here too.
constexpr const char* kReferenceStyle = "html { color-scheme: light dark; }\n"
    "body { max-width: 48em; margin: 0 auto; padding: 1em 2em; font-family: sans-serif; line-height: 1.5; }\n"
    "h1, h2 { padding-block-end: 0.3em; border-block-end: 1px solid color-mix(in srgb, CanvasText 20%, Canvas); }\n"
    "h1, h2, h3, h4, h5, h6 { margin-block: 1.5em 0.75em; line-height: 1.25; }\n"
    "code { padding: 0.1em 0.3em; border-radius: 4px; font-size: 0.9em;"
    " background-color: color-mix(in srgb, CanvasText 8%, Canvas); }\n"
    "pre { padding: 1em; border-radius: 6px; overflow: auto; line-height: 1.4;"
    " background-color: color-mix(in srgb, CanvasText 8%, Canvas); }\n"
    "pre > code { padding: 0; font-size: inherit; background-color: transparent; }\n"
    "blockquote { margin-inline: 0; padding-inline: 1em; color: color-mix(in srgb, CanvasText 70%, Canvas);"
    " border-inline-start: 0.25em solid color-mix(in srgb, CanvasText 25%, Canvas); }\n"
    "table { border-collapse: collapse; }\n"
    "th, td { padding: 0.3em 0.8em; border: 1px solid color-mix(in srgb, CanvasText 20%, Canvas); }\n"
    "hr { border: none; border-block-start: 2px solid color-mix(in srgb, CanvasText 20%, Canvas); }\n"
    "img { max-width: 100%; }\n";

// An example of a spec file: its number from 1, the section that it is in, the extensions that its opening line names,
// its Markdown and the HTML that it expects.
struct Example {
    uint32_t number;
    std::string section;
    std::string extensions;
    std::string markdown;
    std::string html;
};

std::string withTabs(std::string text) {
    for (size_t at = text.find(kTabArrow); at != std::string::npos; at = text.find(kTabArrow, at + 1))
        text.replace(at, 3, "\t");
    return text;
}

// Returns the examples of the spec file at `path`. An example whose opening line names it disabled keeps its number and
// is left out.
std::vector<Example> examplesOf(const fs::path& path) {
    std::vector<Example> examples;
    std::istringstream in(readFile(path));
    std::string section;
    uint32_t number = 0;
    enum { Outside, InMarkdown, InHtml } state = Outside;
    Example example;
    for (std::string line; std::getline(in, line);) {
        if (state == Outside) {
            if (line.starts_with(kFence + " example")) {
                example = Example{};
                example.extensions = line.substr(kFence.size() + 8);
                state = InMarkdown;
            } else if (line.starts_with("#")) {
                size_t start = line.find_first_not_of("# ");
                section = start == std::string::npos ? std::string() : line.substr(start);
            }
            continue;
        }
        if (line == kFence) {
            ++number;
            if (example.extensions.find("disabled") == std::string::npos) {
                example.number = number;
                example.section = section;
                example.markdown = withTabs(example.markdown);
                example.html = withTabs(example.html);
                examples.push_back(std::move(example));
            }
            state = Outside;
        } else if (state == InMarkdown && line == ".") {
            state = InHtml;
        } else {
            (state == InMarkdown ? example.markdown : example.html) += line + "\n";
        }
    }
    return examples;
}

// Writes `text` to `path` unless the file holds it already, so that a listing does not touch files it wrote before.
void writeIfChanged(const fs::path& path, const std::string& text) {
    std::error_code error;
    if (fs::exists(path, error) && readFile(path) == text)
        return;
    fs::create_directories(path.parent_path(), error);
    std::ofstream(path, std::ios::binary) << text;
}

// Returns the examples of the spec files, each written to the cache under `suite` as a Markdown document against an
// HTML page of its expected HTML.
std::vector<Document> examples(const char* suite, const std::vector<fs::path>& files) {
    std::vector<Document> documents;
    fs::path cache = viewerCache() / "markdown" / suite;
    for (const fs::path& file : files) {
        std::string stem = file.stem().string();
        for (const Example& example : examplesOf(file)) {
            std::string name = stem + "-" + std::to_string(example.number);
            fs::path markdown = cache / (name + ".md");
            fs::path html = cache / (name + ".html");
            writeIfChanged(markdown, example.markdown);
            // The load parses the variant that matches the example's extensions (RFC 7763): CommonMark alone, GFM's
            // extensions, or the default extensions, which hold footnotes.
            std::string variant = "; variant=CommonMark";
            if (example.extensions.find("footnotes") != std::string::npos)
                variant.clear();
            else if (!example.extensions.empty())
                variant = "; variant=GFM";
            writeIfChanged(fs::path(markdown.string() + ".headers"), "Content-Type: text/markdown" + variant + "\n");
            writeIfChanged(html, "<!DOCTYPE html>\n<meta charset=\"utf-8\">\n<style>\n" + std::string(kReferenceStyle) +
                "</style>\n<body>\n" + example.html);
            std::string description = "Example " + std::to_string(example.number) + " of " + stem + ".txt";
            if (!example.extensions.empty())
                description += ", with" + example.extensions;
            Document d{stem + ":" + std::to_string(example.number), description, fileSource(markdown)};
            Reference expected{"expected", fileSource(html)};
            d.references.push_back(std::move(expected));
            d.references.push_back(Reference{"Browser", {}, fileUrl(html), {}, true});
            d.folders = {stem, example.section};
            documents.push_back(std::move(d));
        }
    }
    return documents;
}

} // namespace

std::vector<Collection> markdownCollections() {
    std::vector<Collection> collections;
    std::error_code error;
    fs::path commonmark = fs::path(kThirdParty) / "commonmark-spec" / "spec.txt";
    if (fs::exists(commonmark, error)) {
        collections.push_back(Collection{
            "CommonMark",
            800,
            600,
            [commonmark] {
                return examples("commonmark", {commonmark});
            },
        });
    }
    fs::path gfm = fs::path(kThirdParty) / "cmark-gfm" / "test";
    if (fs::exists(gfm / "spec.txt", error)) {
        collections.push_back(Collection{
            "GFM",
            800,
            600,
            [gfm] {
                return examples("gfm", {gfm / "spec.txt", gfm / "extensions.txt", gfm / "regression.txt"});
            },
        });
    }
    return collections;
}

} // namespace viewer
