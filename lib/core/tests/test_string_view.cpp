#include "pr/array.h"
#include "pr/string.h"
#include "pr/string_view.h"
#include "pr/type_traits.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// Runs of each kind of element, as anything with data() and size() is.
template <typename Element>
struct Run {
    const Element* PR_NULLABLE elements = nullptr;
    size_t used = 0;
    const Element* PR_NULLABLE data() const { return elements; }
    size_t size() const { return used; }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// Text converts. Bytes, wide text and a char that is not a run do not.
static_assert(isConvertible<const char*, PrStringView>);
static_assert(isConvertible<String, PrStringView>);
static_assert(isConvertible<Run<char>, PrStringView>);
static_assert(!isConvertible<const uint8_t*, PrStringView>);
static_assert(!isConvertible<Run<uint8_t>, PrStringView>);
static_assert(!isConvertible<Array<uint8_t>, PrStringView>);
static_assert(!isConvertible<Run<wchar_t>, PrStringView>);
static_assert(!isConvertible<char, PrStringView>);

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
    const char kBytes[] = {'a', '\0', 'b'};
    Run<char> run = {.elements = kBytes, .used = 3};
    PrStringView view = run;
    CHECK(view.data == kBytes);
    CHECK(view.size == 3);

    String owned(PrStringView("a\0b", 3));
    PrStringView ownedView = owned;
    CHECK(ownedView.data == owned.data());
    CHECK(ownedView.size == 3);
}

TEST_CASE("equality is by bytes and length") {
    CHECK(PrStringView("color") == "color");
    CHECK_FALSE(PrStringView("color") == "colo");
    CHECK_FALSE(PrStringView("color") == "Color");
    CHECK(PrStringView("color") != "Color");
    CHECK(PrStringView("a\0b", 3) != "a");
    CHECK(PrStringView("a\0b", 3) == String(PrStringView("a\0b", 3)));
}

TEST_CASE("the null view equals the empty string") {
    CHECK(PrStringView{} == "");
    CHECK("" == PrStringView{});
}

TEST_CASE("a view iterates its bytes") {
    String out;
    for (char c : PrStringView("abc"))
        out.append(c);
    CHECK(out == "abc");

    size_t count = 0;
    for ([[maybe_unused]] char c : PrStringView{})
        ++count;
    CHECK(count == 0);
}
