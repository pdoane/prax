// Regular expressions: their syntax, their preference among matches, their groups, and their limits.

#include "pr/regex.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

extern "C" int prTestRegexCApi(void);

namespace pr_impl {

namespace {

// A compiled pattern, which a test expects to compile.
struct Compiled {
    PrRegex* regex = nullptr;

    explicit Compiled(const std::string& pattern) { REQUIRE(prRegexCreate(pattern, &regex) == PrStatus_Ok); }
    ~Compiled() { prRegexDestroy(regex); }

    bool search(const std::string& text) const { return prRegexSearch(regex, text, nullptr, 0); }
    bool match(const std::string& text) const { return prRegexMatch(regex, text, nullptr, 0); }

    // The groups of the first match of a search, or none.
    std::vector<PrRegexGroup> groups(const std::string& text) const {
        std::vector<PrRegexGroup> out(prRegexGroupCount(regex));
        if (!prRegexSearch(regex, text, out.data(), uint32_t(out.size())))
            out.clear();
        return out;
    }

    PrRegexGroup range(const std::string& text) const {
        std::vector<PrRegexGroup> found = groups(text);
        return found.empty() ? PrRegexGroup{kPrRegexGroupNone, kPrRegexGroupNone} : found[0];
    }
};

PrStatus statusOf(const std::string& pattern) {
    PrRegex* regex = nullptr;
    PrStatus status = prRegexCreate(pattern, &regex);
    CHECK((status == PrStatus_Ok) == (regex != nullptr));
    prRegexDestroy(regex);
    return status;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public header compiles as C") {
    CHECK(prTestRegexCApi() == 0);
}

TEST_CASE("a literal matches part of a text, and the whole of one") {
    Compiled re("abc");
    CHECK(re.search("abc"));
    CHECK(re.search("xxabcyy"));
    CHECK_FALSE(re.search("ab"));
    CHECK_FALSE(re.search("abd"));
    CHECK(re.match("abc"));
    CHECK_FALSE(re.match("abcd"));
    CHECK_FALSE(re.match("xabc"));
    Compiled empty("");
    CHECK(empty.search(""));
    CHECK(empty.search("anything"));
    CHECK(empty.match(""));
    CHECK_FALSE(empty.match("x"));
    CHECK(Compiled("ab").range("xxabxxab").start == 2);
    CHECK(Compiled("ab").range("xxabxxab").end == 4);
}

TEST_CASE("a dot matches any code point but a line feed") {
    Compiled re("a.c");
    CHECK(re.match("abc"));
    CHECK(re.match("a_c"));
    CHECK_FALSE(re.match("ac"));
    CHECK_FALSE(Compiled("a.b").search("a\nb"));
    // A code point of two bytes is one step.
    PrRegexGroup r = Compiled("a.b").range("a\xc3\xa9" "b");
    CHECK(r.start == 0);
    CHECK(r.end == 4);
}

TEST_CASE("classes take ranges, their negation, Perl classes, and a literal dash") {
    Compiled lower("[a-z]+");
    CHECK(lower.match("hello"));
    CHECK_FALSE(lower.match("Hello"));
    CHECK(lower.range("12abc34").start == 2);
    CHECK(lower.range("12abc34").end == 5);
    Compiled notDigit("[^0-9]");
    CHECK(notDigit.match("a"));
    CHECK_FALSE(notDigit.match("5"));
    CHECK(notDigit.match("\n"));
    Compiled identifier("[A-Za-z_][A-Za-z0-9_]*");
    CHECK(identifier.match("foo_bar9"));
    CHECK_FALSE(identifier.match("9foo"));
    CHECK(Compiled("[a-]").match("-"));
    CHECK(Compiled("[-a]").match("-"));
    CHECK_FALSE(Compiled("[a-]").match("b"));
    CHECK(Compiled("[\\d.]+").match("3.14"));
    CHECK_FALSE(Compiled("[\\d.]+").match("3,14"));
    CHECK(Compiled("[^\\d]").match("x"));
    CHECK_FALSE(Compiled("[^\\d]").match("7"));
    CHECK(Compiled("[]a]").match("]"));
    // A range of code points past ASCII.
    CHECK(Compiled("^[\xce\xb1-\xcf\x89]+$").match("\xce\xbb\xce\xbf\xce\xb3\xce\xbf\xcf\x82"));
}

TEST_CASE("alternatives and groups") {
    Compiled re("cat|dog|bird");
    CHECK(re.search("I have a dog"));
    CHECK(re.search("the bird flew"));
    CHECK_FALSE(re.search("a fish swims"));
    CHECK(Compiled("^(ab|a)b$").match("abb"));
    CHECK(Compiled("^(ab|a)b$").match("ab"));
    Compiled plain("(?:ab)+");
    CHECK(prRegexGroupCount(plain.regex) == 1);
    CHECK(plain.match("ababab"));
    CHECK_FALSE(plain.match("aba"));
    CHECK(Compiled("^(arguments|module|console|window|document)$").match("console"));
}

TEST_CASE("anchors match at the ends of the text") {
    CHECK(Compiled("^abc").search("abcdef"));
    CHECK_FALSE(Compiled("^abc").search("xabc"));
    CHECK(Compiled("abc$").search("xxabc"));
    CHECK_FALSE(Compiled("abc$").search("abcx"));
    CHECK_FALSE(Compiled("^abc$").search("abcd"));
}

TEST_CASE("quantifiers prefer more, or fewer when they ask") {
    CHECK(Compiled("ab*c").match("ac"));
    CHECK(Compiled("ab*c").match("abbbbc"));
    CHECK_FALSE(Compiled("ab+c").match("ac"));
    CHECK(Compiled("ab+c").match("abbc"));
    CHECK(Compiled("ab?c").match("ac"));
    CHECK_FALSE(Compiled("ab?c").match("abbc"));
    CHECK(Compiled("a.*c").range("axcyc").end == 5);
    CHECK(Compiled("a.*?c").range("axcyc").end == 3);
    PrRegexGroup run = Compiled("a+").range("baaa");
    CHECK(run.start == 1);
    CHECK(run.end == 4);
}

TEST_CASE("counted repetition, and a brace that begins no interval") {
    CHECK(Compiled("a{3}").match("aaa"));
    CHECK_FALSE(Compiled("a{3}").match("aa"));
    CHECK_FALSE(Compiled("a{3}").match("aaaa"));
    CHECK_FALSE(Compiled("a{2,}").match("a"));
    CHECK(Compiled("a{2,}").match("aaaaaa"));
    Compiled range("a{2,4}");
    CHECK_FALSE(range.match("a"));
    CHECK(range.match("aaaa"));
    CHECK_FALSE(range.match("aaaaa"));
    CHECK(range.range("aaaaa").end == 4);
    CHECK(Compiled("a{x}").match("a{x}"));
}

TEST_CASE("Perl classes and escapes") {
    CHECK(Compiled("\\d+").range("abc123def").start == 3);
    CHECK(Compiled("\\d+").range("abc123def").end == 6);
    CHECK(Compiled("\\D+").match("abc"));
    CHECK_FALSE(Compiled("\\D+").match("a1c"));
    CHECK(Compiled("\\w+").match("foo_Bar123"));
    CHECK_FALSE(Compiled("\\w+").match("foo bar"));
    CHECK(Compiled("\\W").match(" "));
    CHECK(Compiled("\\s+").match(" \t\n"));
    CHECK_FALSE(Compiled("\\S+").match("a b"));
    CHECK(Compiled("a\\.c").match("a.c"));
    CHECK_FALSE(Compiled("a\\.c").match("axc"));
    CHECK(Compiled("\\(\\)").match("()"));
    CHECK(Compiled("a\\\\b").match("a\\b"));
    CHECK(Compiled("a\\tb\\nc").match("a\tb\nc"));
}

TEST_CASE("groups record what they matched, and a group that took no part is none") {
    std::vector<PrRegexGroup> g = Compiled("(a)(b)(c)").groups("zzabc");
    REQUIRE(g.size() == 4);
    CHECK(g[0].start == 2);
    CHECK(g[0].end == 5);
    CHECK(g[1].start == 2);
    CHECK(g[3].start == 4);
    g = Compiled("(a)(b)?(c)").groups("ac");
    REQUIRE(g.size() == 4);
    CHECK(g[2].start == kPrRegexGroupNone);
    CHECK(g[3].start == 1);
    // A repeated group records its last repetition.
    g = Compiled("(ab)+").groups("abab");
    CHECK(g[1].start == 2);
    CHECK(g[1].end == 4);
    g = Compiled("((a+)(b+))").groups("aaabb");
    CHECK(g[2].end == 3);
    CHECK(g[3].start == 3);
}

TEST_CASE("patterns that do not parse, and patterns past the limits, fail") {
    for (const char* pattern : {"a(b", "a)b", "[abc", "*", "a**", "a{2,1}", "\\q", "(?P<x>a)", "[z-a]", "a\\"})
        CHECK_MESSAGE(statusOf(pattern) == PrStatus_Syntax, pattern);
    CHECK(statusOf("a{1001}") == PrStatus_Limit);
    CHECK(statusOf(std::string(300, '(') + std::string(300, ')')) == PrStatus_Limit);
    CHECK(statusOf("((a{1000}){1000}){2}") == PrStatus_Limit);
}

TEST_CASE("long patterns and texts compile and match without deep recursion") {
    std::string literal(200000, 'a');
    Compiled long_(literal);
    CHECK(long_.match(literal));
    std::string alternatives = "x0";
    for (uint32_t i = 1; i < 20000; ++i)
        alternatives += "|x" + std::to_string(i);
    Compiled many(alternatives);
    CHECK(many.match("x19999"));
    CHECK_FALSE(many.match("x20000"));
    std::string text(1u << 20, 'b');
    CHECK(Compiled("b*$").search(text));
    CHECK(Compiled("(?:b?){500}b{500}").match(std::string(500, 'b')));
}
