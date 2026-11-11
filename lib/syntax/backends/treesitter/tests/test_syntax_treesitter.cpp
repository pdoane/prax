// The tree-sitter languages: the spans of real grammars, their predicates, injections, and incremental updates that
// agree with a parse from scratch.

#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/syntax_treesitter.h"
#include "pr/syntax_treesitter_c.h"
#include "pr/syntax_treesitter_cpp.h"
#include "pr/syntax_treesitter_css.h"
#include "pr/syntax_treesitter_html.h"
#include "pr/syntax_treesitter_javascript.h"
#include "pr/syntax_treesitter_json.h"
#include "pr/syntax_treesitter_languages.h"
#include "pr/syntax_treesitter_markdown.h"
#include "pr/syntax_treesitter_markdown_inline.h"
#include "pr/syntax_treesitter_python.h"
#include "pr/syntax_treesitter_xml.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

extern "C" int prTestSyntaxTreeSitterCApi(void);

// The grammar of C, whose language a test makes again with queries of its own.
struct TSLanguage;
extern "C" const TSLanguage* tree_sitter_c(void);

namespace pr_impl {

namespace {

// Returns true if the build builds in `language`, as PR_SYNTAX_LANGUAGES names it. The other tests name only the
// languages that every build of the tests builds in, which a lean build names (cmake/lean.cmake).
bool built(const char* language) {
    return std::string(PR_SYNTAX_LANGUAGES_BUILT).find("," + std::string(language) + ",") != std::string::npos;
}

// A registry of the built-in languages, and the syntax tree of a buffer in one of them.
struct Parsed {
    PrSyntaxRegistry* registry = prSyntaxRegistryCreate();
    PrTextEditBuffer* buffer = nullptr;
    PrTextEditView* view = nullptr;
    PrSyntaxTree* tree = nullptr;

    Parsed(const PrSyntaxLanguage* language, const std::string& text) {
        prSyntaxTreeSitterAddLanguages(registry);
        buffer = prTextEditBufferCreate(text);
        view = prTextEditViewCreate(buffer);
        tree = prSyntaxTreeCreate(language, registry, buffer);
        prSyntaxTreeUpdate(tree, kPrF64Infinity);
    }

    ~Parsed() {
        prSyntaxTreeDestroy(tree);
        prTextEditViewDestroy(view);
        prTextEditBufferDestroy(buffer);
        prSyntaxRegistryDestroy(registry);
    }

    std::vector<PrSyntaxSpan> spans(uint32_t start, uint32_t end) {
        std::vector<PrSyntaxSpan> out(prSyntaxTreeSpans(tree, start, end, nullptr, 0));
        uint32_t written = prSyntaxTreeSpans(tree, start, end, out.data(), uint32_t(out.size()));
        CHECK(written == out.size());
        return out;
    }

    std::vector<PrSyntaxSpan> all() { return spans(0, prTextEditBufferSize(buffer)); }

    // Returns the name of the category of the first byte of `needle` in the text, or empty.
    std::string at(const std::string& needle) {
        PrStringView text = prTextEditBufferText(buffer);
        size_t found = std::string(text.data, text.size).find(needle);
        REQUIRE(found != std::string::npos);
        for (const PrSyntaxSpan& span : all()) {
            if (span.start <= found && found < span.end) {
                PrStringView name = prSyntaxRegistryCategoryName(registry, span.category);
                return std::string(name.data, name.size);
            }
        }
        return "";
    }

    void replace(uint32_t start, uint32_t end, const std::string& text) {
        PrTextEditChange change{.start = start, .end = end, .text = text};
        prTextEditBufferApply(buffer, &change, 1, PrTextEditOrigin_Local);
    }
};

bool sameSpans(const std::vector<PrSyntaxSpan>& a, const std::vector<PrSyntaxSpan>& b) {
    if (a.size() != b.size())
        return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i].start != b[i].start || a[i].end != b[i].end || a[i].category != b[i].category)
            return false;
    }
    return true;
}

const char kProgram[] = "#include <stdio.h>\n"
    "#define LIMIT 10\n"
    "// counts\n"
    "static int count(const char* text) {\n"
    "    int n = 0;\n"
    "    while (*text++ != 0)\n"
    "        n += 1;\n"
    "    return n > LIMIT ? LIMIT : n;\n"
    "}\n";

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public header compiles as C") {
    CHECK(prTestSyntaxTreeSitterCApi() == 0);
}

