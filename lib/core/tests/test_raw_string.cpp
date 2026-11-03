#include "pr/raw_string.h"

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
    PrRawString string = {};
    CHECK(prRawStringKind(&string) == PrRawStringKind_External);
    CHECK(prRawStringIsNull(&string));
    CHECK(prRawStringView(&string).size == 0);
    CHECK(prRawStringView(&string).data == nullptr);

    prRawStringReset(&string);
    CHECK(prRawStringIsNull(&string));
}

TEST_CASE("a short copy stays inline") {
    PrRawString string = prRawStringCopy("color");
    CHECK(prRawStringKind(&string) == PrRawStringKind_Inline);
    CHECK(prRawStringView(&string) == "color");

    // Inline text lives in the string, not where it was copied from.
    const char* text = prRawStringView(&string).data;
    CHECK(text == string.small.text);
    prRawStringReset(&string);
}

TEST_CASE("a copy holds its bytes at every inline size") {
    for (size_t size = 0; size <= kPrRawStringInlineCapacity; ++size) {
        CAPTURE(size);
        PrRawString string = prRawStringCopy(PrStringView{pattern(size), size});
        CHECK(prRawStringKind(&string) == PrRawStringKind_Inline);
        CHECK(prRawStringView(&string) == PrStringView{pattern(size), size});
        prRawStringReset(&string);
    }
}

TEST_CASE("one byte past the inline capacity goes to the heap") {
    size_t size = kPrRawStringInlineCapacity + 1;
    PrRawString string = prRawStringCopy(PrStringView{pattern(size), size});
    CHECK(prRawStringKind(&string) == PrRawStringKind_Owned);
    CHECK(prRawStringView(&string) == PrStringView{pattern(size), size});
    CHECK(string.heap.capacity == size);
    prRawStringReset(&string);
}

TEST_CASE("a long copy owns its text") {
    PrRawString string = prRawStringCopy(PrStringView{pattern(3000), 3000});
    CHECK(prRawStringKind(&string) == PrRawStringKind_Owned);
    CHECK(prRawStringView(&string) == PrStringView{pattern(3000), 3000});
    prRawStringReset(&string);
    CHECK(prRawStringIsNull(&string));
    CHECK(prRawStringView(&string).size == 0);
}

TEST_CASE("an external string copies nothing") {
    const char* text = "a very long external value that would not fit inline at all";
    PrRawString string = prRawStringExternal(text);
    CHECK(prRawStringKind(&string) == PrRawStringKind_External);

    const char* bytes = prRawStringView(&string).data;
    CHECK(bytes == text);
    CHECK(prRawStringView(&string).size == strlen(text));
    prRawStringReset(&string);
    CHECK(prRawStringIsNull(&string));
}

TEST_CASE("a short external string stays external") {
    // An external string never copies, however small the text is.
    PrRawString string = prRawStringExternal("red");
    CHECK(prRawStringKind(&string) == PrRawStringKind_External);
    CHECK(prRawStringView(&string) == "red");
    prRawStringReset(&string);
}

TEST_CASE("reset is safe twice, on every kind") {
    PrRawString strings[3] = {
        prRawStringCopy("inline"),
        prRawStringCopy(PrStringView{pattern(100), 100}),
        prRawStringExternal("external"),
    };
    for (PrRawString& string : strings) {
        prRawStringReset(&string);
        prRawStringReset(&string);
        CHECK(prRawStringIsNull(&string));
        CHECK(prRawStringView(&string).size == 0);
    }
}

TEST_CASE("an empty view is empty, not null, copied or external") {
    PrRawString copied = prRawStringCopy("");
    CHECK(prRawStringKind(&copied) == PrRawStringKind_Inline);
    CHECK(prRawStringView(&copied).size == 0);
    CHECK_FALSE(prRawStringIsNull(&copied));
    prRawStringReset(&copied);

    PrRawString external = prRawStringExternal("");
    CHECK(prRawStringView(&external).size == 0);
    CHECK_FALSE(prRawStringIsNull(&external));
    prRawStringReset(&external);
}

TEST_CASE("text is bytes, not C strings") {
    // A NUL inside the text is part of it, and the view carries the whole thing.
    const char embedded[] = {'a', '\0', 'b'};
    PrRawString string = prRawStringCopy(PrStringView{embedded, sizeof(embedded)});
    CHECK(prRawStringView(&string) == PrStringView{embedded, sizeof(embedded)});
    CHECK(prRawStringView(&string).size == sizeof(embedded));
    prRawStringReset(&string);
}

TEST_CASE("UTF-8 passes through unmodified") {
    PrRawString string = prRawStringCopy("\xe2\x88\x9e px");   // an infinity sign
    CHECK(prRawStringView(&string) == "\xe2\x88\x9e px");
    prRawStringReset(&string);
}

TEST_CASE("a NUL-terminated string views its bytes without the terminator") {
    PrStringView sv = "background-color";
    CHECK(sv.size == 16);
    CHECK(sv.data[16] == '\0');
    CHECK(memcmp(sv.data, "background-color", sv.size) == 0);
}

