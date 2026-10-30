#include "pr/url_percent.h"

#include "doctest/doctest.h"

#include <string>
#include <vector>

extern "C" bool prTestPercentC(void);

namespace pr_impl {

namespace {

std::string encode(PrStringView input, PrUrlPercentEncodeSet set) {
    size_t size = prUrlPercentEncode(input, set, nullptr, 0);
    std::string result(size, '\0');
    CHECK(prUrlPercentEncode(input, set, result.data(), result.size()) == size);
    return result;
}
std::string decode(PrStringView input) {
    size_t size = prUrlPercentDecode(input, nullptr, 0);
    std::string result(size, '\0');
    CHECK(prUrlPercentDecode(input, result.data(), result.size()) == size);
    return result;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("percent decoding preserves arbitrary bytes and malformed escapes") {
    CHECK(decode("%00%ff%2F%2f+%G0%0%") == (std::string("\0\xff", 2) + "//+%G0%0%"));
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
    CHECK(encode(PrStringView{}, PrUrlPercentEncodeSet_Path).empty());
}

TEST_CASE("percent codec sizes are exact for every buffer capacity") {
    std::string input;
    for (uint32_t i = 0; i < 256; ++i) input += char(i);
    for (PrUrlPercentEncodeSet set = 0; set < PrUrlPercentEncodeSet_Count; ++set) {
        PrStringView bytes = {input.data(), input.size()};
        std::string full = encode(bytes, set);
        CHECK(decode({full.data(), full.size()}) == input);
        CHECK(prUrlPercentEncode(bytes, set, nullptr, 999) == full.size());
        for (size_t capacity = 0; capacity <= full.size() + 1; ++capacity) {
            std::vector<char> buffer(capacity + 1, '!');
            CHECK(prUrlPercentEncode(bytes, set, buffer.data(), capacity) == full.size());
            CHECK(std::string(buffer.data(), std::min(capacity, full.size())) == full.substr(0, capacity));
            CHECK(buffer[capacity] == '!');
        }
        for (size_t capacity = 0; capacity <= input.size() + 1; ++capacity) {
            std::string buffer = full;
            CHECK(prUrlPercentDecode({buffer.data(), buffer.size()}, buffer.data(), capacity) == input.size());
            CHECK(buffer.substr(0, std::min(capacity, input.size())) == input.substr(0, capacity));
            CHECK(buffer.substr(capacity) == full.substr(capacity));
        }
    }
    CHECK(prUrlPercentDecode("%20+", nullptr, 99) == 2);
}

TEST_CASE("the percent codec compiles and links as C") {
    CHECK(prTestPercentC());
}
