// The PDF draw writer, which writes draw lists as a PDF 2.0 document (ISO 32000-2), a page for each list, tagged if the
// document has a structure.

#pragma once

#include "pr/base.h"
#include "pr/draw_writer.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a writer of PDF. Text is written as text in embedded subsets of its fonts, with maps back to its characters.
// Streams are compressed with the options' codec. A null `options` means the defaults.
PrDrawWriter* prDrawPdfCreate(const PrDrawWriterOptions* PR_NULLABLE options);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
