#include "vterm/src/vterm_parser.h"

#include "tools/harness/test.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace pr_impl {

namespace {

// An action in the form that the tests compare: its kind and a description of what it carries.
struct Parsed {
    VtermActionKind kind;
    std::string text;
};

// Describes an action. A print gives its code points, an execute gives its byte, and a string gives its text. A
// sequence gives its marker, parameters, intermediates and final byte. A colon precedes a subparameter, and `_` stands
// for an empty parameter.
std::string describe(const VtermAction* action) {
    switch (action->kind) {
        case VtermActionKind_Ascii:
        case VtermActionKind_Osc:
        case VtermActionKind_Apc:
            return std::string(action->text.data, action->text.size);
        case VtermActionKind_Print: {
            std::string out;
            for (uint32_t i = 0; i < action->codePointCount; ++i) {
                char hex[16];
                snprintf(hex, sizeof(hex), "%sU+%04X", i == 0 ? "" : " ", action->codePoints[i]);
                out += hex;
            }
            return out;
        }
        case VtermActionKind_Execute:
            return std::to_string(uint8_t(action->final));
        default:
            break;
    }
    std::string out;
    if (action->marker != 0)
        out += action->marker;
    for (uint32_t i = 0; i < action->paramCount; ++i) {
        if (i > 0)
            out += vtermParamIsSub(action, i) ? ":" : ";";
        uint32_t value = action->params[i];
        out += value == kVtermParamNone ? "_" : std::to_string(value);
    }
    out.append(action->intermediates, action->intermediateCount);
    out += action->final;
    if (action->kind == VtermActionKind_Dcs)
        out += "|" + std::string(action->text.data, action->text.size);
    return out;
}

// The actions that one parser produces from `chunks`, written to it in order.
std::vector<Parsed> parse(const std::vector<std::string>& chunks) {
    VtermParser parser{};
    std::vector<Parsed> out;
    for (const std::string& chunk : chunks) {
        size_t at = 0;
        PrStringView text{chunk.data(), chunk.size()};
        for (;;) {
            VtermAction* action = vtermParserNext(&parser, text, &at);
            if (action->kind == VtermActionKind_None)
                break;
            out.push_back(Parsed{action->kind, describe(action)});
        }
    }
    vtermParserReset(&parser);
    return out;
}

std::vector<Parsed> parse(const std::string& text) {
    return parse(std::vector<std::string>{text});
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("vterm parser: printable ASCII is one run, and a control character between runs executes") {
    std::vector<Parsed> actions = parse("ab\ncd");
    REQUIRE(actions.size() == 3);
    CHECK(actions[0].kind == VtermActionKind_Ascii);
    CHECK(actions[0].text == "ab");
    CHECK(actions[1].kind == VtermActionKind_Execute);
    CHECK(actions[1].text == "10");
    CHECK(actions[2].text == "cd");
}

TEST_CASE("vterm parser: UTF-8 decodes across writes, and a malformed sequence is one replacement character") {
    std::vector<Parsed> actions = parse(std::vector<std::string>{"\xe2\x82", "\xac"});
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].kind == VtermActionKind_Print);
    CHECK(actions[0].text == "U+20AC");

    actions = parse("\xe2\x82x\xc0\xaf");
    REQUIRE(actions.size() == 3);
    CHECK(actions[0].text == "U+FFFD");
    CHECK(actions[1].text == "x");
    CHECK(actions[2].text == "U+FFFD U+FFFD");

    // An overlong form and a surrogate are not scalar values. Each maximal subpart becomes one replacement character,
    // whether the write holds the whole sequence or ends inside it.
    const char* const kReplaced = "U+FFFD U+FFFD U+FFFD U+FFFD U+FFFD U+FFFD";
    actions = parse("\xe0\x80\x80\xed\xa0\x80");
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].text == kReplaced);
    actions = parse("\xe0\x80\x80\xed\xa0\x80" "abcd");
    REQUIRE(actions.size() == 2);
    CHECK(actions[0].text == kReplaced);
}

TEST_CASE("vterm parser: text past ASCII prints as runs of code points, which a write may end within") {
    // Each code point decodes the same whether the write holds its sequence or splits it.
    std::string text = "\xe4\xb8\xad\xe6\x96\x87\xf0\x9f\x98\x80\xc3\xa9";
    const char* const kDecoded = "U+4E2D U+6587 U+1F600 U+00E9";
    std::vector<Parsed> actions = parse(text + "!");
    REQUIRE(actions.size() == 2);
    CHECK(actions[0].kind == VtermActionKind_Print);
    CHECK(actions[0].text == kDecoded);
    CHECK(actions[1].text == "!");
    for (size_t split = 1; split < text.size(); ++split) {
        actions = parse(std::vector<std::string>{text.substr(0, split), text.substr(split)});
        std::string decoded;
        for (const Parsed& action : actions)
            decoded += (decoded.empty() ? "" : " ") + action.text;
        INFO(split);
        CHECK(decoded == kDecoded);
    }

    // A run longer than one action can hold continues in the next action.
    std::string repeated;
    for (uint32_t i = 0; i < kVtermParserMaxPrint + 10; ++i)
        repeated += "\xc3\xa9";
    actions = parse(repeated);
    REQUIRE(actions.size() == 2);
    CHECK(actions[1].text == "U+00E9 U+00E9 U+00E9 U+00E9 U+00E9 U+00E9 U+00E9 U+00E9 U+00E9 U+00E9");
}

