// MIME types (mimesniff): the essence of a type, its parameters, and the groups that its essence places it in.

#pragma once

#include "pr/base.h"
#include "pr/raw_string.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Returns the essence of the MIME type `type` (mimesniff/#mime-type-essence): its type and subtype, without its
// parameters or the whitespace around them, in their original case. The result is a part of `type`.
PrStringView prMimeTypeEssence(PrStringView type);

// Writes the value of the first parameter of the MIME type `type` named `name`, compared without regard to ASCII case,
// to `out`, without the quotes and escapes of a quoted value (mimesniff/#parse-a-mime-type). Returns false, and writes
// the null string, if `type` does not parse or has no such parameter. The caller releases `*out` with prRawStringReset.
bool prMimeTypeParameter(PrStringView type, PrStringView name, PrRawString* out);

// Returns true if `essence` is a JavaScript MIME type essence (mimesniff/#javascript-mime-type-essence-match), without
// regard to ASCII case.
bool prMimeTypeIsJavaScript(PrStringView essence);

// Returns true if the MIME type `type` is an XML MIME type (mimesniff/#xml-mime-type).
bool prMimeTypeIsXml(PrStringView type);

// Returns the minimized form of a lowercase essence (mimesniff/#minimize-a-supported-mime-type), which reveals only
// what kind of resource it names. The view is `essence` itself or a literal.
PrStringView prMimeTypeMinimize(PrStringView essence);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
