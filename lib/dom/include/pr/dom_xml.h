// XML documents
//
// xml - https://www.w3.org/TR/xml/

#pragma once

#include "pr/base.h"
#include "pr/dom.h"
#include "pr/string_view.h"
#include "pr/xml.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Parses the UTF-8 `source` into a new XML document whose content type is `contentType` (pr/xml.h). The caller holds
// the document's one reference. If the source is not well-formed, the document holds the content before the error. A
// non-null `error` then describes the error and its position.
PrDomDocument* prDomParseXml(PrStringView source, PrStringView contentType, PrXmlError* PR_NULLABLE error);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