TEST_CASE("a C tree names the parts of a program, and its predicates hold") {
    Parsed h(prSyntaxTreeSitterC(), kProgram);
    CHECK(h.at("#include") == "keyword");
    CHECK(h.at("<stdio.h>") == "string");
    CHECK(h.at("// counts") == "comment");
    CHECK(h.at("static") == "keyword");
    CHECK(h.at("int count") == "type");
    CHECK(h.at("count(") == "function");
    CHECK(h.at("while") == "keyword");
    CHECK(h.at("0;") == "number");
    // #match? names an identifier in capitals a constant.
    CHECK(h.at("LIMIT ?") == "constant");
    CHECK(h.at("n > ") == "variable");
}

TEST_CASE("spans are in order, do not overlap, and the range of a call bounds what it reads") {
    Parsed h(prSyntaxTreeSitterC(), kProgram);
    std::vector<PrSyntaxSpan> all = h.all();
    REQUIRE(all.size() > 10);
    for (size_t i = 0; i < all.size(); ++i) {
        CHECK(all[i].start < all[i].end);
        if (i > 0)
            CHECK(all[i - 1].end <= all[i].start);
    }
    std::vector<PrSyntaxSpan> some = h.spans(70, 90);
    CHECK(!some.empty());
    CHECK(some.size() < all.size());
    for (const PrSyntaxSpan& span : some) {
        CHECK(span.end > 70);
        CHECK(span.start < 90);
    }
}

TEST_CASE("edits update the tree incrementally, and give the spans of a parse from scratch") {
    Parsed h(prSyntaxTreeSitterC(), kProgram);
    const char* insertions[] = {"x", " ", "\"", "/*", "*/", "{", "}", "\n", "int ", "LIMIT", "'a'", ";"};
    uint32_t seed = 12345;
    for (uint32_t step = 0; step < 300; ++step) {
        seed = seed * 1103515245u + 12345u;
        uint32_t size = prTextEditBufferSize(h.buffer);
        uint32_t at = (seed >> 8) % (size + 1);
        uint32_t removed = (seed >> 4) % 4;
        removed = at + removed <= size ? removed : size - at;
        const char* text = insertions[(seed >> 16) % countOf(insertions)];
        h.replace(at, at + removed, (seed & 1) != 0 ? text : "");
        if (step % 7 == 0 || step == 299) {
            prSyntaxTreeUpdate(h.tree, kPrF64Infinity);
            PrStringView now = prTextEditBufferText(h.buffer);
            Parsed fresh(prSyntaxTreeSitterC(), std::string(now.data, now.size));
            // Both registries were made the same way, but a category's number depends on the order of first use, so
            // spans compare by name.
            std::vector<PrSyntaxSpan> kept = h.all();
            std::vector<PrSyntaxSpan> scratch = fresh.all();
            for (PrSyntaxSpan& span : scratch) {
                PrStringView name = prSyntaxRegistryCategoryName(fresh.registry, span.category);
                span.category = prSyntaxRegistryCategoryIntern(h.registry, name);
            }
            REQUIRE_MESSAGE(sameSpans(kept, scratch), "after step " << step);
        }
    }
}

TEST_CASE("an HTML tree hands its scripts and styles to their languages, within the range asked for") {
    std::string page = "<!doctype html>\n<html><head>\n<style>\nbody { color: red; }\n</style>\n</head><body>\n"
        "<p class=\"x\">hello</p>\n<script>\nconst value = 42;\nfunction run() { return value; }\n</script>\n"
        "</body></html>\n";
    Parsed h(prSyntaxTreeSitterHtml(), page);
    CHECK(h.at("<p") == "punctuation.bracket");
    CHECK(h.at("p class") == "tag");
    CHECK(h.at("class=") == "attribute");
    CHECK(h.at("x\">") == "string");
    CHECK(h.at("const") == "keyword");
    CHECK(h.at("42") == "number");
    CHECK(h.at("run()") == "function");
    CHECK(h.at("color:") == "property");
    CHECK(h.at("body {") == "tag");
    // A range that holds only the paragraph injects nothing.
    size_t paragraph = page.find("<p ");
    std::vector<PrSyntaxSpan> some = h.spans(uint32_t(paragraph), uint32_t(paragraph + 10));
    for (const PrSyntaxSpan& span : some)
        CHECK(span.start < paragraph + 10);
}

TEST_CASE("C++ joins C's highlights query with its own, and JSON and CSS name their parts") {
    Parsed cpp(prSyntaxTreeSitterCpp(), "namespace a { class Point { public: int x; }; }\n");
    CHECK(cpp.at("namespace") == "keyword");
    CHECK(cpp.at("class") == "keyword");
    CHECK(cpp.at("int x") == "type");
    Parsed json(prSyntaxTreeSitterJson(), "{\"key\": [1, true, null]}");
    CHECK(json.at("1,") == "number");
    CHECK(json.at("true") == "constant.builtin");
    Parsed css(prSyntaxTreeSitterCss(), ":root { --gap: 4px; }\n");
    CHECK(css.at("--gap") == "property");
    CHECK(css.at("4px") == "number");
}

