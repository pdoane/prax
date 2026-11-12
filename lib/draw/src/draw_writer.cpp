// Draw writers: the interface functions, which enforce the contract around each backend's operations.

#include "pr/draw_writer.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/draw_writer_backend.h"
#include "pr/float.h"

#if defined(PR_DRAW_HAS_PDF)
#include "pr/draw_pdf.h"
#endif
#if defined(PR_DRAW_HAS_SVG)
#include "pr/draw_svg.h"
#endif

namespace pr_impl {

namespace {

constexpr PrStringView kBackendNames[PrDrawWriterBackend_Count] = {"default", "svg", "pdf"};

// The backends that the build has, in the order of preference, followed by Default. Default creates the first backend
// of the list, so a build without backends creates none.
constexpr PrDrawWriterBackend kBuilt[] = {
#if defined(PR_DRAW_HAS_PDF)
    PrDrawWriterBackend_Pdf,
#endif
#if defined(PR_DRAW_HAS_SVG)
    PrDrawWriterBackend_Svg,
#endif
    PrDrawWriterBackend_Default,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrDrawWriterBackend prDrawWriterBackendFromName(PrStringView name) {
    for (PrDrawWriterBackend backend = 0; backend < PrDrawWriterBackend_Count; ++backend) {
        if (prAsciiEqualsFold(kBackendNames[backend], name))
            return backend;
    }
    return PrDrawWriterBackend_Count;
}

PrStringView prDrawWriterBackendName(PrDrawWriterBackend backend) {
    return backend < PrDrawWriterBackend_Count ? kBackendNames[backend] : PrStringView{};
}

bool prDrawWriterBackendIsBuilt(PrDrawWriterBackend backend) {
    for (PrDrawWriterBackend built : kBuilt) {
        if (built != PrDrawWriterBackend_Default && (built == backend || backend == PrDrawWriterBackend_Default))
            return true;
    }
    return false;
}

PrDrawWriterOptions prDrawWriterOptionsDefault(void) {
    PrDrawWriterOptions options{};
    options.rasterScale = 2;
    return options;
}

PrStatus prDrawWriterCreate(PrDrawWriterBackend backend, const PrDrawWriterOptions* PR_NULLABLE options,
        PrDrawWriter* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrDrawWriterOptions o = options != nullptr ? *options : prDrawWriterOptionsDefault();
    PR_ASSERT(o.rasterScale > 0 && prF32IsFinite(o.rasterScale));
    if (backend == PrDrawWriterBackend_Default)
        backend = kBuilt[0];
    switch (backend) {
#if defined(PR_DRAW_HAS_PDF)
        case PrDrawWriterBackend_Pdf:
            *out = prDrawPdfCreate(&o);
            return PrStatus_Ok;
#endif
#if defined(PR_DRAW_HAS_SVG)
        case PrDrawWriterBackend_Svg:
            *out = prDrawSvgCreate(&o);
            return PrStatus_Ok;
#endif
        default: return PrStatus_NotSupported;
    }
}

void prDrawWriterDestroy(PrDrawWriter* PR_NULLABLE writer) {
    if (writer != nullptr && writer->destroyFn != nullptr)
        writer->destroyFn(writer);
}

void prDrawWriterBegin(PrDrawWriter* writer, const PrDrawDocumentDesc* desc) {
    PR_ASSERT(desc->write != nullptr);
    writer->beginFn(writer, desc);
}

void prDrawWriterAddPage(PrDrawWriter* writer, const PrDrawPage* page) {
    PR_ASSERT(prDrawListDepth(page->list) == 0);
    writer->addPageFn(writer, page);
}

void prDrawWriterEnd(PrDrawWriter* writer) {
    writer->endFn(writer);
}

void prDrawWriterWrite(PrDrawWriter* writer, const PrDrawDocumentDesc* desc, const PrDrawPage* PR_NULLABLE pages,
        uint32_t pageCount) {
    prDrawWriterBegin(writer, desc);
    for (uint32_t i = 0; i < pageCount; ++i)
        prDrawWriterAddPage(writer, &pages[i]);
    prDrawWriterEnd(writer);
}
