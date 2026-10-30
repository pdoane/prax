// Percent encoding and decoding of bytes.
//
// url - https://url.spec.whatwg.org/

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The bytes that percent-encoding escapes, one set for each of the spec's percent-encode sets
// (url/#c0-control-percent-encode-set and the sets after it). Each set adds the bytes listed to the set it names.
enum {
    PrUrlPercentEncodeSet_C0Control,    // C0 controls and the bytes above ~
    PrUrlPercentEncodeSet_Fragment,     // C0Control, space and " < > `
    PrUrlPercentEncodeSet_Query,        // C0Control, space and " # < >
    PrUrlPercentEncodeSet_SpecialQuery, // Query and '
    PrUrlPercentEncodeSet_Path,         // Query and ? ^ ` { }
    PrUrlPercentEncodeSet_Userinfo,     // Path and / : ; = @ [ \ ] |
    PrUrlPercentEncodeSet_Component,    // Userinfo and $ % & + ,
    PrUrlPercentEncodeSet_Form,         // Component and ! ' ( ) ~
    PrUrlPercentEncodeSet_Count,
};
typedef uint8_t PrUrlPercentEncodeSet;

// Writes up to `capacity` bytes of `input`, with each byte in `set` percent-encoded in uppercase hex, and returns the
// full size. `out` does not overlap `input`.
size_t prUrlPercentEncode(PrStringView input, PrUrlPercentEncodeSet set, char* PR_NULLABLE out, size_t capacity);

// Writes up to `capacity` bytes of `input` percent-decoded (url/#percent-decode), and returns the full size. `out` may
// be `input.data`. A `%` not followed by two hex digits is kept.
size_t prUrlPercentDecode(PrStringView input, char* PR_NULLABLE out, size_t capacity);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