TEST_CASE("a parse that runs out of time resumes at the next update, and the spans wait for it to finish") {
    std::string program;
    while (program.size() < (2u << 20))
        program += kProgram;
    PrSyntaxRegistry* registry = prSyntaxRegistryCreate();
    PrTextEditBuffer* buffer = prTextEditBufferCreate(program);
    PrSyntaxTree* tree = prSyntaxTreeCreate(prSyntaxTreeSitterC(), registry, buffer);
    CHECK_FALSE(prSyntaxTreeUpdate(tree, 0.5));
    CHECK(prSyntaxTreeIsPending(tree));
    CHECK(prSyntaxTreeSpans(tree, 0, 100, nullptr, 0) == 0);
    uint32_t updates = 1;
    bool finished = false;
    while (!finished && updates < 100000) {
        finished = prSyntaxTreeUpdate(tree, 0.5);
        ++updates;
    }
    REQUIRE(finished);
    CHECK(updates > 2);
    CHECK_FALSE(prSyntaxTreeIsPending(tree));
    PrSyntaxSpan first{};
    REQUIRE(prSyntaxTreeSpans(tree, 0, 30, &first, 1) >= 1);
    CHECK(prSyntaxRegistryCategoryName(registry, first.category) == "keyword");
    prSyntaxTreeDestroy(tree);
    prTextEditBufferDestroy(buffer);
    prSyntaxRegistryDestroy(registry);
}

TEST_CASE("an edit while a parse is halted starts the parse again from the edited text") {
    std::string program;
    while (program.size() < (2u << 20))
        program += kProgram;
    PrSyntaxRegistry* registry = prSyntaxRegistryCreate();
    PrTextEditBuffer* buffer = prTextEditBufferCreate(program);
    PrSyntaxTree* tree = prSyntaxTreeCreate(prSyntaxTreeSitterC(), registry, buffer);
    CHECK_FALSE(prSyntaxTreeUpdate(tree, 0.5));
    REQUIRE(prSyntaxTreeIsPending(tree));
    // The program's first line becomes a comment.
    PrTextEditChange change{.start = 0, .end = 0, .text = "/* "};
    prTextEditBufferApply(buffer, &change, 1, PrTextEditOrigin_Local);
    change = PrTextEditChange{.start = 21, .end = 21, .text = " */"};
    prTextEditBufferApply(buffer, &change, 1, PrTextEditOrigin_Local);
    CHECK(prSyntaxTreeUpdate(tree, kPrF64Infinity));
    CHECK_FALSE(prSyntaxTreeIsPending(tree));
    PrSyntaxSpan first{};
    REQUIRE(prSyntaxTreeSpans(tree, 0, 30, &first, 1) >= 1);
    CHECK(prSyntaxRegistryCategoryName(registry, first.category) == "comment");
    CHECK(first.start == 0);
    CHECK(first.end == 24);
    prSyntaxTreeDestroy(tree);
    prTextEditBufferDestroy(buffer);
    prSyntaxRegistryDestroy(registry);
}

TEST_CASE("text appended in pieces parses to the tree of the whole") {
    std::string program;
    for (uint32_t i = 0; i < 40; ++i)
        program += kProgram;
    Parsed whole(prSyntaxTreeSitterC(), program);
    Parsed pieces(prSyntaxTreeSitterC(), "");
    for (size_t at = 0; at < program.size(); at += 97) {
        std::string piece = program.substr(at, 97);
        uint32_t size = prTextEditBufferSize(pieces.buffer);
        pieces.replace(size, size, piece);
        prSyntaxTreeUpdate(pieces.tree, kPrF64Infinity);
    }
    std::vector<PrSyntaxSpan> a = whole.all();
    std::vector<PrSyntaxSpan> b = pieces.all();
    for (PrSyntaxSpan& span : b) {
        PrStringView name = prSyntaxRegistryCategoryName(pieces.registry, span.category);
        span.category = prSyntaxRegistryCategoryIntern(whole.registry, name);
    }
    CHECK(sameSpans(a, b));
}

