// The SVG draw writer, which writes each page of a document as an SVG 2 document of its own, since SVG has no pages.

#pragma once

#include "pr/base.h"
#include "pr/draw_writer.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a writer of SVG. Glyphs are drawn as their outlines, each defined once, and a run's text labels the run's
// group. Images are embedded as PNG, which the options' image codec encodes. A null `options` means the defaults.
PrDrawWriter* prDrawSvgCreate(const PrDrawWriterOptions* PR_NULLABLE options);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
