// Strings: text that a string holds, or bytes it refers to and does not own.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// How a string stores its bytes.
enum {
    PrRawStringKind_External,   // bytes outside the string, which it does not own
    PrRawStringKind_Inline,     // bytes in the string itself
    PrRawStringKind_Owned,      // heap memory that the string owns and prRawStringReset frees
};
typedef uint8_t PrRawStringKind;

// The most bytes an inline string holds.
enum { kPrRawStringInlineCapacity = 31 };

// The number of bits of a string's tag that hold its kind.
enum { kPrRawStringKindBits = 2 };

// Text that a string holds or refers to. The text follows the rules of PrStringView unless the declaration that takes
// or returns the string says otherwise. No representation is NUL terminated. The bytes are read through
// prRawStringView, which carries the size.
//
// `tag` is the last byte of both representations, so the kind can be read before the live representation is known. Its
// low kPrRawStringKindBits bits hold the kind, and for an inline string the bits above them hold the size. External is
// zero, and an external string over no bytes is the null string, so a zeroed PrRawString is null.
typedef struct PrRawString {
    union {
        struct {
            const char* data;
            uint32_t size;
            uint32_t capacity;   // allocated bytes
            uint8_t _pad[kPrRawStringInlineCapacity - sizeof(const char*) - 2 * sizeof(uint32_t)];
            PrRawStringKind tag;
        } heap;                  // Owned and External
        struct {
            char text[kPrRawStringInlineCapacity];
            PrRawStringKind tag;
        } small;                 // Inline
    };
} PrRawString;

PR_STATIC_ASSERT(sizeof(PrRawString) == 32);

// Returns the null string, which is not the empty string.
PrRawString prRawStringNull(void);

// Returns a copy of `text`, held inline if it fits and on the heap otherwise. The null view gives the null string.
PrRawString prRawStringCopy(PrStringView text);

// Returns an external string over the bytes of `text`. Nothing is copied, and the bytes must outlive the string.
PrRawString prRawStringExternal(PrStringView text);

// Returns the empty string, which is not the null string.
PrRawString prRawStringEmpty(void);

// Returns a copy of `string` that owns its bytes.
PrRawString prRawStringClone(const PrRawString* string);

// Replaces the text of `target` with a copy of `text`. `text` may point into the storage of `target`.
void prRawStringReplace(PrRawString* target, PrStringView text);

// Appends `text` to `string`, which then owns its bytes. An external string first copies its bytes, and a null string
// becomes `text`. `text` must not point into `string`. When the capacity grows, it at least doubles, so building a
// string one append at a time copies each byte once, amortized.
void prRawStringAppend(PrRawString* string, PrStringView text);

// Empties `string` and keeps the storage that it owns, so that text appended next reuses it. A null string stays null,
// and an external string becomes the empty string.
void prRawStringClear(PrRawString* string);

// Frees the heap memory of `string`, if any, and leaves the null string behind. Safe to call twice.
void prRawStringReset(PrRawString* string);

// Returns how `string` stores its bytes.
PrRawStringKind prRawStringKind(const PrRawString* string);

// Returns true if `string` is the null string.
bool prRawStringIsNull(const PrRawString* string);

// Returns true if `string` holds no bytes. The null string holds none.
bool prRawStringIsEmpty(const PrRawString* string);

// Returns a view of the bytes of `string`. The view stays valid until the string changes or moves.
PrStringView prRawStringView(const PrRawString* string);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