TEST_CASE("the null string is not the empty string") {
    PrRawString null = prRawStringNull();
    PrRawString empty = prRawStringCopy("");

    CHECK(prRawStringIsNull(&null));
    CHECK_FALSE(prRawStringIsNull(&empty));

    // Both are zero bytes long. They differ only in whether there is a pointer at all, and a reader bounded by the size
    // never looks at it.
    CHECK(prRawStringIsEmpty(&null));
    CHECK(prRawStringIsEmpty(&empty));
    CHECK(prRawStringView(&null).data == nullptr);
    CHECK(prRawStringView(&empty).data != nullptr);

    prRawStringReset(&null);
    CHECK(prRawStringIsNull(&null));    // reset leaves the null string, which is also what a zeroed string is
    prRawStringReset(&empty);
    CHECK(prRawStringIsNull(&empty));
}

TEST_CASE("the null view is the null string, copied or external") {
    PrStringView none = {nullptr, 0};

    PrRawString copied = prRawStringCopy(none);
    PrRawString external = prRawStringExternal(none);
    CHECK(prRawStringIsNull(&copied));
    CHECK(prRawStringIsNull(&external));

    PrRawString again = prRawStringClone(&copied);
    CHECK(prRawStringIsNull(&again));

    prRawStringReset(&copied);
    prRawStringReset(&external);
    prRawStringReset(&again);
}

TEST_CASE("the empty string is not the null string") {
    PrRawString empty = prRawStringEmpty();
    CHECK(prRawStringIsEmpty(&empty));
    CHECK_FALSE(prRawStringIsNull(&empty));
    CHECK(prRawStringKind(&empty) == PrRawStringKind_Inline);
    prRawStringReset(&empty);
}

TEST_CASE("a clone owns its bytes, whatever it was cloned from") {
    const char* text = "a string longer than the thirty bytes that fit inline";
    PrRawString external = prRawStringExternal(text);
    PrRawString owned = prRawStringClone(&external);
    CHECK(prRawStringKind(&owned) == PrRawStringKind_Owned);
    CHECK(prRawStringView(&owned).data != text);
    CHECK(prRawStringView(&owned) == PrStringView{text, strlen(text)});

    PrRawString inline_ = prRawStringClone(&owned);
    prRawStringReset(&owned);
    CHECK(prRawStringView(&inline_).size == strlen(text));

    prRawStringReset(&external);
    prRawStringReset(&inline_);
}

TEST_CASE("replacing a string releases what it held") {
    PrRawString string = prRawStringCopy("a value longer than the inline capacity");
    prRawStringReplace(&string, "short");
    CHECK(prRawStringView(&string) == "short");
    prRawStringReplace(&string, PrStringView{});
    CHECK(prRawStringIsNull(&string));
    prRawStringReset(&string);
}

TEST_CASE("append keeps a string inline while it fits") {
    PrRawString string = prRawStringCopy("ab");
    prRawStringAppend(&string, "cd");
    CHECK(prRawStringKind(&string) == PrRawStringKind_Inline);
    CHECK(prRawStringView(&string) == "abcd");
    prRawStringReset(&string);
}

TEST_CASE("append moves a string to the heap when it outgrows the inline bytes") {
    PrRawString string = prRawStringCopy("0123456789012345678901234567");
    REQUIRE(prRawStringKind(&string) == PrRawStringKind_Inline);

    prRawStringAppend(&string, "abcde");
    CHECK(prRawStringKind(&string) == PrRawStringKind_Owned);
    CHECK(prRawStringView(&string) == "0123456789012345678901234567abcde");
    prRawStringReset(&string);
}

TEST_CASE("append copies an external string before adding to it") {
    const char* bytes = "abc";
    PrRawString string = prRawStringExternal(bytes);
    prRawStringAppend(&string, "de");
    CHECK(prRawStringKind(&string) != PrRawStringKind_External);
    CHECK(prRawStringView(&string) == "abcde");
    prRawStringReset(&string);
}

TEST_CASE("append makes a null string the text") {
    PrRawString string = prRawStringNull();
    prRawStringAppend(&string, "ab");
    CHECK(!prRawStringIsNull(&string));
    CHECK(prRawStringView(&string) == "ab");
    prRawStringReset(&string);
}

TEST_CASE("append doubles the capacity, so a string built a byte at a time copies each once") {
    PrRawString string = prRawStringNull();
    uint32_t grown = 0;
    uint32_t capacity = 0;
    for (uint32_t i = 0; i < 5000; ++i) {
        char byte = char('a' + i % 26);
        prRawStringAppend(&string, PrStringView{&byte, 1});
        if (prRawStringKind(&string) == PrRawStringKind_Owned && string.heap.capacity != capacity) {
            capacity = string.heap.capacity;
            ++grown;
        }
    }

    PrStringView text = prRawStringView(&string);
    REQUIRE(text.size == 5000);
    for (uint32_t i = 0; i < 5000; ++i)
        REQUIRE(text.data[i] == char('a' + i % 26));

    // Doubling from the inline bytes reaches 5000 in a dozen or so growths, not 5000.
    CHECK(grown < 20);
    prRawStringReset(&string);
}

TEST_CASE("string replacement can copy a view of its current storage") {
    for (const char* text : {"short", "a value longer than the inline string capacity"}) {
        PrRawString string = prRawStringCopy(text);
        PrStringView original = prRawStringView(&string);
        prRawStringReplace(&string, {original.data + 1, original.size - 1});
        CHECK(prRawStringView(&string) == PrStringView{text + 1, strlen(text) - 1});
        prRawStringReplace(&string, prRawStringView(&string));
        CHECK(prRawStringView(&string) == PrStringView{text + 1, strlen(text) - 1});
        prRawStringReset(&string);
    }
}
