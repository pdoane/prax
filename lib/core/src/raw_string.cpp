// Owned strings and string views.

#include "pr/raw_string.h"
#include "core/src/raw_string.h"

#include "pr/assert.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/string_ops.h"

namespace pr_impl {

// The tag sits at the same offset in both representations, so the kind can be read before the live one is known.
static_assert(sizeof(PrRawString{}.heap) == sizeof(PrRawString));
static_assert(offsetof(PrRawString, heap.tag) == offsetof(PrRawString, small.tag));

namespace {

PrRawStringKind inlineTag(size_t size) {
    return PrRawStringKind((size << kPrRawStringKindBits) | PrRawStringKind_Inline);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrRawString prRawStringNull(void) {
    PrRawString string = {};
    return string;
}

PrRawString prRawStringCopy(PrStringView text) {
    PR_ASSERT(text.data != nullptr || text.size == 0);
    PR_ASSERT(text.size <= 0xfffffffeu);
    if (text.data == nullptr)
        return prRawStringNull();

    PrRawString string = {};
    if (text.size <= kPrRawStringInlineCapacity) {
        if (text.size != 0)
            prMemCopy(string.small.text, text.data, text.size);
        string.small.tag = inlineTag(text.size);
        return string;
    }

    char* data = static_cast<char*>(prMemoryAlloc(text.size));
    prMemCopy(data, text.data, text.size);
    return rawStringAdopt(data, uint32_t(text.size), uint32_t(text.size));
}

PrRawString prRawStringExternal(PrStringView text) {
    PR_ASSERT(text.data != nullptr || text.size == 0);
    PR_ASSERT(text.size <= 0xffffffffu);

    PrRawString string = {};
    string.heap.data = text.data;
    string.heap.size = uint32_t(text.size);
    string.heap.capacity = 0;
    string.heap.tag = PrRawStringKind_External;
    return string;
}

PrRawString prRawStringEmpty(void) {
    PrRawString string = {};
    string.small.tag = inlineTag(0);
    return string;
}

PrRawString prRawStringClone(const PrRawString* string) {
    return prRawStringCopy(prRawStringView(string));
}

void prRawStringReplace(PrRawString* target, PrStringView text) {
    PrRawString value = prRawStringCopy(text);
    prRawStringReset(target);
    *target = value;
}

void prRawStringAppend(PrRawString* string, PrStringView text) {
    PR_ASSERT(string != nullptr && (text.data != nullptr || text.size == 0));
    if (text.size == 0)
        return;

    // An external string does not own its bytes, so its bytes are copied before the text is appended. A null string has
    // no bytes and becomes the text.
    if (prRawStringKind(string) == PrRawStringKind_External) {
        PrStringView external = prRawStringView(string);
        *string = external.size != 0 ? prRawStringCopy(external) : prRawStringEmpty();
    }

    PrStringView held = prRawStringView(string);
    PR_ASSERT(text.size <= 0xfffffffeu - held.size);
    size_t size = held.size + text.size;

    if (prRawStringKind(string) == PrRawStringKind_Inline) {
        if (size <= kPrRawStringInlineCapacity) {
            prMemCopy(string->small.text + held.size, text.data, text.size);
            string->small.tag = inlineTag(size);
            return;
        }

        // The text no longer fits inline, so it moves to the heap with room to grow.
        size_t capacity = size * 2;
        char* moved = static_cast<char*>(prMemoryAlloc(capacity));
        prMemCopy(moved, string->small.text, held.size);
        prMemCopy(moved + held.size, text.data, text.size);
        *string = rawStringAdopt(moved, uint32_t(size), uint32_t(capacity));
        return;
    }

    if (size > string->heap.capacity) {
        size_t doubled = size_t(string->heap.capacity) * 2;
        size_t capacity = doubled > size ? doubled : size;
        char* moved = static_cast<char*>(prMemoryAlloc(capacity));
        prMemCopy(moved, string->heap.data, held.size);
        prMemoryFree(const_cast<char*>(string->heap.data));
        string->heap.data = moved;
        string->heap.capacity = uint32_t(capacity);
    }

    char* data = const_cast<char*>(string->heap.data);
    prMemCopy(data + held.size, text.data, text.size);
    string->heap.size = uint32_t(size);
}

void prRawStringReset(PrRawString* string) {
    PR_ASSERT(string != nullptr);
    if (prRawStringKind(string) == PrRawStringKind_Owned)
        prMemoryFree(const_cast<char*>(string->heap.data));

    PrRawString null = {};
    *string = null;
}

PrRawStringKind prRawStringKind(const PrRawString* string) {
    return PrRawStringKind(string->small.tag & ((1 << kPrRawStringKindBits) - 1));
}

bool prRawStringIsNull(const PrRawString* string) {
    return prRawStringView(string).data == nullptr;
}

bool prRawStringIsEmpty(const PrRawString* string) {
    return prRawStringView(string).size == 0;
}

PrStringView prRawStringView(const PrRawString* string) {
    if ((string->small.tag & ((1 << kPrRawStringKindBits) - 1)) == PrRawStringKind_Inline)
        return PrStringView{string->small.text, size_t(string->small.tag >> kPrRawStringKindBits)};
    return PrStringView{string->heap.data, string->heap.size};
}
