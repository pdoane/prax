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
    PrStringKind_External,   // bytes outside the string, which it does not own
    PrStringKind_Inline,     // bytes in the string itself
    PrStringKind_Owned,      // heap memory that the string owns and prStringReset frees
};
typedef uint8_t PrStringKind;

// The most bytes an inline string holds.
enum { kPrStringInlineCapacity = 31 };

// The number of bits of a string's tag that hold its kind.
enum { kPrStringKindBits = 2 };

// Text that a string holds or refers to. The text follows the rules of PrStringView unless the declaration that takes
// or returns the string says otherwise. No representation is NUL terminated. The bytes are read through prStringView,
// which carries the size.
//
// `tag` is the last byte of both representations, so the kind can be read before the live representation is known. Its
// low kPrStringKindBits bits hold the kind, and for an inline string the bits above them hold the size. External is
// zero, and an external string over no bytes is the null string, so a zeroed PrString is null.
typedef struct PrString {
    union {
        struct {
            const char* data;
            uint32_t size;
            uint32_t capacity;   // allocated bytes
            uint8_t _pad[15];
            PrStringKind tag;
        } heap;                  // Owned and External
        struct {
            char text[kPrStringInlineCapacity];
            PrStringKind tag;
        } small;                 // Inline
    };
} PrString;

PR_STATIC_ASSERT(sizeof(PrString) == 32);

// Returns the null string, which is not the empty string.
PrString prStringNull(void);

// Returns a copy of `text`, held inline if it fits and on the heap otherwise. The null view gives the null string.
PrString prStringCopy(PrStringView text);

// Returns an external string over the bytes of `text`. Nothing is copied, and the bytes must outlive the string.
PrString prStringExternal(PrStringView text);

// Returns the empty string, which is not the null string.
PrString prStringEmpty(void);

// Returns a copy of `string` that owns its bytes.
PrString prStringClone(const PrString* string);

// Replaces the text of `target` with a copy of `text`. `text` may point into the storage of `target`.
void prStringReplace(PrString* target, PrStringView text);

// Appends `text` to `string`, which then owns its bytes. An external string first copies its bytes, and a null string
// becomes `text`. `text` must not point into `string`. When the capacity grows, it at least doubles, so building a
// string one append at a time copies each byte once, amortized.
void prStringAppend(PrString* string, PrStringView text);

// Frees the heap memory of `string`, if any, and leaves the null string behind. Safe to call twice.
void prStringReset(PrString* string);

// Returns how `string` stores its bytes.
PrStringKind prStringKind(const PrString* string);

// Returns true if `string` is the null string.
bool prStringIsNull(const PrString* string);

// Returns true if `string` holds no bytes. The null string holds none.
bool prStringIsEmpty(const PrString* string);

// Returns a view of the bytes of `string`. The view stays valid until the string changes or moves.
PrStringView prStringView(const PrString* string);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
