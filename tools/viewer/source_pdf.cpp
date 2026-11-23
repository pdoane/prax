// PDFs as sources (source_pdf.h), which Core Graphics rasterizes on macOS. Other platforms load none.

#include "tools/viewer/source_pdf.h"

#include "pr/texture.h"

#include "tools/viewer/documents.h"
#include "tools/viewer/zoom.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#if defined(__APPLE__)
#include "pr/apple_string.h"

#include <CoreGraphics/CoreGraphics.h>
#endif

namespace viewer {

namespace fs = std::filesystem;

// The document, its pages' boxes in the stack and the stack's extent, each page's raster at `ratio`, or null for a page
// not drawn there, and the viewport's scroll offset and the extent that it was last drawn at.
struct PdfSource {
    PrTextureCache* PR_NULLABLE textures = nullptr;     // the cache that realizes the pages' rasters
#if defined(__APPLE__)
    CGPDFDocumentRef document = nullptr;
#endif
    std::vector<PrRect> boxes;
    PrVec2 size{0, 0};
    float ratio = 0;
    std::vector<const PrTextureSource*> rasters;
    PrVec2 scroll{0, 0};
    PrVec2 port{0, 0};

    PdfSource() = default;
    PdfSource(const PdfSource&) = delete;
    PdfSource& operator=(const PdfSource&) = delete;
    ~PdfSource() {
        for (const PrTextureSource* raster : rasters)
            prTextureSourceRelease(raster);
#if defined(__APPLE__)
        CGPDFDocumentRelease(document);
#endif
    }
};

namespace {

#if defined(__APPLE__)

// CSS pixels per PDF point.
constexpr float kPxPerPoint = 96.0f / 72;

// Rasterizes page `index` of the document, from 0, `width` by `height` pixels over white.
const PrTextureSource* PR_NULLABLE rasterize(CGPDFDocumentRef document, size_t index, uint32_t width, uint32_t height) {
    CGPDFPageRef page = CGPDFDocumentGetPage(document, index + 1);
    if (page == nullptr || width == 0 || height == 0)
        return nullptr;
    std::vector<uint8_t> pixels(size_t(width) * height * 4);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(pixels.data(), width, height, 8, size_t(width) * 4, space,
        uint32_t(kCGImageAlphaPremultipliedLast) | uint32_t(kCGBitmapByteOrder32Big));
    CGColorSpaceRelease(space);
    if (context == nullptr)
        return nullptr;
    CGContextSetRGBFillColor(context, 1, 1, 1, 1);
    CGContextFillRect(context, CGRectMake(0, 0, width, height));
    CGRect crop = CGPDFPageGetBoxRect(page, kCGPDFCropBox);
    CGContextScaleCTM(context, CGFloat(width) / crop.size.width, CGFloat(height) / crop.size.height);
    CGContextTranslateCTM(context, -crop.origin.x, -crop.origin.y);
    CGContextDrawPDFPage(context, page);
    CGContextRelease(context);
    const PrPixelView view{pixels.data(), size_t(width) * 4, {width, height}};
    return prTextureSourceCreateCopy(view, PrPixelFormat_Rgba8, PrTextureAlpha_Premultiplied);
}

#endif

} // namespace

bool pdfRasterized() {
#if defined(__APPLE__)
    return true;
#else
    return false;
#endif
}

std::shared_ptr<PdfSource> loadPdf(const fs::path& path, PrTextureCache* PR_NULLABLE textures) {
#if defined(__APPLE__)
    std::string name = path.string();
    CFURLRef url = prAppleFileUrlCreate(name);
    if (url == nullptr)
        return nullptr;
    CGPDFDocumentRef document = CGPDFDocumentCreateWithURL(url);
    CFRelease(url);
    if (document == nullptr)
        return nullptr;
    std::shared_ptr<PdfSource> pdf = std::make_shared<PdfSource>();
    pdf->textures = textures;
    pdf->document = document;
    std::vector<PrVec2> extents;
    for (size_t i = 1; i <= CGPDFDocumentGetNumberOfPages(document); ++i) {
        // A page shows as a reader displays it: its crop box, which is its media box unless the document crops it (ISO
        // 32000-2, 14.11.2). A sheet with bleed and printer's marks shows whole, as the engine's paged view shows its
        // own sheets.
        CGRect crop = CGPDFPageGetBoxRect(CGPDFDocumentGetPage(document, i), kCGPDFCropBox);
        extents.push_back(PrVec2{float(crop.size.width) * kPxPerPoint, float(crop.size.height) * kPxPerPoint});
    }
    // The pages go down the stack, the gap apart, each centered across the widest.
    for (PrVec2 extent : extents)
        pdf->size.x = std::max(pdf->size.x, extent.x);
    for (PrVec2 extent : extents) {
        float left = (pdf->size.x - extent.x) / 2;
        float top = pdf->boxes.empty() ? 0 : pdf->boxes.back().bottom + kPageGap;
        pdf->boxes.push_back(PrRect{left, top, left + extent.x, top + extent.y});
        pdf->size.y = top + extent.y;
    }
    pdf->rasters.assign(pdf->boxes.size(), nullptr);
    return pdf;
#else
    (void)path;
    (void)textures;
    return nullptr;
#endif
}

void drawPdf(PdfSource& pdf, PrDrawList* list, float width, float height, float pixelRatio) {
    // The rasters are of the ratio that the last draw used, so a draw at another ratio rasterizes again.
    if (pixelRatio != pdf.ratio) {
        for (const PrTextureSource*& raster : pdf.rasters) {
            prTextureSourceRelease(raster);
            raster = nullptr;
        }
        pdf.ratio = pixelRatio;
    }
    // A taller or wider viewport may scroll the stack back.
    pdf.port = PrVec2{width, height};
    pdfScrollTo(pdf, pdf.scroll);
    PrVec2 at{std::max(0.0f, (width - pdf.size.x) / 2) - pdf.scroll.x, -pdf.scroll.y};
    for (size_t i = 0; i < pdf.boxes.size(); ++i) {
        PrRect box = pdf.boxes[i];
        bool shown = box.bottom + at.y > 0 && box.top + at.y < height && box.right + at.x > 0 && box.left + at.x < width;
        if (!shown) {
            prTextureSourceRelease(pdf.rasters[i]);
            pdf.rasters[i] = nullptr;
            continue;
        }
#if defined(__APPLE__)
        if (pdf.rasters[i] == nullptr) {
            uint32_t w = uint32_t(std::lround(prRectWidth(box) * pixelRatio));
            uint32_t h = uint32_t(std::lround(prRectHeight(box) * pixelRatio));
            pdf.rasters[i] = rasterize(pdf.document, i, w, h);
        }
#endif
        if (pdf.rasters[i] == nullptr)
            continue;
        PrRect placed{box.left + at.x, box.top + at.y, box.right + at.x, box.bottom + at.y};
        PrUVec2 extent = prTextureSourceDesc(pdf.rasters[i])->extent;
        // A later draw may release the raster while a pane still renders this list, so the list holds it, and awaits a
        // texture that has not arrived.
        PrTextureRegion region{};
        PrTextureCacheState state = PrTextureCacheState_Pending;
        if (pdf.textures != nullptr)
            state = prDrawListRealize(list, pdf.textures, pdf.rasters[i], prTextureCacheAskEncoded(0), &region);
        if (state != PrTextureCacheState_Ready && state != PrTextureCacheState_Partial)
            continue;
        PrDrawSource source = prDrawSourceRegion(&region, PrRect{0, 0, float(extent.x), float(extent.y)}, 0);
        PrDrawBrush brush = prDrawBrushImage(source, placed, PrDrawSampling_Linear);
        prDrawListRect(list, placed, {.brush = &brush});
    }
}

PrVec2 pdfScroll(const PdfSource& pdf) {
    return pdf.scroll;
}

void pdfScrollTo(PdfSource& pdf, PrVec2 offset) {
    pdf.scroll = PrVec2{clampScroll(offset.x, pdf.size.x, pdf.port.x), clampScroll(offset.y, pdf.size.y, pdf.port.y)};
}

float pdfContentWidth(const PdfSource& pdf) {
    return std::max(pdf.size.x, pdf.port.x);
}

} // namespace viewer
