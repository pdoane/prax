// Draw writers: draw lists written as documents, one list for each page, such as SVG or PDF.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/compress.h"
#include "pr/draw_glyph.h"
#include "pr/draw_list.h"
#include "pr/draw_structure.h"
#include "pr/geom.h"
#include "pr/image.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/task.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A writer of documents, created by prDrawWriterCreate or by its backend's own constructor. A writer writes one
// document at a time, and is used by one thread.
typedef struct PrDrawWriter PrDrawWriter;

// Receives the next bytes of one of a document's outputs, in order. A format that has pages, as PDF does, writes the
// whole document as output 0. A format without pages, as SVG, writes each page as an output of its own, a document of
// that format, numbered by page from 0, and writes an output whole before it begins the next. The bytes are valid only
// during the call.
typedef void (*PrDrawWriterWriteFn)(void* PR_NULLABLE context, uint32_t output, PrByteView bytes);

// A document to write: where its bytes go, and what describes it beyond its pages.
typedef struct PrDrawDocumentDesc {
    PrDrawWriterWriteFn write;
    void* PR_NULLABLE context;                  // passed to `write`

    // What the content means, which a format that tags its content joins to the marks of the pages (PrDrawCommand's
    // PushMark), or null for a document without tags. The structure stays unchanged until the document ends.
    const PrDrawStructure* PR_NULLABLE structure;

    PrStringView title;                         // the document's title, or empty
    PrStringView language;                      // the document's language as a BCP 47 tag, or empty
} PrDrawDocumentDesc;

// A page of a document: its list, drawn on a sheet of `extent` px from the sheet's top left, and the boxes of print
// production on the sheet, in px from its top left (ISO 32000-2, 14.11.2). The bleed box is the area that the page's
// content is clipped to when it is printed, and the trim box is the finished page after the sheet is trimmed. An empty
// box is the whole sheet.
typedef struct PrDrawPage {
    const PrDrawList* list;
    PrVec2 extent;
    PrRect bleed;
    PrRect trim;
} PrDrawPage;

// The library's backends of writers, which the build turns on (README.md), and Default, the first that the build has.
enum {
    PrDrawWriterBackend_Default,
    PrDrawWriterBackend_Svg,
    PrDrawWriterBackend_Pdf,
    PrDrawWriterBackend_Count,
};
typedef uint8_t PrDrawWriterBackend;

// Returns the backend that `name` names, as prDrawWriterBackendName spells it, ignoring case, or Count if it names
// none.
PrDrawWriterBackend prDrawWriterBackendFromName(PrStringView name);

// Returns the name of a backend in lowercase, as a command line spells it.
PrStringView prDrawWriterBackendName(PrDrawWriterBackend backend);

// Returns true if the build has the backend. The build has Default if it has any backend.
bool prDrawWriterBackendIsBuilt(PrDrawWriterBackend backend);

// How a writer writes: what every backend takes.
typedef struct PrDrawWriterOptions {
    PrCompressCodec* PR_NULLABLE compress;          // compresses the streams of a format that compresses them, as
                                                    // Deflate; without it, the streams are written as they are

    // Compresses several streams at once on the pool's threads, if not null, and then writes them in order. The codec
    // must then encode on several threads at once, as the built-in codec does. The writer runs loops on the pool from
    // the thread that writes, so no other thread may run loops on it meanwhile.
    PrTaskPool* PR_NULLABLE tasks;

    PrImageCodec* PR_NULLABLE images;               // encodes the images of a format that embeds them encoded, as PNG;
                                                    // without it, such a format writes no images
    PrDrawGlyphDecoder* PR_NULLABLE glyphDecoder;   // draws the glyphs that are pictures; without it, they draw nothing
    float rasterScale;                              // the target pixels per px at which a writer rasterizes what its
                                                    // format cannot draw, finite and above zero
} PrDrawWriterOptions;

// Returns options without codecs or a glyph decoder, which rasterize at two target pixels per px.
PrDrawWriterOptions prDrawWriterOptionsDefault(void);

// Creates a writer by the backend. Null options mean the defaults. Returns NotSupported if the build does not have the
// backend. On failure, `*out` is null.
PrStatus prDrawWriterCreate(PrDrawWriterBackend backend, const PrDrawWriterOptions* PR_NULLABLE options,
    PrDrawWriter* PR_NULLABLE* PR_NONNULL out);

// Frees the writer. A document that it began must have ended.
void prDrawWriterDestroy(PrDrawWriter* PR_NULLABLE writer);

// Begins a document. The writer must not be writing one already. The format's header may be written now or with the
// first page.
void prDrawWriterBegin(PrDrawWriter* writer, const PrDrawDocumentDesc* desc);

// Writes a page of the document. The page's list has no open pushes. It stays unchanged until the document ends, since
// a writer may refer to it until then.
void prDrawWriterAddPage(PrDrawWriter* writer, const PrDrawPage* page);

// Ends the document and writes what remains of it.
void prDrawWriterEnd(PrDrawWriter* writer);

// Writes a document of `pages`, in order: begins it, adds each page, and ends it.
void prDrawWriterWrite(PrDrawWriter* writer, const PrDrawDocumentDesc* desc, const PrDrawPage* PR_NULLABLE pages,
    uint32_t pageCount);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