TEST_CASE("vterm parser: control sequences carry their marker, parameters, subparameters and intermediates") {
    std::vector<Parsed> actions = parse("\x1b[1;22H\x1b[?1049h\x1b[38:2::10:20:30m\x1b[;5H\x1b[2 q\x1b[m");
    REQUIRE(actions.size() == 6);
    for (const Parsed& action : actions)
        CHECK(action.kind == VtermActionKind_Csi);
    CHECK(actions[0].text == "1;22H");
    CHECK(actions[1].text == "?1049h");
    CHECK(actions[2].text == "38:2:_:10:20:30m");
    CHECK(actions[3].text == "_;5H");
    CHECK(actions[4].text == "2 q");
    CHECK(actions[5].text == "m");
}

TEST_CASE("vterm parser: a sequence split across writes completes, and a control character inside it executes") {
    std::vector<Parsed> actions = parse(std::vector<std::string>{"\x1b[3", "1\nm"});
    REQUIRE(actions.size() == 2);
    CHECK(actions[0].kind == VtermActionKind_Execute);
    CHECK(actions[1].kind == VtermActionKind_Csi);
    CHECK(actions[1].text == "31m");
}

TEST_CASE("vterm parser: CAN cancels a sequence, and a marker after a parameter ignores it") {
    std::vector<Parsed> actions = parse("\x1b[12\x18x\x1b[1?2hy");
    REQUIRE(actions.size() == 2);
    CHECK(actions[0].text == "x");
    CHECK(actions[1].text == "y");
}

TEST_CASE("vterm parser: escape sequences dispatch with their intermediates") {
    std::vector<Parsed> actions = parse("\x1b" "7\x1b(0\x1b#8");
    REQUIRE(actions.size() == 3);
    for (const Parsed& action : actions)
        CHECK(action.kind == VtermActionKind_Escape);
    CHECK(actions[0].text == "7");
    CHECK(actions[1].text == "(0");
    CHECK(actions[2].text == "#8");
}

TEST_CASE("vterm parser: an OSC string ends at BEL or ST, and holds UTF-8") {
    std::vector<Parsed> actions = parse("\x1b]0;t\xc3\xa9st\x07\x1b]2;two\x1b\\z");
    REQUIRE(actions.size() == 4);
    CHECK(actions[0].kind == VtermActionKind_Osc);
    CHECK(actions[0].text == "0;t\xc3\xa9st");
    CHECK(actions[1].kind == VtermActionKind_Osc);
    CHECK(actions[1].text == "2;two");
    // The `\` of ST is an escape sequence that does nothing.
    CHECK(actions[2].kind == VtermActionKind_Escape);
    CHECK(actions[3].text == "z");
}

TEST_CASE("vterm parser: a DCS and an APC carry their strings, and SOS and PM strings are dropped") {
    std::vector<Parsed> actions = parse("\x1bP1$qm\x1b\\\x1b_Gf=24;AAAA\x1b\\\x1bXsos\x1b\\\x1b^pm\x1b\\x");
    REQUIRE(actions.size() == 7);
    CHECK(actions[0].kind == VtermActionKind_Dcs);
    CHECK(actions[0].text == "1$q|m");
    CHECK(actions[1].kind == VtermActionKind_Escape);
    CHECK(actions[2].kind == VtermActionKind_Apc);
    CHECK(actions[2].text == "Gf=24;AAAA");
    CHECK(actions[3].kind == VtermActionKind_Escape);
    CHECK(actions[4].kind == VtermActionKind_Escape);
    CHECK(actions[5].kind == VtermActionKind_Escape);
    CHECK(actions[6].text == "x");
}

TEST_CASE("vterm parser: parameters past the most are dropped, and a large value is clamped") {
    std::string text = "\x1b[";
    for (uint32_t i = 0; i < 40; ++i)
        text += "1;";
    text += "99999999m";
    std::vector<Parsed> actions = parse(text);
    REQUIRE(actions.size() == 1);
    CHECK(std::count(actions[0].text.begin(), actions[0].text.end(), ';') == kVtermParserMaxParams - 1);

    VtermParser parser{};
    size_t at = 0;
    std::string clamp = "\x1b[99999999m";
    VtermAction* action = vtermParserNext(&parser, clamp, &at);
    CHECK(vtermParam(action, 0, 0) == kVtermParserMaxParamValue);
    CHECK(vtermParamCount(action, 1, 7) == 7);
    vtermParserReset(&parser);
}