TEST_CASE("a registry finds a language by the other names of its pattern, in any case") {
    PrSyntaxRegistry* registry = prSyntaxRegistryCreate();
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterJavaScript());
    prSyntaxRegistryAdd(registry, prSyntaxTreeSitterCpp());
    CHECK(prSyntaxRegistryFind(registry, "js") == prSyntaxTreeSitterJavaScript());
    CHECK(prSyntaxRegistryFind(registry, "JavaScript") == prSyntaxTreeSitterJavaScript());
    CHECK(prSyntaxRegistryFind(registry, "c++") == prSyntaxTreeSitterCpp());
    CHECK(prSyntaxRegistryFind(registry, "jsx") == nullptr);
    CHECK(prSyntaxRegistryFind(registry, std::string(100, 'j')) == nullptr);
    prSyntaxRegistryDestroy(registry);
}

TEST_CASE("Markdown injects its inline text, the languages of its fences, and HTML, whose scripts inject in turn") {
    std::string page = "# Title with `code`\n\n"
        "Some *emphasis* and a [link](https://example.com).\n\n"
        "```js\nconst answer = 42;\n```\n\n"
        "<div>\n<script>\nlet nested = 7;\n</script>\n</div>\n\n"
        "```python\ndef run():\n    return None\n```\n";
    Parsed h(prSyntaxTreeSitterMarkdown(), page);
    CHECK(h.at("# Title") == "punctuation.special");
    CHECK(h.at("Title with") == "text.title");
    CHECK(h.at("*emphasis*") == "punctuation.delimiter");
    CHECK(h.at("emphasis* and") == "text.emphasis");
    CHECK(h.at("https://example.com") == "text.uri");
    CHECK(h.at("const") == "keyword");
    CHECK(h.at("42") == "number");
    // An HTML block injects HTML, whose script injects JavaScript, three levels down.
    CHECK(h.at("div>") == "tag");
    CHECK(h.at("let nested") == "keyword");
    CHECK(h.at("7;") == "number");
    CHECK(h.at("def run") == "keyword");
}

TEST_CASE("an injection leaves out the children of its content, as a block quote's markers") {
    std::string quote = "> a quoted *line*\n> and `another` line\n";
    Parsed h(prSyntaxTreeSitterMarkdown(), quote);
    CHECK(h.at("> and") == "punctuation.special");
    CHECK(h.at("*line*") == "punctuation.delimiter");
    CHECK(h.at("`another`") == "punctuation.delimiter");
    CHECK(h.at("another`") == "text.literal");
}

TEST_CASE("a combined injection parses all of its matches as one document") {
    // JavaScript's tagged templates inject the language that the tag names, across the template's parts.
    std::string program = "const page = html`<p class=\"a\">${name}</p>`;\n";
    Parsed h(prSyntaxTreeSitterJavaScript(), program);
    CHECK(h.at("p class") == "tag");
    CHECK(h.at("class=") == "attribute");
    CHECK(h.at("p>`") == "tag");
}

TEST_CASE("Python and XML name their parts") {
    Parsed python(prSyntaxTreeSitterPython(), "def area(radius):\n    return 3.14 * radius ** 2  # comment\n");
    CHECK(python.at("def") == "keyword");
    CHECK(python.at("area") == "function");
    CHECK(python.at("3.14") == "number");
    CHECK(python.at("# comment") == "comment");
    Parsed xml(prSyntaxTreeSitterXml(), "<?xml version=\"1.0\"?>\n<root kind=\"x\"><!-- note --><item>text</item></root>\n");
    CHECK(xml.at("root kind") == "tag");
    CHECK(xml.at("kind=") == "property");
    CHECK(xml.at("<!-- note") == "comment");
}

