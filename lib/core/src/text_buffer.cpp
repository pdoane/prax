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

PrStringView prTextBufferUtf8FromWtf8(PrTextBuffer* scratch, PrStringView text) {
    if (prUtf8Validate(text) == text.size)
        return text;
    prTextBufferClear(scratch);
    for (size_t at = 0; at < text.size;) {
        uint32_t codePoint = 0;
        at += prWtf8DecodeScalar(text, at, &codePoint);
        prTextBufferAppendCodePoint(scratch, codePoint);
    }
    return prTextBufferView(scratch);
}

void prTextBufferAppendWtf8(PrTextBuffer* buffer, PrStringView text) {
    size_t at = 0;
    while (at < text.size) {
        PrStringView rest = {text.data + at, text.size - at};
        size_t valid = prWtf8Validate(rest);
        // A first code point that a pair joins, or that is ill formed, goes through prWtf8Decode.
        uint32_t first = 0;
        if (valid == 0 || (at == 0 && prWtf8Decode(rest, 0, &first) == 3 && first >= 0xdc00 && first <= 0xdfff))
            valid = 0;
        if (valid != 0) {
            prTextBufferAppend(buffer, PrStringView{rest.data, valid});
            at += valid;
            continue;
        }
        uint32_t codePoint = 0;
        at += prWtf8Decode(text, at, &codePoint);
        prTextBufferAppendWtf8CodePoint(buffer, codePoint);
    }
}

void prTextBufferAppendWtf8CodePoint(PrTextBuffer* buffer, uint32_t codePoint) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(buffer->data);
    uint32_t size = buffer->size;
    if (codePoint >= 0xdc00 && codePoint <= 0xdfff && size >= 3 && bytes[size - 3] == 0xed &&
            bytes[size - 2] >= 0xa0 && bytes[size - 2] <= 0xaf) {
        uint32_t high = 0xd000 | uint32_t(bytes[size - 2] & 0x3f) << 6 | (bytes[size - 1] & 0x3fu);
        buffer->size -= 3;
        codePoint = 0x10000 + ((high - 0xd800) << 10) + (codePoint - 0xdc00);
    }
    char encoded[kPrUtf8MaxBytes];
    prTextBufferAppend(buffer, PrStringView{encoded, prWtf8Encode(codePoint, encoded)});
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
