#include "pr/float.h"
#include "pr/json.h"
#include "pr/string_ops.h"

#include "doctest/doctest.h"

#include <initializer_list>

extern "C" int prTestJsonCApi(void);

namespace pr_impl {

namespace {

bool parse(const char* text, PrJson* d) {
    return prJsonParse(d, text, nullptr);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestJsonCApi() == 0);
}

TEST_CASE("scalars read as themselves") {
    PrJson* d = prJsonCreate();
    REQUIRE(parse(" 12.5e1 ", d));
    CHECK(prJsonKind(d, 0) == PrJsonKind_Number);
    CHECK(prJsonNumber(d, 0, 0) == 125);
    REQUIRE(parse("-0", d));
    CHECK(prJsonNumber(d, 0, 1) == 0);
    REQUIRE(parse("true", d));
    CHECK(prJsonKind(d, 0) == PrJsonKind_True);
    CHECK(prJsonNumber(d, 0, 5) == 1);
    REQUIRE(parse("null", d));
    CHECK(prJsonKind(d, 0) == PrJsonKind_Null);
    CHECK(prJsonNumber(d, 0, 5) == 5);
    REQUIRE(parse("\"\"", d));
    CHECK(prJsonString(d, 0) == "");
    CHECK(prJsonString(d, 0).data != nullptr);
    prJsonDestroy(d);
}

TEST_CASE("containers span what they hold, and members are a key then a value") {
    PrJson* d = prJsonCreate();
    REQUIRE(parse("{\"a\": [1, [2, 3], {}], \"b\": \"x\", \"a\": 4}", d));
    CHECK(prJsonCount(d, 0) == 3);
    CHECK(prJsonNext(d, 0) == prJsonValueCount(d));
    uint32_t a = prJsonElement(d, prJsonFirst(d, 0) + 1, 1);
    CHECK(prJsonKind(d, a) == PrJsonKind_Array);
    CHECK(prJsonFirst(d, prJsonElement(d, prJsonFirst(d, 0) + 1, 2)) == kPrJsonMissing);
    CHECK(prJsonNumber(d, prJsonElement(d, a, 1), 0) == 3);
    CHECK(prJsonElement(d, a, 2) == kPrJsonMissing);
    // The last of two members with the same name.
    CHECK(prJsonNumber(d, prJsonMember(d, 0, "a"), 0) == 4);
    CHECK(prJsonString(d, prJsonMember(d, 0, "b")) == "x");
    CHECK(prJsonMember(d, 0, "c") == kPrJsonMissing);
    uint32_t keys = 0;
    for (uint32_t i = prJsonFirst(d, 0); i != kPrJsonMissing; i = prJsonAfter(d, 0, i))
        ++keys;
    CHECK(keys == 3);
    uint32_t inner = prJsonMember(d, 0, "a");
    CHECK(prJsonKind(d, inner) == PrJsonKind_Number);
    prJsonDestroy(d);
}

TEST_CASE("escapes unescape, and a lone surrogate is a replacement") {
    PrJson* d = prJsonCreate();
    REQUIRE(parse("[\"a\\n\\u00e9\\ud83d\\ude00\\/\", \"\\udc00\", \"\xC3\xA9\"]", d));
    CHECK(prJsonString(d, prJsonElement(d, 0, 0)) == "a\n\xC3\xA9\xF0\x9F\x98\x80/");
    CHECK(prJsonString(d, prJsonElement(d, 0, 1)) == "\xEF\xBF\xBD");
    CHECK(prJsonString(d, prJsonElement(d, 0, 2)) == "\xC3\xA9");
    prJsonDestroy(d);
}

TEST_CASE("what is not JSON fails where it stops") {
    PrJson* d = prJsonCreate();
    size_t at = 0;
    for (const char* text : {"", "[1,]", "{\"a\" 1}", "[1 2]", "01", "1.", "\"a", "\"\t\"", "[}",
        "{1: 2}", "tru", "[1] x", "\"\\x\""})
        CHECK_FALSE(prJsonParse(d, text, &at));
    CHECK_FALSE(prJsonParse(d, "[1, 2 x", &at));
    CHECK(at == 6);
    CHECK(prJsonValueCount(d) == 0);
    prJsonDestroy(d);
}

TEST_CASE("nesting past the limit fails") {
    PrJson* d = prJsonCreate();
    char text[2 * (kPrJsonMaxDepth + 1)];
    for (uint32_t i = 0; i <= kPrJsonMaxDepth; ++i) {
        text[i] = '[';
        text[2 * kPrJsonMaxDepth + 1 - i] = ']';
    }
    CHECK_FALSE(prJsonParse(d, PrStringView{text, sizeof(text)}, nullptr));
    CHECK(prJsonParse(d, PrStringView{text + 1, sizeof(text) - 2}, nullptr));
    prJsonDestroy(d);
}

TEST_CASE("a writer separates elements and members, and closes what it opened") {
    PrJsonWriter* w = prJsonWriterCreate();
    prJsonWriterBeginObject(w);
    prJsonWriterKey(w, "a");
    prJsonWriterBeginArray(w);
    prJsonWriterNumber(w, 1);
    prJsonWriterBeginArray(w);
    prJsonWriterEndArray(w);
    prJsonWriterBeginObject(w);
    prJsonWriterEndObject(w);
    prJsonWriterBool(w, false);
    prJsonWriterNull(w);
    prJsonWriterEndArray(w);
    prJsonWriterKey(w, "b");
    prJsonWriterString(w, "x");
    prJsonWriterEndObject(w);
    CHECK(prJsonWriterText(w) == "{\"a\":[1,[],{},false,null],\"b\":\"x\"}");
    prJsonWriterClear(w);
    CHECK(prJsonWriterText(w) == "");
    prJsonWriterNumber(w, 2);
    CHECK(prJsonWriterText(w) == "2");
    prJsonWriterDestroy(w);
}

TEST_CASE("a writer escapes strings and writes numbers in their shortest form") {
    PrJsonWriter* w = prJsonWriterCreate();
    prJsonWriterBeginArray(w);
    prJsonWriterString(w, "q\"b\\n\n\x01\xC3\xA9\xFF");
    for (double number : {0.1, -2.5, 1e21, 1e-7, 123456789.0})
        prJsonWriterNumber(w, number);
    prJsonWriterNumber(w, kPrF64Infinity);
    prJsonWriterEndArray(w);
    CHECK(prJsonWriterText(w) == "[\"q\\\"b\\\\n\\n\\u0001\xC3\xA9\xEF\xBF\xBD\",0.1,-2.5,1e+21,1e-7,123456789,null]");
    prJsonWriterDestroy(w);
}

TEST_CASE("a document's value writes back as the text that reads to it") {
    const char* text = "{\"a\":[1,\"\\u00e9\\n\",{\"b\":null}],\"c\":true}";
    PrJson* d = prJsonCreate();
    REQUIRE(parse(text, d));
    PrJsonWriter* w = prJsonWriterCreate();
    prJsonWriterValue(w, d, 0);
    CHECK(prJsonWriterText(w) == "{\"a\":[1,\"\xC3\xA9\\n\",{\"b\":null}],\"c\":true}");
    prJsonWriterClear(w);
    prJsonWriterBeginArray(w);
    prJsonWriterValue(w, d, prJsonMember(d, 0, "a"));
    prJsonWriterValue(w, d, prJsonMember(d, 0, "c"));
    prJsonWriterEndArray(w);
    CHECK(prJsonWriterText(w) == "[[1,\"\xC3\xA9\\n\",{\"b\":null}],true]");
    prJsonWriterDestroy(w);
    prJsonDestroy(d);
}