TEST_CASE("each built-in language is found by its file, and its queries name a part of a line") {
    struct Case {
        const char* language;   // the language, as PR_SYNTAX_LANGUAGES names it
        const char* path;
        const char* text;
        const char* needle;
        const char* category;
    };
    const Case cases[] = {
        {"bash", "run.sh", "if [ -n \"$1\" ]; then echo hi; fi\n", "if", "keyword"},
        {"c", "main.c", "int main(void) { return 0; }\n", "return", "keyword"},
        {"c_sharp", "Program.cs", "class Point { int x; }\n", "class", "keyword"},
        {"cmake", "CMakeLists.txt", "add_library(core STATIC a.c)\n", "add_library", "function.builtin"},
        {"cpp", "point.h", "namespace a { class Point {}; }\n", "namespace", "keyword"},
        {"css", "page.css", "a { color: red; }\n", "color", "property"},
        {"dart", "main.dart", "class Point { final int x = 0; }\n", "class", "keyword"},
        {"diff", "fix.diff", "--- a/x\n+++ b/x\n@@ -1 +1 @@\n-old\n+new\n", "new", "diff.plus"},
        {"dockerfile", "Dockerfile", "FROM alpine:3\nRUN echo hi\n", "FROM", "keyword"},
        {"glsl", "blur.frag", "uniform vec4 color;\nvoid main() {}\n", "uniform", "type.qualifier"},
        {"go", "main.go", "package main\nfunc main() {}\n", "func", "keyword"},
        {"hcl", "site.hcl", "block \"a\" { count = 1 }\n", "1", "number"},
        {"hlsl", "blur.hlsl", "cbuffer Frame : register(b0) { float4 color; };\n", "cbuffer", "type"},
        {"html", "page.html", "<p class=\"a\">text</p>\n", "p class", "tag"},
        {"java", "Point.java", "public class Point { int x; }\n", "public", "keyword"},
        {"javascript", "app.js", "const answer = 42;\n", "const", "keyword"},
        {"json", "data.json", "{\"key\": true}\n", "true", "constant.builtin"},
        {"kotlin", "Main.kt", "fun main() { val x = 1 }\n", "fun", "keyword.function"},
        {"lua", "init.lua", "local x = 1\n", "local", "keyword"},
        {"make", "Makefile", "# build\nall: main.o\n", "# build", "comment"},
        {"markdown", "README.md", "# Title\n", "Title", "text.title"},
        {"objc", "view.m", "@interface View : NSObject\n@end\n", "@interface", "keyword"},
        {"php", "index.php", "<?php echo $name; ?>\n", "name;", "variable"},
        {"powershell", "build.ps1", "function Get-Thing { param($Name) }\n", "function", "keyword"},
        {"python", "area.py", "def area(radius):\n    return radius\n", "def", "keyword"},
        {"ruby", "point.rb", "class Point\nend\n", "class", "keyword"},
        {"rust", "main.rs", "fn main() { let x = 1; }\n", "fn", "keyword"},
        {"slang", "blur.slang", "struct Frame { float4 color; };\n", "struct", "keyword.type"},
        {"sql", "query.sql", "SELECT id FROM users;\n", "SELECT", "keyword"},
        {"swift", "main.swift", "func area() -> Int { return 1 }\n", "func", "keyword.function"},
        {"terraform", "main.tf", "resource \"aws_s3_bucket\" \"b\" { count = 1 }\n", "1", "number"},
        {"toml", "Cargo.toml", "[package]\nname = \"a\"\n", "name", "type"},
        {"tsx", "view.tsx", "const v = <div>{x}</div>;\n", "div", "tag"},
        {"typescript", "main.ts", "let x: number = 1;\n", "number", "type.builtin"},
        {"wgsl", "blur.wgsl", "fn main() -> f32 { return 1.0; }\n", "fn", "keyword.function"},
        {"xml", "feed.xml", "<root kind=\"x\"/>\n", "root", "tag"},
        {"yaml", "config.yaml", "name: a\n", "name", "property"},
    };
    PrSyntaxRegistry* registry = prSyntaxRegistryCreate();
    prSyntaxTreeSitterAddLanguages(registry);
    for (const Case& c : cases) {
        if (!built(c.language))
            continue;
        std::string path = c.path;
        CAPTURE(path);
        const PrSyntaxLanguage* language = prSyntaxRegistryFromPath(registry, c.path);
        REQUIRE(language != nullptr);
        Parsed h(language, c.text);
        CHECK(h.at(c.needle) == std::string(c.category));
    }
    prSyntaxRegistryDestroy(registry);
}

TEST_CASE("Neovim's predicates test a capture's text with Lua's patterns, and its node's parent and ancestors") {
    const char kHighlights[] = "(identifier) @variable\n"
        "((identifier) @constant (#lua-match? @constant \"^[A-Z][A-Z_%d]*$\"))\n"
        "((identifier) @parameter (#has-parent? @parameter parameter_declaration))\n"
        "((identifier) @returned (#has-ancestor? @returned return_statement) (#not-lua-match? @returned \"^%u\"))\n";
    PrSyntaxTreeSitterDesc desc{.name = "c", .grammar = tree_sitter_c(), .highlights = kHighlights};
    const PrSyntaxLanguage* language = prSyntaxTreeSitterLanguageCreate(&desc);
    Parsed h(language, "int f(int count) { return LIMIT_2 + count; }\n");
    CHECK(h.at("f(") == "variable");
    CHECK(h.at("count)") == "parameter");
    CHECK(h.at("LIMIT_2") == "constant");
    CHECK(h.at("count;") == "returned");
    prSyntaxTreeSitterLanguageDestroy(language);
}
