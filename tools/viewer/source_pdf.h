// PDFs as sources (documents.h): how they load and draw, for the switches in documents.cpp. Nothing outside those
// switches calls them. A loader returns null for a file it cannot read.

#pragma once

#include "pr/draw_list.h"
#include "pr/vec.h"

#include <filesystem>
#include <memory>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// A PDF, which loads only if the platform rasterizes PDFs, as Core Graphics does on macOS. Its viewport shows its pages
// as a paged view shows a printed page's (prHtmlViewSetPaged): down the viewport, kPageGap px apart, each centered
// across the widest page and the stack across the viewport, each page the size of its crop box in CSS pixels. Nothing
// is drawn between or around the pages. The viewport scrolls through the stack, as far as its last draw's extent lets
// it. A draw rasterizes the pages that it shows at the pixel ratio that it draws at, over white, and keeps them until
// it draws at another ratio or no longer shows them.
struct PdfSource;
bool pdfRasterized();
std::shared_ptr<PdfSource> loadPdf(const std::filesystem::path& path);
void drawPdf(PdfSource& pdf, PrDrawList* list, float width, float height, float pixelRatio);
PrVec2 pdfScroll(const PdfSource& pdf);
void pdfScrollTo(PdfSource& pdf, PrVec2 offset);
float pdfContentWidth(const PdfSource& pdf);

} // namespace viewer

PR_ASSUME_NONNULL_END
