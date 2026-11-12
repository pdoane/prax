#include "pr/array.h"
#include "pr/byte_view.h"
#include "pr/string.h"
#include "pr/type_traits.h"

namespace pr_impl {

namespace {

// A container of bytes, and one of text, as anything with data() and size() is.
struct ByteRun {
    const uint8_t* PR_NULLABLE data() const { return nullptr; }
    size_t size() const { return 0; }
};
struct CharRun {
    const char* PR_NULLABLE data() const { return nullptr; }
    size_t size() const { return 0; }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// Bytes convert, from a run or from a whole array. Text does not.
static_assert(isConvertible<ByteRun, PrByteView>);
static_assert(isConvertible<Array<uint8_t>, PrByteView>);
static_assert(isConvertible<const uint8_t (&)[4], PrByteView>);
static_assert(!isConvertible<CharRun, PrByteView>);
static_assert(!isConvertible<String, PrByteView>);
static_assert(!isConvertible<const char*, PrByteView>);

constexpr uint8_t kBytes[] = {1, 0, 2};
static_assert(PrByteView(kBytes).size == 3);
static_assert(PrByteView(kBytes) == PrByteView(kBytes, 3));
static_assert(PrByteView{} == PrByteView(kBytes, 0));
