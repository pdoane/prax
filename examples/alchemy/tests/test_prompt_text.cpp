#include "examples/alchemy/prompt_text.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

using namespace alchemy;

namespace {

constexpr double kUp = kPromptWeightStep;
constexpr double kDown = -kPromptWeightStep;

// An adjustment and the text that it writes, or "" for one that makes no edit.
struct WeightCase {
    const char* text;
    uint32_t start;
    uint32_t end;
    double delta;
    const char* expected;
};

std::string adjusted(const WeightCase& c) {
    WeightEdit edit;
    return promptWeightAdjust(c.text, c.start, c.end, c.delta, &edit) ? edit.text : std::string();
}

} // namespace

TEST_CASE("a bare caret weights the word that it stands in, and nothing past a group") {
    for (const WeightCase& c : std::vector<WeightCase>{
            {"hello world", 2, 2, kUp, "(hello:1.05)"},
            {"a person, walking", 5, 5, kUp, "(person:1.05)"},
            {"say,hello world", 6, 6, kUp, "(hello:1.05)"},
            {"person, walking", 6, 6, kUp, "(person:1.05)"},
            {"hello", 0, 0, kUp, "(hello:1.05)"},
            {"hello", 5, 5, kUp, "(hello:1.05)"},
            {"hello world", 5, 5, kUp, "(hello:1.05)"},
            {"", 0, 0, kUp, ""},
            {"(hello:1.1)", 11, 11, kUp, ""},
    }) {
        CAPTURE(c.text);
        CHECK(adjusted(c) == c.expected);
    }
}

TEST_CASE("a caret or a selection in a group adjusts that group, which may span lines") {
    for (const WeightCase& c : std::vector<WeightCase>{
            {"(person:1.05)", 3, 3, kUp, "(person:1.1)"},
            {"(person:1.05)", 1, 7, kUp, "(person:1.1)"},
            {"((a:1.1):1.2)", 3, 3, kUp, "(a:1.15)"},
            {"(a (b:1.2) c:1.1)", 11, 11, kUp, "(a (b:1.2) c:1.15)"},
            {"(a\nb:1.1)", 4, 4, kUp, "(a\nb:1.15)"},
            {"a\nb", 0, 3, kUp, "(a\nb:1.05)"},
            {"(person)", 3, 3, kUp, "(person:1.15)"},
            {"(person)", 3, 3, kDown, "(person:1.05)"},
            {"(person)", 0, 8, kUp, "(person:1.15)"},
            {"a (person) b", 2, 10, kUp, "(person:1.15)"},
            {"(a:1.1:b)", 2, 2, kUp, "(a:1.1:b:1.15)"},
            {"say \\(hello\\) world", 7, 7, kUp, "(hello:1.05)"},
            {"(a \\) b:1.1)", 2, 2, kUp, "(a \\) b:1.15)"},
            {"(a \\( b:1.1)", 2, 2, kUp, "(a \\( b:1.15)"},
    }) {
        CAPTURE(c.text);
        CHECK(adjusted(c) == c.expected);
    }
    WeightEdit edit;
    REQUIRE(promptWeightAdjust("(person:1.05)", 3, 3, kUp, &edit));
    CHECK(edit.begin == 0);
    CHECK(edit.end == 13);
    REQUIRE(promptWeightAdjust("(person)", 1, 7, kUp, &edit));
    CHECK(edit.end == 8);
}

TEST_CASE("a weight moves in steps, stops at zero once, and a weight of one drops its group") {
    for (const WeightCase& c : std::vector<WeightCase>{
            {"(hello:1.05)", 0, 12, kUp, "(hello:1.1)"},
            {"(hello:1.1)", 0, 11, kDown, "(hello:1.05)"},
            {"(hello:1.05)", 0, 12, kDown, "hello"},
            {"(hello:0.05)", 0, 12, -0.1, "(hello:0)"},
            {"(hello:0)", 0, 9, kDown, "(hello:-0.05)"},
            {"(hello:-0.5)", 0, 12, kDown, "(hello:-0.55)"},
            {"(hello:-0.5)", 0, 12, kUp, "(hello:-0.45)"},
            {"(hello:1.95)", 0, 12, kUp, "(hello:2)"},
            {"(a:b:1.1)", 4, 4, kUp, "(a:b:1.15)"},
            {"(hello : 1.1 )", 3, 3, kUp, "(hello :1.15)"},
            {"(hello:.9)", 3, 3, kUp, "(hello:0.95)"},
            {"(hello:11e-1)", 3, 3, kUp, "(hello:1.15)"},
            {"(hello:-0.5)", 3, 3, 0.6, "(hello:0.1)"},
            {"hello", 0, 5, kUp, "(hello:1.05)"},
            {"hello", 0, 5, kDown, "(hello:0.95)"},
            {"a misty forest at dawn", 8, 22, kUp, "(forest at dawn:1.05)"},
            {"hello", 0, 5, 0.0, ""},
    }) {
        CAPTURE(c.text);
        CHECK(adjusted(c) == c.expected);
    }
    WeightEdit edit;
    REQUIRE(promptWeightAdjust("say hello world", 4, 9, kUp, &edit));
    CHECK(edit.begin == 4);
    CHECK(edit.end == 9);
}

TEST_CASE("a prompt's lines are its pieces, and a line appends as a piece of its own") {
    CHECK(promptLines("").empty());
    CHECK(promptLines("  \n ").empty());
    CHECK(promptLines("  a fox\nin snow\n") == std::vector<PrStringView>{"a fox", "in snow"});
    CHECK(promptAppendLine("", "a fox") == "a fox");
    CHECK(promptAppendLine("a fox", "in snow") == "a fox\nin snow");
}

TEST_CASE("an embedding inserts as its name, spaced from the text that it meets") {
    CHECK(embeddingName("easynegative.safetensors") == "easynegative");
    CHECK(embeddingName("Bad.PT") == "Bad");
    CHECK(embeddingName("plain") == "plain");
    CHECK(embeddingInsertText("", 0, 0, "neg.pt") == "embedding:neg");
    CHECK(embeddingInsertText("a,b", 2, 2, "neg.pt") == " embedding:neg ");
    CHECK(embeddingInsertText("a b", 2, 2, "neg.pt") == "embedding:neg ");
    CHECK(embeddingInsertText("a ", 2, 2, "neg.pt") == "embedding:neg");
}
