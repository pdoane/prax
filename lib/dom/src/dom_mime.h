// MIME types, as fetched resources declare them.
//
// mimesniff - https://mimesniff.spec.whatwg.org/

#pragma once

#include "pr/base.h"
#include "pr/charset_names.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns the essence of a MIME type (mimesniff/#mime-type-essence): its type and subtype without parameters, in their
// original case.
PrStringView domMimeTypeEssence(PrStringView type);

// Returns the charset that the charset parameter of the MIME type `type` names (fetch/#legacy-extract-an-encoding), or
// PrCharset_None if the type does not parse (mimesniff/#parse-a-mime-type), has no charset parameter, or names no
// charset.
PrCharset domMimeTypeCharset(PrStringView type);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
