// Prax Merge's model: its command line, the merge of files that starts a result, and the conflicts that a file marks.

#include "pr/file.h"
#include "pr/string.h"

#include "apps/merge/merge.h"

#include "tools/harness/test.h"

using namespace merge;

namespace {

// Writes `text` to a file of `name` in a folder of the tests' own, and returns its path.
String writeFile(PrStringView name, PrStringView text) {
    PrRawString cache = prFileCacheDirectory();
    String folder = format("{}/prax-tests/merge", prRawStringView(&cache));
    prRawStringReset(&cache);
    REQUIRE(prFileCreateDirectory(folder));
    String path = format("{}/{}", folder, name);
    REQUIRE(prFileWrite(path, PrByteView{reinterpret_cast<const uint8_t*>(text.data), text.size}));
    return path;
}

template <uint32_t N>
bool parsed(const PrStringView (&arguments)[N], Invocation* out) {
    return parseArguments(Span<const PrStringView>(arguments), out);
}

} // namespace

TEST_CASE("merge: two paths compare, four merge, and anything else is refused") {
    Invocation compare;
    REQUIRE(parsed({"old.c", "new.c"}, &compare));
    CHECK(compare.task == Task_Compare);
    CHECK(compare.old == "old.c");
    CHECK(compare.young == "new.c");
    Invocation merge;
    REQUIRE(parsed({"--trace", "base.c", "local.c", "remote.c", "merged.c"}, &merge));
    CHECK(merge.task == Task_Merge);
    CHECK(merge.trace);
    CHECK(merge.base == "base.c");
    CHECK(merge.left == "local.c");
    CHECK(merge.right == "remote.c");
    CHECK(merge.result == "merged.c");
    Invocation refused;
    CHECK_FALSE(parsed({"one"}, &refused));
    CHECK_FALSE(parsed({"a", "b", "c"}, &refused));
    CHECK_FALSE(parsed({"--nope", "a", "b"}, &refused));
}

TEST_CASE("merge: files merge into a result whose conflicts are labeled with their names") {
    String base = writeFile("base.txt", "a\nb\nc\n");
    String left = writeFile("left.txt", "A\nb\nleft\n");
    String right = writeFile("right.txt", "a\nb\nright\n");
    MergedText merged;
    REQUIRE(mergeFiles(base, left, right, &merged));
    CHECK(merged.conflicts == 1);
    CHECK(merged.text == "A\nb\n<<<<<<< left.txt\nleft\n||||||| base.txt\nc\n=======\nright\n>>>>>>> right.txt\n");
    String result = writeFile("result.txt", merged.text);
    CHECK(fileConflicts(result) == 1);
    writeFile("result.txt", "A\nb\nleft\n");
    CHECK(fileConflicts(result) == 0);
    CHECK(fileConflicts("/prax/no/such/file") == -1);
    CHECK_FALSE(mergeFiles("/prax/no/such/file", left, right, &merged));
}

TEST_CASE("merge: a result ends its lines as the left side does") {
    String base = writeFile("crlf_base.txt", "a\r\nb\r\nc\r\n");
    String left = writeFile("crlf_left.txt", "a\r\nb\r\nC\r\n");
    String right = writeFile("crlf_right.txt", "x\r\nb\r\nc\r\n");
    MergedText merged;
    REQUIRE(mergeFiles(base, left, right, &merged));
    CHECK(merged.conflicts == 0);
    CHECK(merged.text == "x\r\nb\r\nC\r\n");
}
