#include "pr/min_max.h"
#include "pr/string_ops.h"
#include "pr/url_percent.h"

#include "tools/harness/test.h"

extern "C" bool prTestPercentC(void);

namespace pr_impl {

namespace {

String encode(PrStringView input, PrUrlPercentEncodeSet set) {
    size_t size = prUrlPercentEncode(input, set, nullptr, 0);
    Array<char> result(size);
    CHECK(prUrlPercentEncode(input, set, result.data(), result.count()) == size);
    return String(PrStringView{result.data(), result.count()});
}
String decode(PrStringView input) {
    size_t size = prUrlPercentDecode(input, nullptr, 0);
    Array<char> result(size);
    CHECK(prUrlPercentDecode(input, result.data(), result.count()) == size);
    return String(PrStringView{result.data(), result.count()});
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("percent decoding preserves arbitrary bytes and malformed escapes") {
    constexpr char kDecoded[] = "\0\xff//+%G0%0%";
    CHECK(decode("%00%ff%2F%2f+%G0%0%") == PrStringView(kDecoded, sizeof(kDecoded) - 1));
    CHECK(decode("%2520") == "%20");
    CHECK(decode(PrStringView{}) == "");
}

TEST_CASE("each percent-encode set encodes its own bytes") {
    CHECK(encode(" /?#'`%+", PrUrlPercentEncodeSet_C0Control) == " /?#'`%+");
    CHECK(encode(" /?#'`%+", PrUrlPercentEncodeSet_Fragment) == "%20/?#'%60%+");
    CHECK(encode(" /?#'`%+", PrUrlPercentEncodeSet_Query) == "%20/?%23'`%+");
    CHECK(encode(" /?#'`%+", PrUrlPercentEncodeSet_SpecialQuery) == "%20/?%23%27`%+");
    CHECK(encode(" /?#'`%+", PrUrlPercentEncodeSet_Path) == "%20/%3F%23'%60%+");
    CHECK(encode(" /?#'`%+", PrUrlPercentEncodeSet_Userinfo) == "%20%2F%3F%23'%60%+");
    CHECK(encode(" /?#'`%+", PrUrlPercentEncodeSet_Component) == "%20%2F%3F%23'%60%25%2B");
    CHECK(encode(" /?#'`%+", PrUrlPercentEncodeSet_Form) == "%20%2F%3F%23%27%60%25%2B");
    CHECK(encode(PrStringView{}, PrUrlPercentEncodeSet_Path).isEmpty());
}

TEST_CASE("percent codec sizes are exact for every buffer capacity") {
    String input;
    for (uint32_t i = 0; i < 256; ++i) input.append(char(i));
    for (PrUrlPercentEncodeSet set = 0; set < PrUrlPercentEncodeSet_Count; ++set) {
        PrStringView bytes = input;
        String full = encode(bytes, set);
        CHECK(decode(full) == input);
        CHECK(prUrlPercentEncode(bytes, set, nullptr, 999) == full.count());
        for (uint32_t capacity = 0; capacity <= full.count() + 1; ++capacity) {
            Array<char> buffer(capacity + 1, '!');
            CHECK(prUrlPercentEncode(bytes, set, buffer.data(), capacity) == full.count());
            CHECK(PrStringView(buffer.data(), min(capacity, full.count())) == prStringHead(full, capacity));
            CHECK(buffer[capacity] == '!');
        }
        for (uint32_t capacity = 0; capacity <= input.count() + 1; ++capacity) {
            Array<char> buffer;
            buffer.append(full.data(), full.count());
            PrStringView decoded(buffer.data(), buffer.count());
            CHECK(prUrlPercentDecode(decoded, buffer.data(), capacity) == input.count());
            CHECK(prStringHead(decoded, min(capacity, input.count())) == prStringHead(input, capacity));
            CHECK(prStringFrom(decoded, capacity) == prStringFrom(full, capacity));
        }
    }
    CHECK(prUrlPercentDecode("%20+", nullptr, 99) == 2);
}

TEST_CASE("the percent codec compiles and links as C") {
    CHECK(prTestPercentC());
}
