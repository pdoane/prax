#include "pr/ascii.h"
#include "pr/name_table.h"

#include "doctest/doctest.h"

namespace pr_impl {

namespace {

constexpr uint64_t kSeed = 0x9e3779b97f4a7c15ull;

static_assert(bitCast<uint32_t>(1.0f) == 0x3f800000u);
static_assert(bitCast<float>(0x3f800000u) == 1.0f);
static_assert(loadUnaligned<uint64_t>(&"!abcdefgh"[1]) == 0x6867666564636261ull);
static_assert(nameWordPart("abcdefg", 7) == 0x67666564636261ull);
static_assert(nameHashLower("STYLE", 5, kSeed) == nameHash("style", 5, kSeed));

// Returns the bytes as a word, low byte first.
uint64_t wordOf(const char* bytes, size_t size) {
    uint64_t word = 0;
    for (size_t i = 0; i < size; ++i)
        word |= uint64_t(uint8_t(bytes[i])) << (8 * i);
    return word;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("constant and runtime name hashes agree across word boundaries") {
    constexpr char kText[] = "!CounterClockwiseContourIntegral;";
    constexpr uint64_t kHashes[] = {
        nameHash(kText + 1, 0, kSeed), nameHash(kText + 1, 4, kSeed),
        nameHash(kText + 1, 7, kSeed), nameHash(kText + 1, 8, kSeed),
        nameHash(kText + 1, 9, kSeed), nameHash(kText + 1, 32, kSeed),
    };
    constexpr size_t kSizes[] = {0, 4, 7, 8, 9, 32};
    char runtime[sizeof(kText)];
    for (size_t i = 0; i < sizeof(kText); ++i)
        runtime[i] = kText[i];
    for (size_t i = 0; i < 6; ++i)
        CHECK(nameHash(runtime + 1, kSizes[i], kSeed) == kHashes[i]);
    constexpr uint64_t kFolded = nameHashLower(kText + 1, 32, kSeed);
    CHECK(nameHashLower(runtime + 1, 32, kSeed) == kFolded);
}

TEST_CASE("a folded word is every ASCII uppercase byte lowered, and nothing else") {
    for (uint32_t byte = 0; byte < 256; ++byte) {
        char text[8] = {char(byte), char(byte), char(byte), char(byte), char(byte), char(byte), char(byte), char(byte)};
        char lowered[8];
        for (uint32_t i = 0; i < 8; ++i)
            lowered[i] = asciiLower(text[i]);
        INFO("byte ", byte);
        CHECK(nameFold(wordOf(text, 8)) == wordOf(lowered, 8));
    }
}

TEST_CASE("a part word holds the bytes below eight and nothing above them") {
    const char* text = "abcdefg";
    for (size_t size = 0; size < 8; ++size) {
        INFO("size ", size);
        CHECK(nameWordPart(text, size) == wordOf(text, size));
    }
}

TEST_CASE("a part word reads no byte past the size it was given") {
    // The name is at the end of its buffer, so a read past it would go out of bounds.
    char buffer[16] = {};
    for (size_t size = 1; size < 8; ++size) {
        char* name = buffer + sizeof(buffer) - size;
        for (size_t i = 0; i < size; ++i)
            name[i] = char('a' + i);
        CHECK(nameWordPart(name, size) == wordOf(name, size));
    }
}

TEST_CASE("a mask covers the low bytes of a size and no others") {
    CHECK(nameWordMask(0) == 0);
    CHECK(nameWordMask(1) == 0xffull);
    CHECK(nameWordMask(7) == 0x00ffffffffffffffull);
    CHECK(nameWordMask(8) == ~uint64_t(0));
}

TEST_CASE("a hash is the same however the name is read") {
    // The two paths meet at eight bytes, where the partial read gives way to the word read.
    const char* names[] = {
        "", "a", "div", "quot;", "abcdefg", "abcdefgh", "abcdefghi",
        "CounterClockwiseContourIntegral;",
    };
    for (const char* name : names) {
        size_t size = PrStringView(name).size;
        uint64_t word;
        uint64_t hash = nameHash(name, size, kSeed, &word);
        INFO(name);
        CHECK(hash == nameHash(name, size, kSeed));
        CHECK(word == wordOf(name, size < 8 ? size : 8));
    }
}

TEST_CASE("a folded hash is the same whatever case the name was written in") {
    uint64_t word;
    CHECK(nameHashLower("DIV", 3, kSeed, &word) == nameHashLower("div", 3, kSeed, &word));
    CHECK(nameHashLower("Border-Top-Left-Radius", 22, kSeed, &word) ==
        nameHashLower("border-top-left-radius", 22, kSeed, &word));
}

TEST_CASE("a name the hash reached is compared against the text") {
    // A word can be read at any name in the blob, regardless of its length.
    const char blob[] = "a" "div" "textPath" "annotation-xml" "\0\0\0\0\0\0\0\0";
    PrStringView a = {blob, 1};
    PrStringView div = {blob + 1, 3};
    PrStringView path = {blob + 4, 8};
    PrStringView annotation = {blob + 12, 14};

    uint64_t word;
    nameHash("div", 3, kSeed, &word);
    CHECK(nameEquals(div, "div", word));
    CHECK(!nameEquals(a, "div", word));
    CHECK(!nameEquals(path, "div", word));

    nameHash("textPath", 8, kSeed, &word);
    CHECK(nameEquals(path, "textPath", word));

    nameHash("annotation-xml", 14, kSeed, &word);
    CHECK(nameEquals(annotation, "annotation-xml", word));
    CHECK(!nameEquals(annotation, "annotation-xmm", word));
}

TEST_CASE("a folded comparison answers what asciiEqualsFold answers") {
    // '\r' is '-' with 0x20 set, and the controls are the digits with it set. A fold that set the bit over the whole
    // word, rather than only its uppercase bytes, would match these.
    const char blob[] = "font-face" "h1" "\0\0\0\0\0\0\0\0";
    PrStringView face = {blob, 9};
    PrStringView h1 = {blob + 9, 2};

    uint64_t word;
    nameHashLower("FONT-FACE", 9, kSeed, &word);
    CHECK(nameEqualsLower(face, "FONT-FACE", word));

    nameHashLower("font\rface", 9, kSeed, &word);
    CHECK(!nameEqualsLower(face, "font\rface", word));
    CHECK(!asciiEqualsFold("font\rface", face));

    nameHashLower("h\x11", 2, kSeed, &word);
    CHECK(!nameEqualsLower(h1, "h\x11", word));
    CHECK(!asciiEqualsFold("h\x11", h1));

    nameHashLower("H1", 2, kSeed, &word);
    CHECK(nameEqualsLower(h1, "H1", word));
}

TEST_CASE("a text of another length is not the name") {
    const char blob[] = "div" "\0\0\0\0\0\0\0\0";
    PrStringView div = {blob, 3};
    uint64_t word;
    nameHash("di", 2, kSeed, &word);
    CHECK(!nameEquals(div, "di", word));
    nameHash("divs", 4, kSeed, &word);
    CHECK(!nameEquals(div, "divs", word));
}

TEST_CASE("a blob hands back the text an index names") {
    const char blob[] = "a" "div" "span" "\0\0\0\0\0\0\0\0";
    const uint8_t at[] = {0, 1, 4, 8};
    CHECK(nameAt(blob, at, 0).size == 1);
    CHECK(nameAt(blob, at, 1).data == blob + 1);
    CHECK(nameAt(blob, at, 1).size == 3);
    CHECK(nameAt(blob, at, 2).size == 4);
}
