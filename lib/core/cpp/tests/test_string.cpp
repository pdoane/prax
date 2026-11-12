#include "pr/string.h"

#include "tools/harness/test.h"

using namespace pr_impl;

TEST_CASE("a string copies, moves and compares its text") {
    String empty;
    CHECK(empty.isEmpty());
    CHECK(empty.data() == nullptr);
    String a("short");
    String b = a;
    CHECK(a == b);
    CHECK(b == "short");
    String c = move(b);
    CHECK(c == "short");
    CHECK(b.isEmpty());
    // A string longer than fits inline lives on the heap, which a copy duplicates.
    String longer("a string longer than the thirty-one bytes held inline");
    String copy = longer;
    CHECK(copy.data() != longer.data());
    CHECK(copy == longer);
}

TEST_CASE("a string appends, including its own text, and converts to a view") {
    String s("ab");
    s.append("cd");
    s.append(s);
    CHECK(s == "abcdabcd");
    s.append('!');
    PrStringView view = s;
    CHECK(view.size == 9);
    s = "x";
    CHECK(s == "x");
    s = PrStringView{s.data(), 0};
    CHECK(s.isEmpty());
}

TEST_CASE("clearing a string keeps its storage, and resetting frees it") {
    String s("a string longer than the thirty-one bytes held inline");
    const char* storage = s.data();
    s.clear();
    CHECK(s.isEmpty());
    s.append("again");
    CHECK(s.data() == storage);
    s = "a shorter string, still past the inline bytes";
    CHECK(s.data() == storage);
    PrStringView view = "another string that is still past the inline bytes";
    s = view;
    CHECK(s.data() == storage);
    s.reset();
    CHECK(s.data() == nullptr);
}

TEST_CASE("a string adopts and releases a raw string, and hashes as its view") {
    String s = String::adopt(prRawStringCopy("owned"));
    CHECK(s == "owned");
    PrRawString raw = s.release();
    CHECK(s.isEmpty());
    CHECK(prRawStringView(&raw) == "owned");
    prRawStringReset(&raw);
    PrStringView key = "key";
    CHECK(hashKey(String(key)) == hashKey(key));
}

TEST_CASE("a failed check prints text in quotes, and bytes in hexadecimal") {
    PrStringView text = "ab";
    PrStringView null = {nullptr, 0};
    CHECK(doctest::toString(text) == "\"ab\"");
    CHECK(doctest::toString(null) == "{null string}");
    CHECK(doctest::toString(String(text)) == "\"ab\"");
    uint8_t bytes[] = {1, 0xab};
    CHECK(doctest::toString(PrByteView(bytes)) == "{01 ab} (2 bytes)");
}

TEST_CASE("a string keeps the null view apart from the empty view") {
    String none;
    CHECK(none.isNull());
    CHECK(none.isEmpty());
    String fromNull = PrStringView{nullptr, 0};
    CHECK(fromNull.isNull());
    String empty = "";
    CHECK(!empty.isNull());
    CHECK(empty.isEmpty());
    String copy = empty;
    CHECK(!copy.isNull());
    String moved = move(copy);
    CHECK(!moved.isNull());
    CHECK(copy.isNull());
    String text = "text";
    text.assign(PrStringView{nullptr, 0});
    CHECK(text.isNull());
    text = "again";
    text.clear();
    CHECK(!text.isNull());
    CHECK(text.isEmpty());
    text.reset();
    CHECK(text.isNull());
}

TEST_CASE("text converts to a string where a string is taken") {
    auto length = [](const String& text) { return text.count(); };
    CHECK(length("four") == 4);
    PrStringView view = "view";
    String s = view;
    CHECK(s == view);
    s = "replaced";
    CHECK(s == "replaced");
    // Assigning the string's own text keeps it.
    s = s.view();
    CHECK(s == "replaced");
    s.assign(PrStringView{s.data() + 2, 3});
    CHECK(s == "pla");
}

TEST_CASE("a string replaces a range of its bytes, with any text including its own") {
    String s = "hello world";
    s.replace(0, 5, "goodbye");
    CHECK(s == "goodbye world");
    s.replace(s.count(), 0, "!");
    CHECK(s == "goodbye world!");
    s.replace(4, 3, "");
    CHECK(s == "good world!");
    s.replace(0, 4, s);
    CHECK(s == "good world! world!");
}

TEST_CASE("a run of one byte, the last byte, and texts joined") {
    String s("a");
    s.append(3, '-');
    CHECK(s == "a---");
    s.append(200, 'x');
    CHECK(s.count() == 204);
    CHECK(s.last() == 'x');
    s.append(0, 'y');
    CHECK(s.count() == 204);
    String name("b{}");
    CHECK(concat("dir", "/", name, "/c") == "dir/b{}/c");
    CHECK(concat().isEmpty());
}
