#include "pr/string.h"
#include "pr/writer.h"

#include "tools/harness/test.h"

namespace pr_impl {
namespace {

// A writer that drains into a String and counts its drains, failing each drain once `failAfter` have passed.
struct CountingWriter : Writer {
    String text;
    uint32_t drains = 0;
    uint32_t failAfter = 0xffffffffu;
    char storage[4];

    static bool take(void* PR_NULLABLE context, PrStringView text) {
        CountingWriter* counting = static_cast<CountingWriter*>(context);
        if (counting->drains == counting->failAfter)
            return false;
        counting->text.append(text);
        ++counting->drains;
        return true;
    }

    CountingWriter() : Writer{.drain = take, .context = this, .buffer = storage, .capacity = sizeof(storage)} {}
};

} // namespace
} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a buffer writer keeps what fits, and reports that the rest was dropped") {
    char buffer[8];
    Writer writer = bufferWriter(Span(buffer));
    writer.write("0123");
    CHECK(!writer.truncated);
    CHECK(writer.flush());
    writer.write("456789");
    CHECK(writer.truncated);
    CHECK(writer.view() == "01234567");
    CHECK(!writer.flush());
    writer.write("more");
    CHECK(writer.view() == "01234567");
}

TEST_CASE("a buffer writer drops a UTF-8 sequence whole rather than cut it") {
    char buffer[6];
    Writer two = bufferWriter(Span(buffer));
    two.write("abcde\xc3\xa9");
    CHECK(two.view() == "abcde");
    Writer three = bufferWriter(Span(buffer));
    three.write("abcd\xe2\x82\xac");
    CHECK(three.view() == "abcd");
    Writer four = bufferWriter(Span(buffer));
    four.write("ab\xf0\x9f\x98\x80z");
    CHECK(four.view() == "ab\xf0\x9f\x98\x80");
    Writer whole = bufferWriter(Span(buffer));
    whole.write("a\xc3\xa9");
    whole.write("\xc3\xa9\xc3\xa9");
    CHECK(whole.view() == "a\xc3\xa9\xc3\xa9");
    CHECK(whole.truncated);
}

TEST_CASE("a writer drains its buffer to its context when it fills, and a large write goes straight through") {
    CountingWriter writer;
    writer.write("ab");
    writer.write("cd");
    CHECK(writer.drains == 0);
    writer.write("e");
    CHECK(writer.drains == 1);
    writer.write("a write larger than the buffer");
    writer.writeRepeated('.', 6);
    CHECK(writer.flush());
    CHECK(writer.text == "abcdea write larger than the buffer......");
}

TEST_CASE("a drain that fails marks the writer truncated") {
    CountingWriter writer;
    writer.failAfter = 1;
    writer.write("abcd");
    writer.write("efgh");
    CHECK(!writer.truncated);
    writer.write("ijkl");
    CHECK(writer.truncated);
    CHECK(!writer.flush());
    CHECK(writer.text == "abcd");
}

TEST_CASE("padded text is aligned within its width by its fill") {
    char buffer[32];
    Writer writer = bufferWriter(Span(buffer));
    writer.writePadded("ab", 5, '.', '<');
    writer.writeChar('|');
    writer.writePadded("ab", 5, '.', '^');
    writer.writeChar('|');
    writer.writePadded("ab", 5, '.', '>');
    writer.writeChar('|');
    writer.writePadded("wider", 2, '.', '>');
    CHECK(writer.view() == "ab...|.ab..|...ab|wider");
}

TEST_CASE("a string writer appends to its string when it is flushed or goes") {
    String out = "start ";
    {
        StringWriter writer(&out);
        writer.write("buffered");
        CHECK(out == "start ");
        writer.writeRepeated('!', 300);
        CHECK(out.count() > 6);
    }
    CHECK(out.count() == 6 + 8 + 300);
}
