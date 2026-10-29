#include "css/tests/css_tests.h"

#include "doctest/doctest.h"

namespace pr_impl {

namespace {

// Returns the value of the token at `index`, with escapes resolved.
std::string value(const CssTestSource& tokens, uint32_t index) {
    PrStringView text = cssTokenValueText(&tokens.tokens, index);
    std::string out(cssValue(text, nullptr, 0), '\0');
    cssValue(text, out.data(), uint32_t(out.size()));
    return out;
}

// ---------------------------------------------------------------------------------------------------------------------
// Blocks

// Returns the tokens of `text` alone, and then after a run of ident bytes and a semicolon, for every run length from 64
// to 128 bytes. The semicolon therefore ends a run at every offset within a word. A semicolon is its own token whatever
// follows it, so all the tokenizations agree from there on.
std::vector<std::string> tokensAfterRuns(const std::string& text) {
    CssTestSource plain{text};
    std::vector<std::string> got = plain.all();

    for (uint32_t pad = 0; pad <= 64; ++pad) {
        std::string padding(64 + pad, 'a');
        std::string input = padding + ";" + text;
        CssTestSource shifted{input};
        std::vector<std::string> both = shifted.all();
        REQUIRE(both.size() == got.size() + 2);
        CHECK(both[0] == padding);
        CHECK(both[1] == ";");
        CHECK(std::vector<std::string>(both.begin() + 2, both.end()) == got);
    }
    return got;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an empty input has no tokens") {
    CssTestSource tokens{""};
    CHECK(tokens.count() == 0);
    CHECK(tokens.kind(0) == CssTokenKind_Eof);
}

TEST_CASE("a declaration is its tokens") {
    CssTestSource tokens{"color: red;"};
    CHECK(tokens.all() == std::vector<std::string>{"color", ":", " ", "red", ";"});
    CHECK(tokens.kinds()
          == std::vector<CssTokenKind>{CssTokenKind_Ident, CssTokenKind_Colon,
                                       CssTokenKind_Whitespace, CssTokenKind_Ident,
                                       CssTokenKind_Semicolon});
}

TEST_CASE("a comment is not a token and the token after it is not contiguous") {
    CssTestSource tokens{"a/*x*/b"};
    CHECK(tokens.all() == std::vector<std::string>{"a", "b"});
    CHECK((tokens.flags(0) & CssTokenFlag_Contiguous) != 0);
    CHECK((tokens.flags(1) & CssTokenFlag_Contiguous) == 0);
}

TEST_CASE("a numeric token covers the bytes it read") {
    CHECK(CssTestSource{"3.5px"}.all() == std::vector<std::string>{"3.5px"});
    CHECK(CssTestSource{"3.x"}.all() == std::vector<std::string>{"3", ".", "x"});
    CHECK(CssTestSource{"1e+5"}.all() == std::vector<std::string>{"1e+5"});
    CHECK(CssTestSource{".5"}.all() == std::vector<std::string>{".5"});
    CHECK(CssTestSource{"+1"}.all() == std::vector<std::string>{"+1"});
    CHECK(CssTestSource{"50%"}.all() == std::vector<std::string>{"50%"});
    CHECK(CssTestSource{"1-2"}.all() == std::vector<std::string>{"1", "-2"});
    CHECK(CssTestSource{"1px+2px"}.all() == std::vector<std::string>{"1px", "+2px"});
}

TEST_CASE("a numeric token records how it was written") {
    CssTestSource tokens{"1 1.5 -1 1e5"};
    CHECK((tokens.flags(0) & CssTokenFlag_Integer) != 0);
    CHECK((tokens.flags(2) & CssTokenFlag_Integer) == 0);
    CHECK((tokens.flags(4) & CssTokenFlag_SignPresent) != 0);
    CHECK((tokens.flags(6) & CssTokenFlag_Integer) == 0);
    CHECK(cssTokenNumber(&tokens.tokens, 6) == 100000.0);
}

TEST_CASE("a dimension keeps its unit in its bytes") {
    CssTestSource tokens{"2rem"};
    REQUIRE(tokens.count() == 1);
    CHECK(tokens.kind(0) == CssTokenKind_Dimension);
    CHECK(cssTokenNumber(&tokens.tokens, 0) == 2.0);
    CHECK(cssTokenUnit(&tokens.tokens, 0) == PrCssUnit_Rem);
}

TEST_CASE("an ident followed by a paren is a function") {
    CssTestSource tokens{"rgb(1)"};
    CHECK(tokens.all() == std::vector<std::string>{"rgb(", "1", ")"});
    CHECK(tokens.kind(0) == CssTokenKind_Function);
}

TEST_CASE("an unquoted url is one token and a quoted one is a function") {
    CHECK(CssTestSource{"url(x)"}.kinds() == std::vector<CssTokenKind>{CssTokenKind_Url});
    CHECK(CssTestSource{"url( x )"}.all() == std::vector<std::string>{"url( x )"});
    CHECK(CssTestSource{"url(\"x\")"}.kinds()
          == std::vector<CssTokenKind>{CssTokenKind_Function, CssTokenKind_String,
                                       CssTokenKind_CloseParen});
    CHECK(CssTestSource{"url(a\"b)c"}.all() == std::vector<std::string>{"url(a\"b)", "c"});
    CHECK(CssTestSource{"url(a\"b)c"}.kind(0) == CssTokenKind_BadUrl);
}

TEST_CASE("a hash is an id when its name could open an ident") {
    CssTestSource tokens{"#abc #1a"};
    CHECK(tokens.kind(0) == CssTokenKind_Hash);
    CHECK((tokens.flags(0) & CssTokenFlag_Id) != 0);
    CHECK((tokens.flags(2) & CssTokenFlag_Id) == 0);
    CHECK(tokens.text(2) == "#1a");
}

TEST_CASE("an at-keyword, a CDO and a CDC") {
    CHECK(CssTestSource{"@media"}.kinds() == std::vector<CssTokenKind>{CssTokenKind_AtKeyword});
    CHECK(CssTestSource{"@ x"}.kind(0) == CssTokenKind_Delim);
    CHECK(CssTestSource{"<!--"}.kinds() == std::vector<CssTokenKind>{CssTokenKind_Cdo});
    CHECK(CssTestSource{"-->"}.kinds() == std::vector<CssTokenKind>{CssTokenKind_Cdc});
}

TEST_CASE("a string that meets a newline is a bad string") {
    CssTestSource tokens{"\"ab\nc"};
    CHECK(tokens.kind(0) == CssTokenKind_BadString);
    CHECK(tokens.text(0) == "\"ab");
    CHECK(CssTestSource{"\"ab"}.kind(0) == CssTokenKind_String);
    CHECK((CssTestSource{"\"ab"}.flags(0) & CssTokenFlag_EofTerminated) != 0);
}

TEST_CASE("an escape is resolved by the reader, not by the token") {
    CssTestSource tokens{"\\61 b"};
    REQUIRE(tokens.count() == 1);
    CHECK(tokens.kind(0) == CssTokenKind_Ident);
    CHECK(tokens.text(0) == "\\61 b");
    CHECK((tokens.flags(0) & CssTokenFlag_PlainText) == 0);
}

TEST_CASE("every code point above ASCII belongs to an ident") {
    CHECK(CssTestSource{"caf\xc3\xa9"}.all() == std::vector<std::string>{"caf\xc3\xa9"});
    CHECK(CssTestSource{"a\xc2\xa1" "b"}.all() == std::vector<std::string>{"a\xc2\xa1" "b"});    // U+00A1
    CHECK(CssTestSource{"\xc2\x81"}.all() == std::vector<std::string>{"\xc2\x81"});              // U+0081
    CHECK(CssTestSource{"\xe2\x80\xa3 \xe2\x97\xb0"}.all()                                       // U+2023, U+25F0
          == std::vector<std::string>{"\xe2\x80\xa3", " ", "\xe2\x97\xb0"});
    CHECK(CssTestSource{"--\xe2\x9c\xa8"}.kind(0) == CssTokenKind_Ident);                       // U+2728
}

TEST_CASE("an opener knows its closer") {
    CssTestSource tokens{"a{b{c}}"};
    CHECK(cssTokenBlockEnd(&tokens.tokens, 1) == 7);
    CHECK(cssTokenBlockEnd(&tokens.tokens, 3) == 6);
    CHECK((tokens.flags(5) & CssTokenFlag_Unmatched) == 0);
}

TEST_CASE("a block the input never closed ends at the end of the input") {
    CssTestSource tokens{"a{b"};
    CHECK(cssTokenBlockEnd(&tokens.tokens, 1) == tokens.count());
}

TEST_CASE("a closer nothing opened is unmatched") {
    CssTestSource tokens{"a)b"};
    CHECK((tokens.flags(1) & CssTokenFlag_Unmatched) != 0);
}

TEST_CASE("a run of verbatim tokens serializes as its source") {
    CssTestSource tokens{"1px solid red"};
    PrStringView text;
    REQUIRE(cssSpanIsVerbatim(&tokens.tokens, CssTokenSpan{0, tokens.count()}, &text));
    CHECK(textOf(text) == "1px solid red");

    CssTestSource spaced{"1px  solid"};
    CHECK(!cssSpanIsVerbatim(&spaced.tokens, CssTokenSpan{0, spaced.count()}, &text));
}

TEST_CASE("a thousand bad urls tokenize as a thousand bad urls") {
    std::string unit = "a{b:url(x\"y)}";
    std::string text;
    for (uint32_t i = 0; i < 1000; ++i)
        text += unit;

    CssTestSource tokens{text};
    REQUIRE(tokens.count() == 6000);
    for (uint32_t i = 0; i < 1000; ++i) {
        CHECK(tokens.kind(i * 6 + 4) == CssTokenKind_BadUrl);
        CHECK(tokens.text(i * 6 + 4) == "url(x\"y)");
    }
}

TEST_CASE("a unicode range is a token only where the descriptor allows one") {
    CHECK(CssTestSource{"U+0025"}.all() == std::vector<std::string>{"U", "+0025"});

    CssTestSource tokens{"U+0025", CssTokenizeFlag_UnicodeRanges};
    REQUIRE(tokens.count() == 1);
    CHECK(tokens.kind(0) == CssTokenKind_UnicodeRange);
    CHECK(tokens.text(0) == "U+0025");
    CHECK((tokens.flags(0) & CssTokenFlag_Verbatim) == 0);

    uint32_t first = 0;
    uint32_t last = 0;
    cssTokenRange(&tokens.tokens, 0, &first, &last);
    CHECK(first == 0x25);
    CHECK(last == 0x25);
}

TEST_CASE("a question mark in a range stands for both ends of it") {
    CssTestSource tokens{"u+4??", CssTokenizeFlag_UnicodeRanges};
    REQUIRE(tokens.count() == 1);
    CHECK(tokens.text(0) == "u+4??");

    uint32_t first = 0;
    uint32_t last = 0;
    cssTokenRange(&tokens.tokens, 0, &first, &last);
    CHECK(first == 0x400);
    CHECK(last == 0x4ff);
}

TEST_CASE("a range takes its end from the digits after a hyphen") {
    CssTestSource tokens{"u+0-7f x", CssTokenizeFlag_UnicodeRanges};
    CHECK(tokens.text(0) == "u+0-7f");

    uint32_t first = 0;
    uint32_t last = 0;
    cssTokenRange(&tokens.tokens, 0, &first, &last);
    CHECK(first == 0);
    CHECK(last == 0x7f);
}

TEST_CASE("a range takes six digits at most") {
    CssTestSource tokens{"u+12345678", CssTokenizeFlag_UnicodeRanges};
    CHECK(tokens.all() == std::vector<std::string>{"u+123456", "78"});
}

TEST_CASE("url( is not a range") {
    CssTestSource tokens{"url(x)", CssTokenizeFlag_UnicodeRanges};
    CHECK(tokens.kinds() == std::vector<CssTokenKind>{CssTokenKind_Url});
}

TEST_CASE("an escape joins the ident run around it") {
    CHECK(CssTestSource{"a\\62"}.all() == std::vector<std::string>{"a\\62"});
    CHECK(CssTestSource{"a\\62"}.kind(0) == CssTokenKind_Ident);
    CHECK(CssTestSource{"\\61 b"}.all() == std::vector<std::string>{"\\61 b"});
}

TEST_CASE("a unit opening with an e is not the number's exponent") {
    CssTestSource tokens{"2.5em 1e5 1e-5 2e"};
    CHECK(tokens.kind(0) == CssTokenKind_Dimension);
    CHECK((tokens.flags(0) & CssTokenFlag_Verbatim) != 0);
    CHECK((tokens.flags(2) & CssTokenFlag_Verbatim) == 0);   // 1e5 serializes as 100000
    CHECK((tokens.flags(4) & CssTokenFlag_Verbatim) == 0);
    CHECK(tokens.text(6) == "2e");
    CHECK((tokens.flags(6) & CssTokenFlag_Verbatim) != 0);   // `e` alone is no exponent
}

TEST_CASE("a token's value is its bytes with the escapes resolved") {
    CHECK(value(CssTestSource{"\\61 bc"}, 0) == "abc");
    CHECK(value(CssTestSource{"\\000041"}, 0) == "A");
    CHECK(value(CssTestSource{"\\0000000"}, 0) == "\xef\xbf\xbd" "0");
    CHECK(value(CssTestSource{"\\d800"}, 0) == "\xef\xbf\xbd");
    CHECK(value(CssTestSource{"\"a\\\nb\""}, 0) == "ab");
    CHECK(value(CssTestSource{"\"a\\\"b\""}, 0) == "a\"b");
    CHECK(value(CssTestSource{"\"ab\\"}, 0) == "ab");
    CHECK(value(CssTestSource{"ab\\"}, 0) == "ab\xef\xbf\xbd");
}

TEST_CASE("preprocessing settles what a byte spells") {
    CHECK(value(CssTestSource{"a\rb"}, 1) == "\n");
    CHECK(value(CssTestSource{"a\r\nb"}, 1) == "\n");
    CHECK(value(CssTestSource{PrStringView{"a\0b", 3}}, 0) == "a\xef\xbf\xbd" "b");
    CHECK(value(CssTestSource{"a\x80" "b"}, 0) == "a\xef\xbf\xbd" "b");
}

TEST_CASE("a value longer than the buffer says how much it needed") {
    CssTestSource tokens{"abcdef"};
    char buffer[3];
    CHECK(cssValue(cssTokenValueText(&tokens.tokens, 0), buffer, 3) == 6);
}

TEST_CASE("a unit names a unit whatever case or escape it was written with") {
    CssTestSource upper{"1PX"};
    CssTestSource escaped{"1p\\78"};
    CssTestSource unknown{"1zz"};
    CHECK(cssTokenUnit(&upper.tokens, 0) == PrCssUnit_Px);
    CHECK(cssTokenUnit(&escaped.tokens, 0) == PrCssUnit_Px);
    CHECK(cssTokenUnit(&unknown.tokens, 0) == PrCssUnit_Unknown);
}

TEST_CASE("a delim carries its code point") {
    CssTestSource star{"*"};
    CssTestSource wide{"\xc2\xa1"};
    CHECK(cssTokenDelim(&star.tokens, 0) == '*');
    CHECK(cssTokenDelim(&wide.tokens, 0) == 0xa1);
    CHECK(CssTestSource{"~=|=^=$="}.count() == 8);   // two tokens each, not one
}

TEST_CASE("a comment splits the whitespace around it") {
    CssTestSource tokens{"a /**/ b"};
    CHECK(tokens.all() == std::vector<std::string>{"a", " ", " ", "b"});
    CHECK((tokens.flags(2) & CssTokenFlag_Contiguous) == 0);
}

TEST_CASE("a token that ran out of input says so") {
    CHECK((CssTestSource{"url(a"}.flags(0) & CssTokenFlag_EofTerminated) != 0);
    CHECK((CssTestSource{"\"a"}.flags(0) & CssTokenFlag_EofTerminated) != 0);
    CHECK((CssTestSource{"url(a)"}.flags(0) & CssTokenFlag_EofTerminated) == 0);
}

TEST_CASE("a non-printable code point makes a url bad") {
    CHECK(CssTestSource{"url(a\x01" "b)"}.kind(0) == CssTokenKind_BadUrl);
    CHECK(CssTestSource{"url(a b)"}.kind(0) == CssTokenKind_BadUrl);
}

TEST_CASE("a token says whether its bytes are its value and its serialization") {
    CssTestSource tokens{"red \\72 ed 'a' \"a\""};
    CHECK((tokens.flags(0) & (CssTokenFlag_PlainText | CssTokenFlag_Verbatim))
          == (CssTokenFlag_PlainText | CssTokenFlag_Verbatim));
    CHECK((tokens.flags(2) & CssTokenFlag_PlainText) == 0);
    CHECK((tokens.flags(4) & CssTokenFlag_Verbatim) == 0);   // a single quoted string
    CHECK((tokens.flags(6) & CssTokenFlag_Verbatim) != 0);
}

TEST_CASE("a token is the same after a run of every length") {
    CHECK(tokensAfterRuns("color") == std::vector<std::string>{"color"});
    CHECK(tokensAfterRuns("  \t\n ") == std::vector<std::string>{"  \t\n "});
    CHECK(tokensAfterRuns("a:b") == std::vector<std::string>{"a", ":", "b"});
    CHECK(tokensAfterRuns("--main-color") == std::vector<std::string>{"--main-color"});
    CHECK(tokensAfterRuns("1px+2px") == std::vector<std::string>{"1px", "+2px"});
    CHECK(tokensAfterRuns("caf\xc3\xa9") == std::vector<std::string>{"caf\xc3\xa9"});
    CHECK(tokensAfterRuns("\"a'b\"") == std::vector<std::string>{"\"a'b\""});
    CHECK(tokensAfterRuns("\"a\\\"b\"") == std::vector<std::string>{"\"a\\\"b\""});
    CHECK(tokensAfterRuns("\"ab\nc") == std::vector<std::string>{"\"ab", "\n", "c"});
    CHECK(tokensAfterRuns("a/*x*/b") == std::vector<std::string>{"a", "b"});
    CHECK(tokensAfterRuns("/*a*b*/x") == std::vector<std::string>{"x"});
    CHECK(tokensAfterRuns("a/b") == std::vector<std::string>{"a", "/", "b"});
    CHECK(tokensAfterRuns("url(a/b.png)") == std::vector<std::string>{"url(a/b.png)"});
    CHECK(tokensAfterRuns("url( a )x") == std::vector<std::string>{"url( a )", "x"});
    CHECK(tokensAfterRuns("myurl(a)") == std::vector<std::string>{"myurl(", "a", ")"});
    CHECK(tokensAfterRuns("/*\"*/a") == std::vector<std::string>{"a"});
    CHECK(tokensAfterRuns("\"/*\"a") == std::vector<std::string>{"\"/*\"", "a"});
    CHECK(tokensAfterRuns("\\61 b") == std::vector<std::string>{"\\61 b"});
    CHECK(tokensAfterRuns("a\\62") == std::vector<std::string>{"a\\62"});
    CHECK(tokensAfterRuns("-->") == std::vector<std::string>{"-->"});
    CHECK(tokensAfterRuns("a-->") == std::vector<std::string>{"a--", ">"});
    CHECK(tokensAfterRuns("12px/1.5") == std::vector<std::string>{"12px", "/", "1.5"});
    CHECK(tokensAfterRuns("\"a\\\\\"b") == std::vector<std::string>{"\"a\\\\\"", "b"});
}

TEST_CASE("a long string, comment or name is still one token") {
    std::string body(200, 'x');
    CHECK(tokensAfterRuns("\"" + body + "\"y")
          == std::vector<std::string>{"\"" + body + "\"", "y"});
    CHECK(tokensAfterRuns("a/*" + body + "*/b") == std::vector<std::string>{"a", "b"});
    CHECK(tokensAfterRuns(body + "(") == std::vector<std::string>{body + "("});
}

TEST_CASE("the last byte of the input is read no further") {
    CHECK(CssTestSource{"a*"}.all() == std::vector<std::string>{"a", "*"});
    CHECK(CssTestSource{"a/"}.all() == std::vector<std::string>{"a", "/"});
    CHECK(CssTestSource{"/*"}.count() == 0);
    CHECK(CssTestSource{"\\"}.all() == std::vector<std::string>{"\\"});
    CHECK(CssTestSource{"u"}.all() == std::vector<std::string>{"u"});
}

TEST_CASE("a name is the same at every length and whatever ends it") {
    struct Stop {
        const char* text;
        bool joins;          // true if the name takes the code point in
        CssTokenKind kind;
        bool plain;
    };
    const Stop stops[] = {
        {"", true, CssTokenKind_Ident, true},
        {":", false, CssTokenKind_Ident, true},
        {"(", true, CssTokenKind_Function, true},
        {" ", false, CssTokenKind_Ident, true},
        {"\\41", true, CssTokenKind_Ident, false},
        {"\xc3\xa9", true, CssTokenKind_Ident, false},    // U+00E9
        {"\xc2\xa0", true, CssTokenKind_Ident, false},    // U+00A0
    };
    for (uint32_t length = 1; length <= 40; ++length) {
        std::string name(length, 'n');
        for (const Stop& stop : stops) {
            std::string input = name + stop.text;
            CAPTURE(input);
            CssTestSource tokens{input};
            CHECK(tokens.text(0) == (stop.joins ? input : name));
            CHECK(tokens.kind(0) == stop.kind);
            CHECK(((tokens.flags(0) & CssTokenFlag_PlainText) != 0) == stop.plain);
        }
    }
}

TEST_CASE("two hyphens are a CDC only before a '>'") {
    CHECK(CssTestSource{"-->"}.kinds() == std::vector<CssTokenKind>{CssTokenKind_Cdc});
    CHECK(CssTestSource{"--->"}.all() == std::vector<std::string>{"---", ">"});
    CHECK(CssTestSource{"--a>"}.all() == std::vector<std::string>{"--a", ">"});
    CHECK(CssTestSource{"-a>"}.all() == std::vector<std::string>{"-a", ">"});
    CHECK(CssTestSource{"--"}.kinds() == std::vector<CssTokenKind>{CssTokenKind_Ident});
    CHECK(CssTestSource{"--("}.kinds() == std::vector<CssTokenKind>{CssTokenKind_Function});
}

TEST_CASE("a sheet decodes in the charset of its mark, its response, its @charset rule or its document") {
    auto decode = [](PrStringView bytes, PrCharset protocol, PrCharset environment) {
        PrString out = {};
        PrStringView text = cssDecodeBytes(bytes, protocol, environment, &out);
        std::string result(text.data, text.size);
        prStringReset(&out);
        return result;
    };
    CHECK(decode("a{}", PrCharset_None, PrCharset_None) == "a{}");
    CHECK(decode("\xE9", PrCharset_None, PrCharset_None) == "\xEF\xBF\xBD");
    CHECK(decode("\xE9", PrCharset_None, PrCharset_Windows1252) == "\xC3\xA9");
    CHECK(decode("@charset \"koi8-r\";\xC1", PrCharset_None, PrCharset_Windows1252) == "@charset \"koi8-r\";\xD0\xB0");
    CHECK(decode("@charset \"koi8-r\";\xC1", PrCharset_Windows1251, PrCharset_None) == "@charset \"koi8-r\";\xD0\x91");
    CHECK(decode("\xEF\xBB\xBF@charset \"koi8-r\";\xC3\xA9", PrCharset_Windows1251, PrCharset_None)
          == "@charset \"koi8-r\";\xC3\xA9");
    // Only the rule as the grammar writes it names a charset, and a name of UTF-16 means UTF-8.
    CHECK(decode("@charset 'koi8-r';\xC3\xA9", PrCharset_None, PrCharset_None) == "@charset 'koi8-r';\xC3\xA9");
    CHECK(decode("@charset \"utf-16\";\xC3\xA9", PrCharset_None, PrCharset_Windows1252)
          == "@charset \"utf-16\";\xC3\xA9");
}
