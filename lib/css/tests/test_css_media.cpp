#include "css/src/css_media.h"
#include "css/src/css_parser.h"
#include "css/tests/css_tests.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// A media query list held while a test runs.
struct TestList {
    CssMediaQueries list = {};

    explicit TestList(const char* text) {
        cssMediaListParse(&list, text);
    }
    ~TestList() { cssValueListReset(&list.queries); }

    TestList(const TestList&) = delete;
    TestList& operator=(const TestList&) = delete;

    std::string text() const {
        PrTextBuffer out = {};
        cssSerializeMediaList(&out, &list);
        return take(&out);
    }
};

std::string serialized(const char* text) {
    return TestList(text).text();
}

// A desktop screen without scripting, with the features that `set` names.
struct TestEnvironment {
    CssMediaEnvironment media;

    TestEnvironment() { cssMediaEnvironmentInit(&media, false); }

    bool set(const char* name, const char* value) {
        PrCssDescriptor feature = cssDescriptorFromText(CssAtRule_Media, name);
        return cssMediaEnvironmentSet(&media, feature, value);
    }

    bool matches(const char* text) const {
        TestList list(text);
        return cssMediaListMatches(&list.list, &media);
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a change to a media list that writes what it holds leaves it unchanged") {
    TestList list("screen, print");
    auto prepared = [&](CssEditResult (*prepare)(CssEdit*, const CssMediaQueries*, PrStringView), const char* text) {
        CssEdit edit;
        cssEditBegin(&edit, nullptr);
        CssEditResult result = prepare(&edit, &list.list, text);
        if (result == CssEditResult_Changed)
            cssMediaListCommit(&edit, &list.list);
        cssEditEnd(&edit);
        return result;
    };
    CHECK(prepared(cssMediaListPrepareText, "screen,print") == CssEditResult_Unchanged);
    CHECK(prepared(cssMediaListPrepareAppend, "print") == CssEditResult_Unchanged);
    CHECK(prepared(cssMediaListPrepareAppend, "a, b") == CssEditResult_Unchanged);
    CHECK(prepared(cssMediaListPrepareDelete, "tv") == CssEditResult_Rejected);
    CHECK(prepared(cssMediaListPrepareAppend, "tv") == CssEditResult_Changed);
    CHECK(prepared(cssMediaListPrepareDelete, "screen") == CssEditResult_Changed);
    CHECK(list.text() == "print, tv");
}

TEST_CASE("a media feature's value is held as its grammar matches it") {
    CHECK(serialized("(resolution: calc(2x))") == "(resolution: calc(2dppx))");
    CHECK(serialized("(width >= calc(1px + 2em))") == "(width >= calc(2em + 1px))");
    CHECK(serialized("(aspect-ratio: 16/9)") == "(aspect-ratio: 16 / 9)");
    CHECK(serialized("(MIN-WIDTH: 5PX) AND (orientation: PORTRAIT)") == "(min-width: 5px) and (orientation: portrait)");
    CHECK(serialized("(400px < width <= 1000px)") == "(400px < width <= 1000px)");
    // An unknown feature or value makes the query unknown, and the query is kept as written (csswg-drafts#7595).
    CHECK(serialized("(width: portrait)") == "(width: portrait)");
    CHECK(serialized("(orientation: 1px)") == "(orientation: 1px)");
    CHECK(serialized("(min-orientation: portrait)") == "(min-orientation: portrait)");
    CHECK(serialized("(orientation > portrait)") == "(orientation > portrait)");
    CHECK(serialized("(max-weight: 3kg) and (color), (color)") == "(max-weight: 3kg) and (color), (color)");
    // A <dashed-ident> keeps its case.
    CHECK(serialized("(--FOO: bar)") == "(--FOO: bar)");
}

TEST_CASE("a media query list splits at every comma outside a block") {
    CHECK(serialized("color)") == "not all");
    CHECK(serialized("  color ), ( color") == "not all, (color)");
    CHECK(serialized(" ( color  ") == "(color)");
    CHECK(serialized(",") == "not all, not all");
    CHECK(serialized("(a, b), all") == "(a, b), all");
}

TEST_CASE("the media type matches all and the environment's type") {
    TestEnvironment environment;
    CHECK(environment.matches(""));
    CHECK(environment.matches("all"));
    CHECK(environment.matches("screen"));
    CHECK(environment.matches("only screen and (color)"));
    CHECK(!environment.matches("print"));
    CHECK(!environment.matches("tv"));
    CHECK(!environment.matches("not all"));
    CHECK(environment.matches("not print"));
    CHECK(environment.matches("print, screen"));
    cssMediaEnvironmentSetType(&environment.media, PrCssMediaType_Print);
    CHECK(environment.matches("print"));
    CHECK(!environment.matches("screen"));
}

TEST_CASE("a range feature compares its value") {
    TestEnvironment environment;
    CHECK(environment.matches("(width: 800px)"));
    CHECK(environment.matches("(min-width: 800px) and (max-height: 600px)"));
    CHECK(!environment.matches("(min-width: 801px)"));
    CHECK(environment.matches("(width > 799.5px)"));
    CHECK(!environment.matches("(width < 800px)"));
    CHECK(environment.matches("(400px < width <= 800px)"));
    CHECK(!environment.matches("(400px < width < 800px)"));
    CHECK(environment.matches("(1000px > width >= 800px)"));
    CHECK(environment.matches("(800px = width)"));
    CHECK(environment.matches("(width = 50em)"));
    CHECK(environment.matches("(width: calc(100vw))"));
    CHECK(environment.matches("(width < calc(50em + 1px))"));
    CHECK(environment.matches("(aspect-ratio: 4/3)"));
    CHECK(environment.matches("(min-aspect-ratio: 1)"));
    CHECK(environment.matches("(device-aspect-ratio: 800/600)"));
    CHECK(environment.matches("(resolution: 96dpi)"));
    CHECK(environment.matches("(resolution < infinite)"));
    CHECK(environment.matches("(-webkit-device-pixel-ratio: 1)"));
    CHECK(environment.matches("(color: 8)"));
    CHECK(!environment.matches("(min-color-index: 1)"));
}

TEST_CASE("a discrete feature compares equal, and is true in a boolean context unless it is none") {
    TestEnvironment environment;
    CHECK(environment.matches("(orientation: landscape)"));
    CHECK(!environment.matches("(orientation: portrait)"));
    CHECK(environment.matches("(hover: hover) and (pointer: fine)"));
    CHECK(!environment.matches("(pointer: coarse)"));
    CHECK(environment.matches("(hover)"));
    CHECK(environment.matches("(color)"));
    CHECK(!environment.matches("(monochrome)"));
    CHECK(!environment.matches("(grid)"));
    CHECK(!environment.matches("(scripting)"));
    CHECK(!environment.matches("(inverted-colors)"));
    // mediaqueries-5/#prefers-reduced-motion: no-preference evaluates as false.
    CHECK(!environment.matches("(prefers-reduced-motion)"));
    CHECK(environment.matches("(prefers-color-scheme)"));
    CHECK(environment.matches("(dynamic-range)"));
}

TEST_CASE("a condition is three-valued") {
    TestEnvironment environment;
    CHECK(environment.matches("not (monochrome)"));
    CHECK(environment.matches("(monochrome) or (color)"));
    CHECK(!environment.matches("(monochrome) and (color)"));
    CHECK(environment.matches("((color) and (hover)) or (grid)"));
    CHECK(environment.matches("screen and (not (grid))"));
    CHECK(!environment.matches("not screen and (color)"));
    // A cap height for which the environment gives no px value is unknown, and so is its negation.
    CHECK(!environment.matches("(width > 1cap)"));
    CHECK(!environment.matches("not (width > 1cap)"));
    CHECK(environment.matches("(width > 1cap) or (color)"));
    // So is an unknown feature, and a value that a feature does not accept.
    CHECK(!environment.matches("(width: portrait)"));
    CHECK(!environment.matches("not (nonesuch)"));
    CHECK(environment.matches("(nonesuch) or (color)"));
}

TEST_CASE("the environment's features are set as a query writes them") {
    TestEnvironment environment;
    CHECK(environment.set("width", "1024px"));
    CHECK(environment.matches("(min-width: 1000px)"));
    CHECK(environment.set("height", "2000px"));
    CHECK(environment.matches("(orientation: portrait)"));
    CHECK(environment.set("PREFERS-COLOR-SCHEME", " dark "));
    CHECK(environment.matches("(prefers-color-scheme: dark)"));
    CHECK(environment.set("resolution", "infinite"));
    CHECK(environment.matches("(min-resolution: 1000dppx)"));
    CHECK(environment.set("device-width", "50em"));
    CHECK(environment.matches("(device-width: 800px)"));
    CHECK_FALSE(environment.set("width", "dark"));
    CHECK(environment.matches("(min-width: 1000px)"));
}

TEST_CASE("a media query list is its query and its environment") {
    CssMediaEnvironment media;
    cssMediaEnvironmentInit(&media, true);
    PrCssMediaQueryList* list = cssMediaQueryListCreate(&media, " (SCRIPTING: enabled) , print");
    PrTextBuffer out = {};
    cssSerializeMediaList(&out, &list->media);
    CHECK(take(&out) == "(scripting: enabled), print");
    CHECK(cssMediaListMatches(&list->media, list->environment));
    CHECK(cssMediaEnvironmentSet(&media, PrCssDescriptor_MediaScripting, "none"));
    CHECK(!cssMediaListMatches(&list->media, list->environment));
    cssMediaQueryListRelease(list);
}

TEST_CASE("a media rule matches only in a document") {
    CssTestSheet sheet("@media all {}");
    CHECK(!prCssMediaRuleMatches(prCssMediaRuleQuery((sheet.object(0)))));
}
