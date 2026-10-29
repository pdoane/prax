// Owned strings and string views.

#include "pr/string.h"

#include "pr/assert.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/string_ops.h"

namespace pr_impl {

// The tag sits at the same offset in both representations, so the kind can be read before the live one is known.
static_assert(sizeof(PrString{}.heap) == sizeof(PrString));
static_assert(offsetof(PrString, heap.tag) == offsetof(PrString, small.tag));

namespace {

PrStringKind inlineTag(size_t size) {
    return PrStringKind((size << kPrStringKindBits) | PrStringKind_Inline);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrString prStringNull(void) {
    PrString string = {};
    return string;
}

PrString prStringCopy(PrStringView text) {
    PR_ASSERT(text.data != nullptr || text.size == 0);
    PR_ASSERT(text.size <= 0xfffffffeu);
    if (text.data == nullptr)
        return prStringNull();

    PrString string = {};
    if (text.size <= kPrStringInlineCapacity) {
        if (text.size != 0)
            memCopy(string.small.text, text.data, text.size);
        string.small.tag = inlineTag(text.size);
        return string;
    }

    char* data = static_cast<char*>(prMemoryAlloc(text.size));
    memCopy(data, text.data, text.size);
    return stringAdopt(data, uint32_t(text.size), uint32_t(text.size));
}

PrString prStringExternal(PrStringView text) {
    PR_ASSERT(text.data != nullptr || text.size == 0);
    PR_ASSERT(text.size <= 0xffffffffu);

    PrString string = {};
    string.heap.data = text.data;
    string.heap.size = uint32_t(text.size);
    string.heap.capacity = 0;
    string.heap.tag = PrStringKind_External;
    return string;
}

PrString prStringEmpty(void) {
    PrString string = {};
    string.small.tag = inlineTag(0);
    return string;
}

PrString prStringClone(const PrString* string) {
    return prStringCopy(prStringView(string));
}

void prStringReplace(PrString* target, PrStringView text) {
    PrString value = prStringCopy(text);
    prStringReset(target);
    *target = value;
}

void prStringAppend(PrString* string, PrStringView text) {
    PR_ASSERT(string != nullptr && (text.data != nullptr || text.size == 0));
    if (text.size == 0)
        return;

    // An external string does not own its bytes, so its bytes are copied before the text is appended. A null string has
    // no bytes and becomes the text.
    if (prStringKind(string) == PrStringKind_External) {
        PrStringView external = prStringView(string);
        *string = external.size != 0 ? prStringCopy(external) : prStringEmpty();
    }

    PrStringView held = prStringView(string);
    PR_ASSERT(text.size <= 0xfffffffeu - held.size);
    size_t size = held.size + text.size;

    if (prStringKind(string) == PrStringKind_Inline) {
        if (size <= kPrStringInlineCapacity) {
            memCopy(string->small.text + held.size, text.data, text.size);
            string->small.tag = inlineTag(size);
            return;
        }

        // The text no longer fits inline, so it moves to the heap with room to grow.
        size_t capacity = size * 2;
        char* moved = static_cast<char*>(prMemoryAlloc(capacity));
        memCopy(moved, string->small.text, held.size);
        memCopy(moved + held.size, text.data, text.size);
        *string = stringAdopt(moved, uint32_t(size), uint32_t(capacity));
        return;
    }

    if (size > string->heap.capacity) {
        size_t doubled = size_t(string->heap.capacity) * 2;
        size_t capacity = doubled > size ? doubled : size;
        char* moved = static_cast<char*>(prMemoryAlloc(capacity));
        memCopy(moved, string->heap.data, held.size);
        prMemoryFree(const_cast<char*>(string->heap.data));
        string->heap.data = moved;
        string->heap.capacity = uint32_t(capacity);
    }

    char* data = const_cast<char*>(string->heap.data);
    memCopy(data + held.size, text.data, text.size);
    string->heap.size = uint32_t(size);
}

void prStringReset(PrString* string) {
    PR_ASSERT(string != nullptr);
    if (prStringKind(string) == PrStringKind_Owned)
        prMemoryFree(const_cast<char*>(string->heap.data));

    PrString null = {};
    *string = null;
}

PrStringKind prStringKind(const PrString* string) {
    return PrStringKind(string->small.tag & ((1 << kPrStringKindBits) - 1));
}

bool prStringIsNull(const PrString* string) {
    return prStringView(string).data == nullptr;
}

bool prStringIsEmpty(const PrString* string) {
    return prStringView(string).size == 0;
}

PrStringView prStringView(const PrString* string) {
    if ((string->small.tag & ((1 << kPrStringKindBits) - 1)) == PrStringKind_Inline)
        return PrStringView{string->small.text, size_t(string->small.tag >> kPrStringKindBits)};
    return PrStringView{string->heap.data, string->heap.size};
}
