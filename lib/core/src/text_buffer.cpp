#include "pr/text_buffer.h"

#include "pr/assert.h"
#include "pr/memory.h"

#include "core/src/raw_string.h"

namespace pr_impl {
namespace {

// The capacity of a buffer when it first grows.
constexpr uint32_t kInitialCapacity = 64;

} // namespace
} // namespace pr_impl

using namespace pr_impl;

PR_NOINLINE void prTextBufferReserve(PrTextBuffer* buffer, uint32_t size) {
    PR_ASSERT(size <= UINT32_MAX - buffer->size);
    uint32_t needed = buffer->size + size;
    if (needed <= buffer->capacity)
        return;
    PR_ASSERT(buffer->capacity <= UINT32_MAX / 2);
    uint32_t doubled = buffer->capacity == 0 ? kInitialCapacity : buffer->capacity * 2;
    uint32_t grown = doubled > needed ? doubled : needed;
    buffer->data = static_cast<char*>(prMemoryRealloc(buffer->data, grown));
    buffer->capacity = grown;
}

void prTextBufferAppendUtf8(PrTextBuffer* buffer, PrStringView text) {
    size_t at = 0;
    for (;;) {
        PrStringView rest = {text.data + at, text.size - at};
        size_t valid = prUtf8Validate(rest);
        prTextBufferAppend(buffer, PrStringView{rest.data, valid});
        at += valid;
        if (at == text.size)
            return;

        uint32_t codePoint;
        at += prUtf8Decode(text, at, &codePoint);
        prTextBufferAppendCodePoint(buffer, kPrUtf8Replacement);
    }
}

void prTextBufferReset(PrTextBuffer* buffer) {
    if (buffer->data != nullptr)
        prMemoryFree(buffer->data);
    *buffer = PrTextBuffer{};
}

PrRawString prTextBufferTake(PrTextBuffer* buffer) {
    if (buffer->size <= kPrRawStringInlineCapacity) {
        PrRawString string = prRawStringCopy(prTextBufferView(buffer));
        prTextBufferReset(buffer);
        return string;
    }

    PrRawString string = rawStringAdopt(buffer->data, buffer->size, buffer->capacity);
    *buffer = PrTextBuffer{};
    return string;
}
