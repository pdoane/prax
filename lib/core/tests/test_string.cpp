#include "pr/string.h"

#include "doctest/doctest.h"

#include <initializer_list>
#include <string.h>

namespace pr_impl {

namespace {

// Returns a run of `size` distinct bytes, so that a truncated or shifted copy is visible.
const char* pattern(size_t size) {
    static char buffer[4096];
    for (size_t i = 0; i < size; ++i)
        buffer[i] = char('a' + (i % 26));
    buffer[size] = '\0';
    return buffer;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a zeroed string is the null string, as a zeroed view is the null view") {
    PrString string = {};
    CHECK(prStringKind(&string) == PrStringKind_External);
    CHECK(prStringIsNull(&string));
    CHECK(prStringView(&string).size == 0);
    CHECK(prStringView(&string).data == nullptr);

    prStringReset(&string);
    CHECK(prStringIsNull(&string));
}

TEST_CASE("a short copy stays inline") {
    PrString string = prStringCopy("color");
    CHECK(prStringKind(&string) == PrStringKind_Inline);
    CHECK(prStringView(&string) == "color");

    // Inline text lives in the string, not where it was copied from.
    const char* text = prStringView(&string).data;
    CHECK(text == string.small.text);
    prStringReset(&string);
}

TEST_CASE("a copy holds its bytes at every inline size") {
    for (size_t size = 0; size <= kPrStringInlineCapacity; ++size) {
        CAPTURE(size);
        PrString string = prStringCopy(PrStringView{pattern(size), size});
        CHECK(prStringKind(&string) == PrStringKind_Inline);
        CHECK(prStringView(&string) == PrStringView{pattern(size), size});
        prStringReset(&string);
    }
}

TEST_CASE("one byte past the inline capacity goes to the heap") {
    size_t size = kPrStringInlineCapacity + 1;
    PrString string = prStringCopy(PrStringView{pattern(size), size});
    CHECK(prStringKind(&string) == PrStringKind_Owned);
    CHECK(prStringView(&string) == PrStringView{pattern(size), size});
    CHECK(string.heap.capacity == size);
    prStringReset(&string);
}

TEST_CASE("a long copy owns its text") {
    PrString string = prStringCopy(PrStringView{pattern(3000), 3000});
    CHECK(prStringKind(&string) == PrStringKind_Owned);
    CHECK(prStringView(&string) == PrStringView{pattern(3000), 3000});
    prStringReset(&string);
    CHECK(prStringIsNull(&string));
    CHECK(prStringView(&string).size == 0);
}

TEST_CASE("an external string copies nothing") {
    const char* text = "a very long external value that would not fit inline at all";
    PrString string = prStringExternal(text);
    CHECK(prStringKind(&string) == PrStringKind_External);

    const char* bytes = prStringView(&string).data;
    CHECK(bytes == text);
    CHECK(prStringView(&string).size == strlen(text));
    prStringReset(&string);
    CHECK(prStringIsNull(&string));
}

TEST_CASE("a short external string stays external") {
    // An external string never copies, however small the text is.
    PrString string = prStringExternal("red");
    CHECK(prStringKind(&string) == PrStringKind_External);
    CHECK(prStringView(&string) == "red");
    prStringReset(&string);
}

TEST_CASE("reset is safe twice, on every kind") {
    PrString strings[3] = {
        prStringCopy("inline"),
        prStringCopy(PrStringView{pattern(100), 100}),
        prStringExternal("external"),
    };
    for (PrString& string : strings) {
        prStringReset(&string);
        prStringReset(&string);
        CHECK(prStringIsNull(&string));
        CHECK(prStringView(&string).size == 0);
    }
}

TEST_CASE("an empty view is empty, not null, copied or external") {
    PrString copied = prStringCopy("");
    CHECK(prStringKind(&copied) == PrStringKind_Inline);
    CHECK(prStringView(&copied).size == 0);
    CHECK_FALSE(prStringIsNull(&copied));
    prStringReset(&copied);

    PrString external = prStringExternal("");
    CHECK(prStringView(&external).size == 0);
    CHECK_FALSE(prStringIsNull(&external));
    prStringReset(&external);
}

TEST_CASE("text is bytes, not C strings") {
    // A NUL inside the text is part of it, and the view carries the whole thing.
    const char embedded[] = {'a', '\0', 'b'};
    PrString string = prStringCopy(PrStringView{embedded, sizeof(embedded)});
    CHECK(prStringView(&string) == PrStringView{embedded, sizeof(embedded)});
    CHECK(prStringView(&string).size == sizeof(embedded));
    prStringReset(&string);
}

TEST_CASE("UTF-8 passes through unmodified") {
    PrString string = prStringCopy("\xe2\x88\x9e px");   // an infinity sign
    CHECK(prStringView(&string) == "\xe2\x88\x9e px");
    prStringReset(&string);
}

TEST_CASE("a NUL-terminated string views its bytes without the terminator") {
    PrStringView sv = "background-color";
    CHECK(sv.size == 16);
    CHECK(sv.data[16] == '\0');
    CHECK(memcmp(sv.data, "background-color", sv.size) == 0);
}

TEST_CASE("the null string is not the empty string") {
    PrString null = prStringNull();
    PrString empty = prStringCopy("");

    CHECK(prStringIsNull(&null));
    CHECK_FALSE(prStringIsNull(&empty));

    // Both are zero bytes long. They differ only in whether there is a pointer at all, and a reader bounded by the size
    // never looks at it.
    CHECK(prStringIsEmpty(&null));
    CHECK(prStringIsEmpty(&empty));
    CHECK(prStringView(&null).data == nullptr);
    CHECK(prStringView(&empty).data != nullptr);

    prStringReset(&null);
    CHECK(prStringIsNull(&null));    // reset leaves the null string, which is also what a zeroed string is
    prStringReset(&empty);
    CHECK(prStringIsNull(&empty));
}

TEST_CASE("the null view is the null string, copied or external") {
    PrStringView none = {nullptr, 0};

    PrString copied = prStringCopy(none);
    PrString external = prStringExternal(none);
    CHECK(prStringIsNull(&copied));
    CHECK(prStringIsNull(&external));

    PrString again = prStringClone(&copied);
    CHECK(prStringIsNull(&again));

    prStringReset(&copied);
    prStringReset(&external);
    prStringReset(&again);
}

TEST_CASE("the empty string is not the null string") {
    PrString empty = prStringEmpty();
    CHECK(prStringIsEmpty(&empty));
    CHECK_FALSE(prStringIsNull(&empty));
    CHECK(prStringKind(&empty) == PrStringKind_Inline);
    prStringReset(&empty);
}

TEST_CASE("a clone owns its bytes, whatever it was cloned from") {
    const char* text = "a string longer than the thirty bytes that fit inline";
    PrString external = prStringExternal(text);
    PrString owned = prStringClone(&external);
    CHECK(prStringKind(&owned) == PrStringKind_Owned);
    CHECK(prStringView(&owned).data != text);
    CHECK(prStringView(&owned) == PrStringView{text, strlen(text)});

    PrString inline_ = prStringClone(&owned);
    prStringReset(&owned);
    CHECK(prStringView(&inline_).size == strlen(text));

    prStringReset(&external);
    prStringReset(&inline_);
}

TEST_CASE("replacing a string releases what it held") {
    PrString string = prStringCopy("a value longer than the inline capacity");
    prStringReplace(&string, "short");
    CHECK(prStringView(&string) == "short");
    prStringReplace(&string, PrStringView{});
    CHECK(prStringIsNull(&string));
    prStringReset(&string);
}

TEST_CASE("append keeps a string inline while it fits") {
    PrString string = prStringCopy("ab");
    prStringAppend(&string, "cd");
    CHECK(prStringKind(&string) == PrStringKind_Inline);
    CHECK(prStringView(&string) == "abcd");
    prStringReset(&string);
}

TEST_CASE("append moves a string to the heap when it outgrows the inline bytes") {
    PrString string = prStringCopy("0123456789012345678901234567");
    REQUIRE(prStringKind(&string) == PrStringKind_Inline);

    prStringAppend(&string, "abcde");
    CHECK(prStringKind(&string) == PrStringKind_Owned);
    CHECK(prStringView(&string) == "0123456789012345678901234567abcde");
    prStringReset(&string);
}

TEST_CASE("append copies an external string before adding to it") {
    const char* bytes = "abc";
    PrString string = prStringExternal(bytes);
    prStringAppend(&string, "de");
    CHECK(prStringKind(&string) != PrStringKind_External);
    CHECK(prStringView(&string) == "abcde");
    prStringReset(&string);
}

TEST_CASE("append makes a null string the text") {
    PrString string = prStringNull();
    prStringAppend(&string, "ab");
    CHECK(!prStringIsNull(&string));
    CHECK(prStringView(&string) == "ab");
    prStringReset(&string);
}

TEST_CASE("append doubles the capacity, so a string built a byte at a time copies each once") {
    PrString string = prStringNull();
    uint32_t grown = 0;
    uint32_t capacity = 0;
    for (uint32_t i = 0; i < 5000; ++i) {
        char byte = char('a' + i % 26);
        prStringAppend(&string, PrStringView{&byte, 1});
        if (prStringKind(&string) == PrStringKind_Owned && string.heap.capacity != capacity) {
            capacity = string.heap.capacity;
            ++grown;
        }
    }

    PrStringView text = prStringView(&string);
    REQUIRE(text.size == 5000);
    for (uint32_t i = 0; i < 5000; ++i)
        REQUIRE(text.data[i] == char('a' + i % 26));

    // Doubling from the inline bytes reaches 5000 in a dozen or so growths, not 5000.
    CHECK(grown < 20);
    prStringReset(&string);
}

TEST_CASE("string replacement can copy a view of its current storage") {
    for (const char* text : {"short", "a value longer than the inline string capacity"}) {
        PrString string = prStringCopy(text);
        PrStringView original = prStringView(&string);
        prStringReplace(&string, {original.data + 1, original.size - 1});
        CHECK(prStringView(&string) == PrStringView{text + 1, strlen(text) - 1});
        prStringReplace(&string, prStringView(&string));
        CHECK(prStringView(&string) == PrStringView{text + 1, strlen(text) - 1});
        prStringReset(&string);
    }
}
