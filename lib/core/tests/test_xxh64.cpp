#include "pr/string_ops.h"
#include "pr/xxh64.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

PrByteView bytes(PrStringView value) {
    return prStringBytes(value);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("XXH64 matches xxHash's values, whole and in pieces") {
    CHECK(prXxh64(bytes(""), 0) == 0xef46db3751d8e999ull);
    CHECK(prXxh64(bytes("abc"), 0) == 0x44bc2cf5ad770999ull);
    CHECK(prXxh64(bytes(""), 1) != prXxh64(bytes(""), 0));
    // In pieces of 7 bytes, across the stripes of 32 bytes, and past the stripes.
    const PrStringView text = "It was the best of times, it was the worst of times, it was the age of wisdom, it was "
        "the age of foolishness, it was the epoch of belief, it was the epoch of incredulity.";
    PrXxh64 hash = prXxh64Start(0);
    for (size_t at = 0; at < text.size; at += 7)
        prXxh64Update(&hash, bytes(PrStringView{text.data + at, text.size - at < 7 ? text.size - at : 7}));
    CHECK(prXxh64Digest(&hash) == prXxh64(bytes(text), 0));
}
