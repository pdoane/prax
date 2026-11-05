// WPT's reftests (suite_wpt.h), selected as tools/suite/wpt/wpt.py selects the reftests that it renders: a page that
// WPT's manifest treats as a reftest, whose document and references can each be rendered. An HTML or XHTML page is
// drawable unless it is for print, and an SVG document is drawable unless it is dynamic. Each is compared with its
// first reference.

#include "tools/viewer/suite_wpt.h"

#include "tools/viewer/source_html.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

namespace viewer {

namespace {

namespace fs = std::filesystem;

constexpr const char* kThirdParty = PR_THIRD_PARTY_DIR;

fs::path checkout() {
    return fs::path(kThirdParty) / "wpt";
}

// Returns the page's text, decoded by its byte order mark, or as Latin-1 if it has none, as wpt.py's page_text does.
// Every marker that the selection looks for is ASCII, so UTF-16 keeps only the low byte of each unit.
std::string pageText(const fs::path& path) {
    std::string bytes = readFile(path);
    if (bytes.starts_with("\xef\xbb\xbf"))
        return bytes.substr(3);
    bool little = bytes.starts_with("\xff\xfe");
    if (little || bytes.starts_with("\xfe\xff")) {
        std::string text;
        for (size_t i = 2 + (little ? 0 : 1); i < bytes.size(); i += 2)
            text += bytes[i];
        return text;
    }
    return bytes;
}

std::string folded(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z')
            c = char(c - 'A' + 'a');
    }
    return out;
}

bool wordChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

// A tag of the folded text: where it starts, its name without a namespace prefix, whether it had one, and its text up
// to the next `>`.
struct Tag {
    size_t at;
    std::string_view name;
    bool prefixed;
    std::string_view text;
};

// Calls `visit` with each tag of the folded text whose name is a word, as `<(?:\w+:)?name` reads it.
template <typename Visit>
void eachTag(std::string_view text, Visit visit) {
    for (size_t at = text.find('<'); at != std::string_view::npos; at = text.find('<', at + 1)) {
        size_t start = at + 1;
        size_t end = start;
        while (end < text.size() && wordChar(text[end]))
            ++end;
        bool prefixed = false;
        if (end < text.size() && text[end] == ':' && end > start) {
            prefixed = true;
            start = end + 1;
            end = start;
            while (end < text.size() && wordChar(text[end]))
                ++end;
        }
        if (end == start)
            continue;
        size_t close = text.find('>', end);
        std::string_view body = text.substr(at, close == std::string_view::npos ? std::string_view::npos : close - at);
        visit(Tag{at, text.substr(start, end - start), prefixed, body});
    }
}

// Returns the values of the tag's attribute `name`, written as `\bname\s*=\s*` and then a value. If `quoted` is true,
// only a value in quotes is read, as wpt.py's LINK_ATTRIBUTE reads it. Otherwise, a value may start with an optional
// quote, and runs to the next quote, space or `>`.
std::vector<std::string_view> attributeValues(std::string_view tag, std::string_view name, bool quoted) {
    std::vector<std::string_view> values;
    for (size_t at = tag.find(name); at != std::string_view::npos; at = tag.find(name, at + 1)) {
        if (at > 0 && wordChar(tag[at - 1]))
            continue;
        size_t i = at + name.size();
        while (i < tag.size() && space(tag[i]))
            ++i;
        if (i == tag.size() || tag[i] != '=')
            continue;
        ++i;
        while (i < tag.size() && space(tag[i]))
            ++i;
        char quote = i < tag.size() && (tag[i] == '"' || tag[i] == '\'') ? tag[i] : '\0';
        if (quote == '\0' && quoted)
            continue;
        size_t start = quote != '\0' ? i + 1 : i;
        size_t end = start;
        while (end < tag.size() && (quote != '\0' ? tag[end] != quote : !space(tag[end]) && tag[end] != '"' && tag[end] != '\''))
            ++end;
        if (quote != '\0' && end == tag.size())
            continue;
        values.push_back(tag.substr(start, end - start));
    }
    return values;
}

// Returns true if the page is a reftest according to WPT's manifest (wpt.py's kind_of): it has a link in the page's own
// namespace whose rel starts with match or mismatch.
bool declaresReference(std::string_view lower) {
    bool found = false;
    eachTag(lower, [&](const Tag& tag) {
        if (found || tag.prefixed || tag.name.substr(0, 4) != "link")
            return;
        for (std::string_view rel : attributeValues(tag.text, "rel", false)) {
            for (std::string_view kind : {std::string_view("match"), std::string_view("mismatch")}) {
                if (rel.substr(0, kind.size()) == kind && (rel.size() == kind.size() || !wordChar(rel[kind.size()])))
                    found = true;
            }
        }
    });
    return found;
}

// A reftest's reference, and whether the test must match it rather than differ from it.
struct Ref {
    fs::path path;
    bool match;
};

// Returns the reftest's references in order, as wpt.py's references reads them: the href of each link whose rel is
// match or mismatch, without its query or fragment, resolved against the checkout's root or the test's directory.
std::vector<Ref> references(const fs::path& test, const std::string& text, std::string_view lower) {
    std::vector<Ref> found;
    eachTag(lower, [&](const Tag& tag) {
        if (tag.name != "link")
            return;
        std::vector<std::string_view> rels = attributeValues(tag.text, "rel", true);
        std::vector<std::string_view> hrefs = attributeValues(tag.text, "href", true);
        if (rels.empty() || hrefs.empty() || (rels.back() != "match" && rels.back() != "mismatch"))
            return;
        // The href as written, which the folded text holds at the same place.
        size_t offset = size_t(hrefs.back().data() - lower.data());
        std::string href = text.substr(offset, hrefs.back().size());
        if (href.empty())
            return;
        href = href.substr(0, href.find_first_of("?#"));
        fs::path base = href.starts_with("/") ? checkout() : test.parent_path();
        std::string relative = href.substr(std::min(href.find_first_not_of('/'), href.size()));
        found.push_back(Ref{(base / relative).lexically_normal(), rels.back() == "match"});
    });
    return found;
}

bool captured(const fs::path& path) {
    std::string extension = path.extension().string();
    return extension == ".html" || extension == ".htm" || extension == ".xht" || extension == ".xhtml";
}

// Returns true if the SVG document is more than a still image, as reftest.py's DYNAMIC determines: it scripts,
// animates, handles events or plays media, or its reftest waits or prints.
bool dynamic(std::string_view lower) {
    if (lower.find("reftest-wait") != std::string_view::npos || lower.find("reftest-print") != std::string_view::npos)
        return true;
    bool found = false;
    eachTag(lower, [&](const Tag& tag) {
        for (std::string_view name : {"script", "handler", "listener", "set", "audio", "video", "animation", "discard"})
            found = found || tag.name == name;
        found = found || tag.name.starts_with("animate");
    });
    return found;
}

// Returns true if the document can be rendered: a page that the shell captures and that is not for print, or a static
// SVG document.
bool drawable(const fs::path& path, const std::string& text, std::string_view lower) {
    if (captured(path))
        return !path.stem().string().ends_with("-print") && text.find("reftest-print") == std::string::npos;
    return path.extension() == ".svg" && !dynamic(lower);
}

bool drawable(const fs::path& path) {
    std::string text = pageText(path);
    return drawable(path, text, folded(text));
}

// Returns true for the directories that hold no tests (WPT's manifest rules).
bool notTests(const std::string& name) {
    return name == "support" || name == "resources" || name == "tools" || name == "reference" || name.starts_with(".");
}

std::vector<Document> wpt() {
    fs::path root = checkout();
    std::vector<fs::path> pages;
    std::error_code error;
    for (fs::recursive_directory_iterator it(root, error), end; it != end; it.increment(error)) {
        if (error)
            break;
        if (it->is_directory()) {
            if (notTests(it->path().filename().string()))
                it.disable_recursion_pending();
            continue;
        }
        fs::path extension = it->path().extension();
        if (captured(it->path()) || extension == ".svg")
            pages.push_back(it->path());
    }
    std::sort(pages.begin(), pages.end());

    std::vector<Document> documents;
    for (const fs::path& test : pages) {
        std::string stem = test.stem().string();
        if (stem.ends_with("-ref") || stem.ends_with("-notref") || stem.starts_with("ref-") ||
                stem.starts_with("notref-") || stem.ends_with("-crash"))
            continue;
        fs::path relative = test.lexically_relative(root);
        bool crashtest = false;
        fs::path directory = relative.parent_path();
        for (const fs::path& part : directory)
            crashtest = crashtest || part == "crashtests";
        if (crashtest)
            continue;
        std::string text = pageText(test);
        if (text.find("/resources/testharness.js") != std::string::npos)
            continue;
        std::string lower = folded(text);
        if (!declaresReference(lower))
            continue;
        std::vector<Ref> refs = references(test, text, lower);
        if (refs.empty() || !drawable(test, text, lower))
            continue;
        bool all = true;
        for (const Ref& ref : refs)
            all = all && fs::exists(ref.path) && drawable(ref.path);
        if (!all)
            continue;
        Document d{
            relative.generic_string(),
            refs[0].match ? "WPT reftest, against its reference" : "WPT reftest, against the reference it must differ from",
            fileSource(test),
        };
        d.references.push_back(Reference{"Corpus", fileSource(refs[0].path)});
        documents.push_back(std::move(d));
    }
    return documents;
}

} // namespace

std::vector<Collection> wptCollections() {
    std::vector<Collection> collections;
    if (fs::exists(checkout()) && wptServed())
        collections.push_back(Collection{"WPT", 800, 600, wpt});
    return collections;
}

} // namespace viewer
