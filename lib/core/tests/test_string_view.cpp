#include "pr/string_view.h"

#include "tools/harness/test.h"

#include <array>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// Text converts. Bytes, wide text and a char that is not a run do not.
static_assert(std::is_convertible_v<const char*, PrStringView>);
static_assert(std::is_convertible_v<std::string, PrStringView>);
static_assert(std::is_convertible_v<std::string_view, PrStringView>);
static_assert(std::is_convertible_v<std::vector<char>, PrStringView>);
static_assert(std::is_convertible_v<std::array<char, 4>, PrStringView>);
static_assert(!std::is_convertible_v<const uint8_t*, PrStringView>);
static_assert(!std::is_convertible_v<std::vector<uint8_t>, PrStringView>);
static_assert(!std::is_convertible_v<std::wstring, PrStringView>);
static_assert(!std::is_convertible_v<char, PrStringView>);

// A literal is measured at compile time.
static_assert(PrStringView("color").size == 5);
static_assert(PrStringView("color") == "color");

TEST_CASE("a NUL-terminated string views its bytes, and a null pointer is the null view") {
    const char* text = "a\0b";
    PrStringView view = text;
    CHECK(view.data == text);
    CHECK(view.size == 1);

    const char* none = nullptr;
    PrStringView null = none;
    CHECK(null.data == nullptr);
    CHECK(null.size == 0);
}

TEST_CASE("a run of chars views its bytes, including U+0000") {
    std::string owned("a\0b", 3);
    PrStringView view = owned;
    CHECK(view.data == owned.data());
    CHECK(view.size == 3);

    std::string_view borrowed = "xyz";
    CHECK(PrStringView(borrowed) == "xyz");
}

TEST_CASE("equality is by bytes and length") {
    CHECK(PrStringView("color") == "color");
    CHECK_FALSE(PrStringView("color") == "colo");
    CHECK_FALSE(PrStringView("color") == "Color");
    CHECK(PrStringView("color") != "Color");
    CHECK(PrStringView(std::string("a\0b", 3)) != "a");
    CHECK(PrStringView(std::string("a\0b", 3)) == std::string("a\0b", 3));
}

TEST_CASE("the null view equals the empty string") {
    CHECK(PrStringView{} == "");
    CHECK("" == PrStringView{});
}

TEST_CASE("a view iterates its bytes") {
    std::string out;
    for (char c : PrStringView("abc"))
        out += c;
    CHECK(out == "abc");

    size_t count = 0;
    for ([[maybe_unused]] char c : PrStringView{})
        ++count;
    CHECK(count == 0);
}
