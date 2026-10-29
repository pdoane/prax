// Text between alchemy's standard strings and the library's views. A std::string converts to a PrStringView implicitly;
// these copy the other way.

#pragma once

#include "pr/base.h"
#include "pr/string.h"
#include "pr/string_view.h"

#include <string>

PR_ASSUME_NONNULL_BEGIN

namespace alchemy {

// Returns a copy of the view's text. The null view is the empty string.
inline std::string stringOf(PrStringView text) {
    return text.size != 0 ? std::string(text.data, text.size) : std::string();
}

// Returns a copy of the string's text, and releases the string.
inline std::string takeString(PrString* string) {
    std::string text = stringOf(prStringView(string));
    prStringReset(string);
    return text;
}

} // namespace alchemy

PR_ASSUME_NONNULL_END
