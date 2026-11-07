// The PDF draw writer, which writes draw lists as a PDF 2.0 document (ISO 32000-2), a page for each list, tagged if the
// document has a structure.
//
// PDF 2.0 - ISO 32000-2:2020. Tagged PDF - ISO 32000-2, 14.7 and 14.8. PDF/UA-2 - ISO 14289-2.

#include "pr/draw_pdf.h"

#include "pr/alloc.h"
#include "pr/draw_glyph.h"
#include "pr/draw_lower.h"
#include "pr/draw_writer_backend.h"
#include "pr/float.h"
#include "pr/font_subset.h"
#include "pr/hash.h"
#include "pr/hash_index.h"
#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/sort.h"
#include "pr/utf8.h"

#include "draw/backends/shared/src/draw_shared_writer.h"

namespace pr_impl {

namespace {

// Points per px: a px is a 96th of an inch, and PDF's default space has 72 points to the inch.
constexpr double kPointsPerPx = 0.75;

// The objects that every document has, by number.
constexpr uint32_t kCatalog = 1;
constexpr uint32_t kPages = 2;
constexpr uint32_t kResources = 3;

// The structure namespaces (ISO 32000-2, 14.7.4).
constexpr PrStringView kNamespace1 = "http://iso.org/pdf/ssn";
constexpr PrStringView kNamespace2 = "http://iso.org/pdf2/ssn";

// A font that the document shows text in, and the glyphs that it shows.
struct PdfFont {
    const PrFont* font;
    uint32_t object;                    // the Type 0 font
    PrFontSubsetFormat format;
    RawArray<uint8_t> used;             // by glyph: nonzero if shown
    RawArray<double> widths;            // by glyph: its width in the font's widths, once it is shown
    RawArray<uint32_t> unicodeStart;    // by glyph: where its characters start in `unicode`, or zero for none
    RawArray<uint32_t> unicodeSize;     // by glyph
    PrTextBuffer unicode{};             // UTF-8, starting with one byte so that offsets are never zero
};

// An image that the document holds once, however many times it is drawn.
struct PdfImage {
    uint64_t id;
    uint32_t object;
};

// A graphics state of constant alphas and a blend mode.
struct PdfState {
    float fill;
    float stroke;
    PrDrawBlendMode blendMode;
    uint32_t object;
};

// The parameters of a content stream's graphics state that the writer tracks, so that it leaves out an operator that
// would set a parameter to the value that it holds. Saving and restoring the graphics state (q and Q) saves and
// restores them.
struct PdfGraphicsState {
    PrColor fill;                       // the nonstroking color, if `fillKnown`
    uint32_t extGState;                 // by state: the selected state, or kHashMissing for the initial alphas
    uint32_t font;                      // by font: the text font, or kHashMissing if it is not known
    double fontSize;                    // the text font size, as written
    double charSpacing;                 // the character spacing (Tc), as written
    uint8_t renderMode;                 // the text rendering mode
    bool fillKnown;
};

// A content stream being written: a page's, or a group's form.
struct PdfStream {
    PrTextBuffer content{};
    uint32_t object;                    // the form, or the page for a page's stream
    uint32_t structParents;             // its key in the parent tree, if the document is tagged
    uint32_t nextMcid;
    PrRect bounds;                      // a form's bounding box, in page px
    DrawSharedGroup group;
    bool form;

    PdfGraphicsState state;
    RawArray<PdfGraphicsState> saved;   // the states that q saved, innermost last

    // The text object that the stream has open, if `text`. Text runs that follow one another share it, and any other
    // operator closes it first. The text line matrix is as written, and moves from one run to the next by Td while its
    // linear part stays the same.
    bool text;
    PrAffine2 line;

    // True while the stream has an artifact's marked-content sequence open. Operations that paint artifacts one after
    // another share one sequence, which closes before anything that is not an artifact and before q and Q, so that it
    // nests with the graphics state.
    bool artifact;
};

// A mark's content as the document holds it: a marked-content sequence of a stream.
struct PdfMark {
    uint64_t key;
    uint32_t page;                      // the index of the page
    uint32_t stream;                    // the form that holds it, or zero for the page's own stream
    uint32_t mcid;
    uint32_t structParents;
    PrRect bounds;                      // in page px
};

// A mark that a walk has open, and the marked-content sequence that it is in, if any.
struct PdfOpenMark {
    uint64_t key;
    uint32_t mark;                      // the index of the mark, or kHashMissing for a mark that tags nothing
};

// A stream that waits to be compressed on the threads of the writer's pool, and then written.
struct PdfPending {
    uint32_t object;
    PrTextBuffer dictionary{};
    RawArray<uint8_t> data;
    RawArray<uint8_t> encoded;
    size_t encodedSize;                 // the size of the compressed data, or zero if it is written as it is
};

// A page as the document holds it.
struct PdfPage {
    uint32_t object;
    uint32_t content;
    uint32_t structParents;
    PrVec2 extent;                      // px
    PrRect bleed;                       // px from the top left, empty for the whole page
    PrRect trim;                        // px from the top left, empty for the whole page
    RawArray<uint32_t> annotations;
};

struct PdfWriter : PrDrawWriter {
    PrDrawWriterOptions options;
    DrawSharedWalker walker;
    PrDrawDocumentDesc document;
    const PrDrawStructure* PR_NULLABLE structure;

    uint64_t written;                   // bytes written so far
    uint64_t hash;                      // of the bytes written so far, which the document's identifier is made from
    PrTextBuffer out{};                 // the object being written

    // Where each object is: where it starts in the file, or its index in the object stream that holds it, or zero for
    // an object not yet written.
    RawArray<uint64_t> offsets;         // by object
    RawArray<uint32_t> containers;      // by object: the object stream that holds it, or zero

    // The object stream being gathered, if the writer has a codec (ISO 32000-2, 7.5.7). Each object that is not a
    // stream goes in one, where it is compressed with the objects around it.
    uint32_t packedStream;              // its object, or zero for none
    uint32_t packedCount;               // the number of objects that it holds
    PrTextBuffer packedIndex{};         // the number and the offset of each of its objects
    PrTextBuffer packed{};              // its objects

    // The streams that wait to be compressed on the pool's threads, the first `pendingCount` of `pending`, and the
    // number of bytes that they hold.
    RawArray<PdfPending> pending;
    uint32_t pendingCount;
    size_t pendingBytes;

    RawArray<PdfPage> pages;
    RawArray<PdfStream> streams;        // the streams being written, innermost last
    uint32_t streamDepth;

    RawArray<PdfFont> fonts;
    KeyIndex fontIndex;
    RawArray<PdfImage> images;
    KeyIndex imageIndex;
    RawArray<PdfState> states;
    RawArray<uint32_t> patterns;        // by pattern: its object
    RawArray<uint32_t> forms;           // by form: its object

    // Tagging: the node that each key belongs to, the marks of each, and what each stream's parent tree entry holds.
    RawArray<uint64_t> keys;            // the structure's keys
    RawArray<uint32_t> keyNodes;        // by key: its node
    KeyIndex keyIndex;
    RawArray<PdfMark> marks;
    RawArray<PdfOpenMark> openMarks;
    uint32_t nextStructParents;
    uint32_t namespaceObject;           // the namespace of PDF 2.0's structure types
    uint32_t legacyNamespace;           // the namespace of PDF 1.7's

    // Scratch.
    RawArray<uint32_t> glyphStarts;     // by glyph of a run: where its characters start in the run's text
    RawArray<uint32_t> glyphSizes;      // by glyph of a run
    RawArray<uint64_t> clusterOrder;
    RawArray<int64_t> gaps;             // by glyph of a text line: its gap after the glyph before it, in thousandths
    PrTextBuffer scratch{};
    RawArray<uint8_t> bytes;
    RawArray<uint8_t> alpha;
    RawArray<DrawSharedStop> stops;
    PrPath* path;
};

// ---------------------------------------------------------------------------------------------------------------------
// Output

void reference(PrTextBuffer& out, uint32_t object) {
    prTextBufferAppendU64(&out, object, 10);
    prTextBufferAppend(&out, " 0 R");
}

// Appends the page's box `key`, `box` in px from the page's top left, to a page's dictionary in default user space (ISO
// 32000-2, 14.11.2), unless the box is empty or is the media box, which a missing box defaults to.
void appendBox(PrTextBuffer& out, PrStringView key, const PdfPage& page, PrRect box) {
    if (box.right <= box.left || box.bottom <= box.top)
        return;
    if (box.left == 0 && box.top == 0 && box.right == page.extent.x && box.bottom == page.extent.y)
        return;
    double height = double(page.extent.y);
    const double corners[4] =
        {double(box.left), height - double(box.bottom), double(box.right), height - double(box.top)};
    prTextBufferAppend(&out, " /");
    prTextBufferAppend(&out, key);
    prTextBufferAppend(&out, " [");
    for (uint32_t i = 0; i < 4; ++i) {
        if (i != 0)
            prTextBufferAppendChar(&out, ' ');
        drawSharedAppendNumber(out, corners[i] * kPointsPerPx);
    }
    prTextBufferAppendChar(&out, ']');
}

void emit(PdfWriter& p, const void* data, size_t size) {
    p.document.write(p.document.context, 0, PrByteView{static_cast<const uint8_t*>(data), size});
    p.written += size;
    p.hash = prHashBytes(data, size, p.hash);
}

uint32_t allocate(PdfWriter& p) {
    p.offsets.push(0);
    p.containers.push(0);
    return p.offsets.count - 1;
}

void writeStream(PdfWriter& p, uint32_t object, PrStringView dictionary, const uint8_t* data, size_t size);
void writeStreamNow(PdfWriter& p, uint32_t object, PrStringView dictionary, const uint8_t* data, size_t size);
void flushPending(PdfWriter& p);

// The most objects that an object stream holds. A stream of more objects compresses better, and a reader decompresses
// the whole stream to read any one of them.
constexpr uint32_t kPackedObjects = 512;

// Writes the object stream being gathered, if there is one.
void flushPacked(PdfWriter& p) {
    if (p.packedStream == 0)
        return;
    prTextBufferAppendChar(&p.packedIndex, '\n');
    uint32_t first = p.packedIndex.size;
    prTextBufferAppend(&p.packedIndex, PrStringView{p.packed.data, p.packed.size});
    PrTextBuffer dictionary{};
    prTextBufferAppend(&dictionary, " /Type /ObjStm /N ");
    prTextBufferAppendU64(&dictionary, p.packedCount, 10);
    prTextBufferAppend(&dictionary, " /First ");
    prTextBufferAppendU64(&dictionary, first, 10);
    writeStream(p, p.packedStream, PrStringView{dictionary.data, dictionary.size},
        reinterpret_cast<const uint8_t*>(p.packedIndex.data), p.packedIndex.size);
    prTextBufferReset(&dictionary);
    p.packedStream = 0;
    p.packedCount = 0;
    prTextBufferClear(&p.packedIndex);
    prTextBufferClear(&p.packed);
}

// Writes `p.out` as the body of object `object`: in an object stream if the writer has a codec, and on its own
// otherwise.
void writeObject(PdfWriter& p, uint32_t object) {
    if (p.options.compress != nullptr) {
        if (p.packedStream == 0)
            p.packedStream = allocate(p);
        p.containers.data[object] = p.packedStream;
        p.offsets.data[object] = p.packedCount++;
        prTextBufferAppendU64(&p.packedIndex, object, 10);
        prTextBufferAppendChar(&p.packedIndex, ' ');
        prTextBufferAppendU64(&p.packedIndex, p.packed.size, 10);
        prTextBufferAppendChar(&p.packedIndex, ' ');
        prTextBufferAppend(&p.packed, PrStringView{p.out.data, p.out.size});
        prTextBufferAppendChar(&p.packed, '\n');
        prTextBufferClear(&p.out);
        if (p.packedCount == kPackedObjects)
            flushPacked(p);
        return;
    }
    p.offsets.data[object] = p.written;
    prTextBufferClear(&p.scratch);
    prTextBufferAppendU64(&p.scratch, object, 10);
    prTextBufferAppend(&p.scratch, " 0 obj\n");
    emit(p, p.scratch.data, p.scratch.size);
    emit(p, p.out.data, p.out.size);
    emit(p, "\nendobj\n", 8);
    prTextBufferClear(&p.out);
}

// Compresses `size` bytes of `data` with the codec into `encoded`, and returns the size of the compressed data, or zero
// if compressing them does not make them smaller.
size_t compressStream(PrCompressCodec* codec, const uint8_t* data, size_t size, RawArray<uint8_t>& encoded) {
    if (size <= 32)
        return 0;
    size_t bound = prCompressCodecEncodeBound(codec, PrCompressFormat_Zlib, size);
    if (bound == 0)
        return 0;
    encoded.resize(uint32_t(bound));
    size_t written = 0;
    if (prCompressCodecEncode(codec, PrCompressFormat_Zlib, PrCompressLevel_Default, PrByteView{data, size},
            encoded.data, &written) != PrStatus_Ok || written >= size)
        return 0;
    return written;
}

// Writes object `object` as a stream of `body`, with the entries of `dictionary` beside its length and filter.
void emitStream(PdfWriter& p, uint32_t object, PrStringView dictionary, const uint8_t* body, size_t size,
        bool compressed) {
    p.offsets.data[object] = p.written;
    prTextBufferClear(&p.scratch);
    prTextBufferAppendU64(&p.scratch, object, 10);
    prTextBufferAppend(&p.scratch, " 0 obj\n<<");
    prTextBufferAppend(&p.scratch, dictionary);
    prTextBufferAppend(&p.scratch, " /Length ");
    prTextBufferAppendU64(&p.scratch, size, 10);
    if (compressed)
        prTextBufferAppend(&p.scratch, " /Filter /FlateDecode");
    prTextBufferAppend(&p.scratch, " >>\nstream\n");
    emit(p, p.scratch.data, p.scratch.size);
    emit(p, body, size);
    emit(p, "\nendstream\nendobj\n", 18);
}

// Writes object `object` as a stream of `data`, compressed if the writer has a codec, with the entries of `dictionary`
// beside its length and filter.
void writeStreamNow(PdfWriter& p, uint32_t object, PrStringView dictionary, const uint8_t* data, size_t size) {
    PrCompressCodec* codec = p.options.compress;
    size_t encoded = codec != nullptr ? compressStream(codec, data, size, p.bytes) : 0;
    if (encoded != 0)
        emitStream(p, object, dictionary, p.bytes.data, encoded, true);
    else
        emitStream(p, object, dictionary, data, size, false);
}

// The most streams, and the most bytes, that wait to be compressed on the pool's threads before they are written.
constexpr uint32_t kPendingStreams = 64;
constexpr size_t kPendingBytes = 16u << 20;

// Writes the stream as writeStreamNow does, or, if the writer has a pool, queues it to be compressed with others on the
// pool's threads and written in its turn.
void writeStream(PdfWriter& p, uint32_t object, PrStringView dictionary, const uint8_t* data, size_t size) {
    if (p.options.compress == nullptr || p.options.tasks == nullptr || size <= 32) {
        writeStreamNow(p, object, dictionary, data, size);
        return;
    }
    if (p.pendingCount == p.pending.count)
        p.pending.emplace() = PdfPending{};
    PdfPending& queued = p.pending.data[p.pendingCount++];
    queued.object = object;
    prTextBufferClear(&queued.dictionary);
    prTextBufferAppend(&queued.dictionary, PrStringView{dictionary.data, uint32_t(dictionary.size)});
    queued.data.clear();
    queued.data.append(data, uint32_t(size));
    p.pendingBytes += size;
    if (p.pendingCount == kPendingStreams || p.pendingBytes >= kPendingBytes)
        flushPending(p);
}

// Compresses the queued streams on the pool's threads, and writes them in the order that they were queued.
void flushPending(PdfWriter& p) {
    if (p.pendingCount == 0)
        return;
    PrTaskPool* tasks = p.options.tasks;
    prTaskPoolRun(tasks, p.pendingCount, prTaskPoolWidth(tasks), [](void* PR_NULLABLE context, uint32_t index, uint32_t) {
        PdfWriter& w = *static_cast<PdfWriter*>(context);
        PdfPending& queued = w.pending.data[index];
        queued.encodedSize = compressStream(w.options.compress, queued.data.data, queued.data.count, queued.encoded);
    }, &p);
    for (uint32_t i = 0; i < p.pendingCount; ++i) {
        PdfPending& queued = p.pending.data[i];
        PrStringView dictionary{queued.dictionary.data, queued.dictionary.size};
        if (queued.encodedSize != 0)
            emitStream(p, queued.object, dictionary, queued.encoded.data, queued.encodedSize, true);
        else
            emitStream(p, queued.object, dictionary, queued.data.data, queued.data.count, false);
    }
    p.pendingCount = 0;
    p.pendingBytes = 0;
}

void literal(PrTextBuffer& out, PrStringView text);

// Appends a text string (ISO 32000-2, 7.9.2.2): printable ASCII as a literal string, which PDFDocEncoding reads as the
// same characters, and any other text as UTF-16BE with a byte order mark, in hexadecimal.
void textString(PrTextBuffer& out, PrStringView text) {
    bool printable = true;
    for (size_t i = 0; i < text.size && printable; ++i)
        printable = text.data[i] >= 0x20 && text.data[i] < 0x7f;
    if (printable) {
        literal(out, text);
        return;
    }
    prTextBufferAppendChar(&out, '<');
    prTextBufferAppend(&out, "FEFF");
    for (size_t at = 0; at < text.size;) {
        uint32_t c = 0;
        uint32_t size = prUtf8DecodeScalar(text, at, &c);
        at += size != 0 ? size : 1;
        if (c >= 0x10000) {
            c -= 0x10000;
            drawSharedAppendHex(out, 0xD800 + (c >> 10), 2);
            drawSharedAppendHex(out, 0xDC00 + (c & 0x3ff), 2);
        } else {
            drawSharedAppendHex(out, c, 2);
        }
    }
    prTextBufferAppendChar(&out, '>');
}

// Appends a name, escaping the bytes that a name cannot hold as they are (ISO 32000-2, 7.3.5).
void name(PrTextBuffer& out, PrStringView text) {
    prTextBufferAppendChar(&out, '/');
    for (size_t i = 0; i < text.size; ++i) {
        uint8_t c = uint8_t(text.data[i]);
        if (c < 0x21 || c > 0x7e || c == '#' || c == '/' || c == '(' || c == ')' || c == '<' || c == '>' ||
                c == '[' || c == ']' || c == '{' || c == '}' || c == '%') {
            prTextBufferAppendChar(&out, '#');
            drawSharedAppendHex(out, c, 1);
        } else {
            prTextBufferAppendChar(&out, char(c));
        }
    }
}

// Appends a string of bytes as a literal string, escaped (ISO 32000-2, 7.3.4.2).
void literal(PrTextBuffer& out, PrStringView text) {
    prTextBufferAppendChar(&out, '(');
    for (size_t i = 0; i < text.size; ++i) {
        char c = text.data[i];
        if (c == '(' || c == ')' || c == '\\')
            prTextBufferAppendChar(&out, '\\');
        prTextBufferAppendChar(&out, c);
    }
    prTextBufferAppendChar(&out, ')');
}

void matrix(PrTextBuffer& out, PrAffine2 t) {
    float values[6] = {t.a, t.b, t.c, t.d, t.e, t.f};
    for (uint32_t i = 0; i < 6; ++i) {
        drawSharedAppendNumber(out, double(values[i]));
        prTextBufferAppendChar(&out, ' ');
    }
}

void rect(PrTextBuffer& out, PrRect r) {
    prTextBufferAppendChar(&out, '[');
    drawSharedAppendNumber(out, double(r.left));
    prTextBufferAppendChar(&out, ' ');
    drawSharedAppendNumber(out, double(r.top));
    prTextBufferAppendChar(&out, ' ');
    drawSharedAppendNumber(out, double(r.right));
    prTextBufferAppendChar(&out, ' ');
    drawSharedAppendNumber(out, double(r.bottom));
    prTextBufferAppendChar(&out, ']');
}

// Returns a rect of page px in the page's default space, in points with y up.
PrRect pageRect(const PdfPage& page, PrRect r) {
    double height = double(page.extent.y);
    return PrRect{
        float(double(r.left) * kPointsPerPx), float((height - double(r.bottom)) * kPointsPerPx),
        float(double(r.right) * kPointsPerPx), float((height - double(r.top)) * kPointsPerPx),
    };
}

// ---------------------------------------------------------------------------------------------------------------------
// Content

PdfStream& current(PdfWriter& p) {
    return p.streams.data[p.streamDepth - 1];
}

// The current stream's content, after the text object that it has open, for an operator that a text object may not hold
// (ISO 32000-2, 8.2, figure 9).
PrTextBuffer& content(PdfWriter& p) {
    PdfStream& s = current(p);
    if (s.text) {
        prTextBufferAppend(&s.content, "ET\n");
        s.text = false;
    }
    return s.content;
}

// Closes the stream's artifact sequence, if it has one open.
void endArtifact(PdfWriter& p) {
    PdfStream& s = current(p);
    if (!s.artifact)
        return;
    prTextBufferAppend(&content(p), "EMC\n");
    s.artifact = false;
}

// Saves or restores the graphics state, with the parameters that the writer tracks.
void save(PdfWriter& p) {
    endArtifact(p);
    prTextBufferAppend(&content(p), "q\n");
    PdfStream& s = current(p);
    s.saved.push(s.state);
}

void restore(PdfWriter& p) {
    endArtifact(p);
    prTextBufferAppend(&content(p), "Q\n");
    PdfStream& s = current(p);
    s.state = s.saved.last();
    s.saved.pop();
}

// Appends a path's segments, mapped by `transform`.
void pathOps(PrTextBuffer& out, const PrPath* path, PrAffine2 transform) {
    const PrPathVerb* verbs = prPathVerbs(path);
    const PrVec2* points = prPathPoints(path);
    uint32_t at = 0;
    PrVec2 last{0, 0};
    auto point = [&]() {
        PrVec2 q = prAffine2MapPoint(transform, points[at++]);
        drawSharedAppendNumber(out, double(q.x));
        prTextBufferAppendChar(&out, ' ');
        drawSharedAppendNumber(out, double(q.y));
        prTextBufferAppendChar(&out, ' ');
        return q;
    };
    uint32_t count = prPathVerbCount(path);
    for (uint32_t i = 0; i < count; ++i) {
        switch (verbs[i]) {
            case PrPathVerb_Move:
                // A closed subpath of four sides along the axes, starting along x, is the rectangle that re appends
                // (ISO 32000-2, 8.5.2.1), which goes around it in the same direction.
                if (i + 4 < count && verbs[i + 1] == PrPathVerb_Line && verbs[i + 2] == PrPathVerb_Line &&
                        verbs[i + 3] == PrPathVerb_Line && verbs[i + 4] == PrPathVerb_Close) {
                    PrVec2 c[4];
                    for (uint32_t k = 0; k < 4; ++k)
                        c[k] = prAffine2MapPoint(transform, points[at + k]);
                    if (c[0].y == c[1].y && c[1].x == c[2].x && c[2].y == c[3].y && c[3].x == c[0].x) {
                        float values[4] = {c[0].x, c[0].y, c[2].x - c[0].x, c[2].y - c[0].y};
                        for (float v : values) {
                            drawSharedAppendNumber(out, double(v));
                            prTextBufferAppendChar(&out, ' ');
                        }
                        prTextBufferAppend(&out, "re\n");
                        at += 4;
                        last = c[0];
                        i += 4;
                        break;
                    }
                }
                last = point();
                prTextBufferAppend(&out, "m\n");
                break;
            case PrPathVerb_Line:
                last = point();
                prTextBufferAppend(&out, "l\n");
                break;
            case PrPathVerb_Quad: {
                // A quadratic is the cubic whose controls lie two thirds of the way to its control.
                PrVec2 c = prAffine2MapPoint(transform, points[at]);
                PrVec2 e = prAffine2MapPoint(transform, points[at + 1]);
                at += 2;
                PrVec2 c1{last.x + (c.x - last.x) * 2 / 3, last.y + (c.y - last.y) * 2 / 3};
                PrVec2 c2{e.x + (c.x - e.x) * 2 / 3, e.y + (c.y - e.y) * 2 / 3};
                PrVec2 values[3] = {c1, c2, e};
                for (const PrVec2& v : values) {
                    drawSharedAppendNumber(out, double(v.x));
                    prTextBufferAppendChar(&out, ' ');
                    drawSharedAppendNumber(out, double(v.y));
                    prTextBufferAppendChar(&out, ' ');
                }
                prTextBufferAppend(&out, "c\n");
                last = e;
                break;
            }
            case PrPathVerb_Cubic:
                point();
                point();
                last = point();
                prTextBufferAppend(&out, "c\n");
                break;
            default:
                prTextBufferAppend(&out, "h\n");
                break;
        }
    }
}

bool tagged(const PdfWriter& p) {
    return p.structure != nullptr;
}

// Returns true if the content written now belongs to a mark that the structure names.
bool inTaggedMark(const PdfWriter& p) {
    for (uint32_t i = p.openMarks.count; i-- > 0;) {
        if (p.openMarks.data[i].mark != kHashMissing)
            return true;
    }
    return false;
}

// Brackets an operation that paints: as an artifact if it belongs to no tagged mark of a tagged document. The
// artifact's sequence stays open for the artifacts that follow it.
void beginPainting(PdfWriter& p) {
    if (!tagged(p) || inTaggedMark(p) || current(p).artifact)
        return;
    prTextBufferAppend(&content(p), "/Artifact BMC\n");
    current(p).artifact = true;
}

void endPainting(PdfWriter& p) {
    (void)p;
}

constexpr PrStringView kBlendModes[PrDrawBlendMode_Count] = {
    "Normal", "Multiply", "Screen", "Overlay", "Darken", "Lighten", "ColorDodge", "ColorBurn", "HardLight",
    "SoftLight", "Difference", "Exclusion", "", "Hue", "Saturation", "Color", "Luminosity",
};

// Selects the graphics state of the alphas and the blend mode, unless the stream has it selected. A text object may
// hold the operator, so the stream's text object stays open.
void state(PdfWriter& p, float fill, float stroke, PrDrawBlendMode blendMode) {
    fill = prF32Clamp(fill, 0, 1);
    stroke = prF32Clamp(stroke, 0, 1);
    PdfGraphicsState& tracked = current(p).state;
    bool initial = fill == 1 && stroke == 1 && blendMode == PrDrawBlendMode_Normal;
    if (initial && tracked.extGState == kHashMissing)
        return;
    if (tracked.extGState != kHashMissing) {
        const PdfState& s = p.states.data[tracked.extGState];
        if (s.fill == fill && s.stroke == stroke && s.blendMode == blendMode)
            return;
    }
    uint32_t found = kHashMissing;
    for (uint32_t i = 0; i < p.states.count && found == kHashMissing; ++i) {
        const PdfState& s = p.states.data[i];
        if (s.fill == fill && s.stroke == stroke && s.blendMode == blendMode)
            found = i;
    }
    if (found == kHashMissing) {
        found = p.states.count;
        uint32_t object = allocate(p);
        p.states.push(PdfState{fill, stroke, blendMode, object});
        prTextBufferAppend(&p.out, "<< /Type /ExtGState /ca ");
        drawSharedAppendNumber(p.out, double(fill));
        prTextBufferAppend(&p.out, " /CA ");
        drawSharedAppendNumber(p.out, double(stroke));
        if (blendMode != PrDrawBlendMode_Normal) {
            prTextBufferAppend(&p.out, " /BM /");
            prTextBufferAppend(&p.out, kBlendModes[blendMode]);
        }
        prTextBufferAppend(&p.out, " >>");
        writeObject(p, object);
    }
    PrTextBuffer& out = current(p).content;
    prTextBufferAppend(&out, "/G");
    prTextBufferAppendU64(&out, found, 10);
    prTextBufferAppend(&out, " gs\n");
    current(p).state.extGState = initial ? kHashMissing : found;
}

// Writes the function of a gradient's stops: a stitching function of linear pieces (ISO 32000-2, 7.10.4).
void gradientFunction(PrTextBuffer& out, const RawArray<DrawSharedStop>& stops) {
    auto piece = [&](const PrColor& a, const PrColor& b) {
        prTextBufferAppend(&out, "<< /FunctionType 2 /Domain [0 1] /N 1 /C0 [");
        drawSharedAppendNumber(out, double(a.r));
        prTextBufferAppendChar(&out, ' ');
        drawSharedAppendNumber(out, double(a.g));
        prTextBufferAppendChar(&out, ' ');
        drawSharedAppendNumber(out, double(a.b));
        prTextBufferAppend(&out, "] /C1 [");
        drawSharedAppendNumber(out, double(b.r));
        prTextBufferAppendChar(&out, ' ');
        drawSharedAppendNumber(out, double(b.g));
        prTextBufferAppendChar(&out, ' ');
        drawSharedAppendNumber(out, double(b.b));
        prTextBufferAppend(&out, "] >>");
    };
    if (stops.count == 1) {
        piece(stops.data[0].color, stops.data[0].color);
        return;
    }
    if (stops.count == 2 && stops.data[0].offset == 0 && stops.data[1].offset == 1) {
        piece(stops.data[0].color, stops.data[1].color);
        return;
    }
    // The pieces run between consecutive stops, from 0 to 1, with the end colors held before the first stop and after
    // the last.
    RawArray<DrawSharedStop> all;
    if (stops.data[0].offset > 0)
        all.push(DrawSharedStop{0, stops.data[0].color});
    all.append(stops.data, stops.count);
    if (stops.last().offset < 1)
        all.push(DrawSharedStop{1, stops.last().color});
    prTextBufferAppend(&out, "<< /FunctionType 3 /Domain [0 1] /Functions [");
    for (uint32_t i = 0; i + 1 < all.count; ++i)
        piece(all.data[i].color, all.data[i + 1].color);
    prTextBufferAppend(&out, "] /Bounds [");
    for (uint32_t i = 1; i + 1 < all.count; ++i) {
        drawSharedAppendNumber(out, double(all.data[i].offset));
        prTextBufferAppendChar(&out, ' ');
    }
    prTextBufferAppend(&out, "] /Encode [");
    for (uint32_t i = 0; i + 1 < all.count; ++i)
        prTextBufferAppend(&out, "0 1 ");
    prTextBufferAppend(&out, "] >>");
    all.reset();
}

// Defines a shading pattern of the gradient of `paint`, whose geometry `toPattern` maps into the pattern space of the
// current stream, and returns its index. Returns the constant alpha of the gradient's colors through `alpha`.
uint32_t definePattern(PdfWriter& p, const DrawSharedPaint& paint, PrAffine2 toPattern, float* alpha) {
    const PrDrawBrush* brush = paint.brush;
    drawSharedGradientStops(brush, paint.color, p.stops);
    *alpha = p.stops.count != 0 ? p.stops.data[0].color.a : 1;
    uint32_t object = allocate(p);
    uint32_t index = p.patterns.count;
    p.patterns.push(object);
    PrTextBuffer& out = p.out;
    prTextBufferAppend(&out, "<< /Type /Pattern /PatternType 2 /Matrix [");
    matrix(out, prAffine2Concat(paint.brushToUser, toPattern));
    prTextBufferAppend(&out, "] /Shading << /ColorSpace /DeviceRGB ");
    if (brush->kind == PrDrawBrushKind_Linear) {
        prTextBufferAppend(&out, "/ShadingType 2 /Coords [");
        float coords[4] = {brush->linear.start.x, brush->linear.start.y, brush->linear.end.x, brush->linear.end.y};
        for (float c : coords) {
            drawSharedAppendNumber(out, double(c));
            prTextBufferAppendChar(&out, ' ');
        }
    } else {
        prTextBufferAppend(&out, "/ShadingType 3 /Coords [");
        const PrDrawRadialGradient& r = brush->radial;
        float coords[6] = {r.start.x, r.start.y, r.startRadius, r.end.x, r.end.y, r.endRadius};
        for (float c : coords) {
            drawSharedAppendNumber(out, double(c));
            prTextBufferAppendChar(&out, ' ');
        }
    }
    prTextBufferAppend(&out, "] /Function ");
    gradientFunction(out, p.stops);
    prTextBufferAppend(&out, brush->extend == PrDrawExtend_Pad ? " /Extend [true true] >> >>" : " /Extend [false false] >> >>");
    writeObject(p, object);
    return index;
}

// The transform from the current stream's space to its pattern space: the page's default space for a page's stream, and
// the form's own space for a form, whose space is the page's in px.
PrAffine2 toPatternSpace(const PdfWriter& p) {
    if (p.streams.data[p.streamDepth - 1].form)
        return prAffine2Identity();
    double height = double(p.pages.last().extent.y);
    return PrAffine2{float(kPointsPerPx), 0, 0, -float(kPointsPerPx), 0, float(height * kPointsPerPx)};
}

// Selects the paint as the fill or the stroke color, unless the stream has it selected, and returns its alpha. A text
// object may hold the operators, so the stream's text object stays open.
float selectPaint(PdfWriter& p, const DrawSharedPaint& paint, PrAffine2 transform, bool stroke) {
    PdfStream& s = current(p);
    PrTextBuffer& out = s.content;
    if (paint.brush != nullptr) {
        float alpha = 1;
        uint32_t pattern = definePattern(p, paint, prAffine2Concat(transform, toPatternSpace(p)), &alpha);
        prTextBufferAppend(&out, stroke ? "/Pattern CS /P" : "/Pattern cs /P");
        prTextBufferAppendU64(&out, pattern, 10);
        prTextBufferAppend(&out, stroke ? " SCN\n" : " scn\n");
        if (!stroke)
            s.state.fillKnown = false;
        return alpha;
    }
    PrColor c = drawSharedClamp(paint.color);
    if (!stroke) {
        const PrColor& known = s.state.fill;
        if (s.state.fillKnown && known.r == c.r && known.g == c.g && known.b == c.b)
            return c.a;
        s.state.fill = c;
        s.state.fillKnown = true;
    }
    drawSharedAppendNumber(out, double(c.r));
    prTextBufferAppendChar(&out, ' ');
    drawSharedAppendNumber(out, double(c.g));
    prTextBufferAppendChar(&out, ' ');
    drawSharedAppendNumber(out, double(c.b));
    prTextBufferAppend(&out, stroke ? " RG\n" : " rg\n");
    return c.a;
}

void strokeState(PrTextBuffer& out, const PrDrawStroke* stroke) {
    const PrPathStroke& s = stroke->shape;
    drawSharedAppendNumber(out, double(s.width));
    prTextBufferAppend(&out, " w\n");
    uint32_t cap = s.startCap == PrPathCap_Round ? 1 : s.startCap == PrPathCap_Square ? 2 : 0;
    prTextBufferAppendU64(&out, cap, 10);
    prTextBufferAppend(&out, " J\n");
    uint32_t join = s.join == PrPathJoin_Round ? 1 : s.join == PrPathJoin_Bevel ? 2 : 0;
    prTextBufferAppendU64(&out, join, 10);
    prTextBufferAppend(&out, " j\n");
    drawSharedAppendNumber(out, double(prF32Max(s.miterLimit, 1)));
    prTextBufferAppend(&out, " M\n");
    if (s.dashes != nullptr && s.dashCount != 0) {
        prTextBufferAppendChar(&out, '[');
        for (uint32_t i = 0; i < s.dashCount; ++i) {
            drawSharedAppendNumber(out, double(prF32Max(s.dashes[i], 0)));
            prTextBufferAppendChar(&out, ' ');
        }
        prTextBufferAppend(&out, "] ");
        drawSharedAppendNumber(out, double(s.dashOffset));
        prTextBufferAppend(&out, " d\n");
    }
}

bool pdfGradient(void* PR_NULLABLE context, const PrDrawBrush* brush) {
    (void)context;
    if (brush->kind != PrDrawBrushKind_Linear && brush->kind != PrDrawBrushKind_Radial)
        return false;
    if (brush->extend != PrDrawExtend_Pad && brush->extend != PrDrawExtend_None)
        return false;
    // A shading has no alpha of its own, so a gradient whose stops vary in alpha is drawn otherwise.
    for (uint32_t i = 0; i < brush->stopCount; ++i) {
        if (prFloat16ToFloat(brush->stops[i].color.a) < 1)
            return false;
    }
    return true;
}

bool pdfBlendMode(void* PR_NULLABLE context, PrDrawBlendMode blendMode) {
    (void)context;
    return blendMode != PrDrawBlendMode_HardMix;
}

void pdfFill(void* PR_NULLABLE context, const PrPath* path, PrPathFillRule rule, PrAffine2 transform,
        const DrawSharedPaint* paint) {
    PdfWriter& p = *static_cast<PdfWriter*>(context);
    beginPainting(p);
    content(p);
    float alpha = selectPaint(p, *paint, transform, false);
    state(p, alpha, 1, paint->blendMode);
    pathOps(content(p), path, transform);
    prTextBufferAppend(&content(p), rule == PrPathFillRule_EvenOdd ? "f*\n" : "f\n");
    endPainting(p);
}

void pdfStroke(void* PR_NULLABLE context, const PrPath* path, const PrDrawStroke* stroke, PrAffine2 transform,
        const DrawSharedPaint* paint) {
    PdfWriter& p = *static_cast<PdfWriter*>(context);
    beginPainting(p);
    save(p);
    matrix(content(p), transform);
    prTextBufferAppend(&content(p), "cm\n");
    float alpha = selectPaint(p, *paint, transform, true);
    state(p, 1, alpha, paint->blendMode);
    strokeState(content(p), stroke);
    pathOps(content(p), path, prAffine2Identity());
    prTextBufferAppend(&content(p), "S\n");
    restore(p);
    endPainting(p);
}

// Returns the index of the image's object, which is written the first time that the image is drawn.
uint32_t imageObject(PdfWriter& p, const PrDrawImage* image) {
    uint64_t id = prDrawImageId(image);
    uint32_t found = p.imageIndex.find(id, [&](uint32_t at) { return p.images.data[at].id; });
    if (found != kHashMissing)
        return found;
    found = p.images.count;
    PrUVec2 extent = prDrawImageExtent(image);
    RawArray<uint8_t> rgba;
    bool translucent = drawSharedImageRgba8(image, rgba);
    uint32_t pixels = extent.x * extent.y;
    RawArray<uint8_t> rgb;
    rgb.resize(pixels * 3);
    p.alpha.resize(pixels);
    for (uint32_t i = 0; i < pixels; ++i) {
        rgb.data[i * 3] = rgba.data[i * 4];
        rgb.data[i * 3 + 1] = rgba.data[i * 4 + 1];
        rgb.data[i * 3 + 2] = rgba.data[i * 4 + 2];
        p.alpha.data[i] = rgba.data[i * 4 + 3];
    }
    rgba.reset();
    uint32_t mask = 0;
    if (translucent) {
        mask = allocate(p);
        PrTextBuffer dictionary{};
        prTextBufferAppend(&dictionary, " /Type /XObject /Subtype /Image /ColorSpace /DeviceGray /BitsPerComponent 8 /Width ");
        prTextBufferAppendU64(&dictionary, extent.x, 10);
        prTextBufferAppend(&dictionary, " /Height ");
        prTextBufferAppendU64(&dictionary, extent.y, 10);
        writeStream(p, mask, PrStringView{dictionary.data, dictionary.size}, p.alpha.data, pixels);
        prTextBufferReset(&dictionary);
    }
    uint32_t object = allocate(p);
    PrTextBuffer dictionary{};
    prTextBufferAppend(&dictionary, " /Type /XObject /Subtype /Image /ColorSpace /DeviceRGB /BitsPerComponent 8 /Interpolate true");
    prTextBufferAppend(&dictionary, " /Width ");
    prTextBufferAppendU64(&dictionary, extent.x, 10);
    prTextBufferAppend(&dictionary, " /Height ");
    prTextBufferAppendU64(&dictionary, extent.y, 10);
    if (mask != 0) {
        prTextBufferAppend(&dictionary, " /SMask ");
        reference(dictionary, mask);
    }
    writeStream(p, object, PrStringView{dictionary.data, dictionary.size}, rgb.data, pixels * 3);
    prTextBufferReset(&dictionary);
    rgb.reset();
    p.images.push(PdfImage{id, object});
    p.imageIndex.insert(id, found);
    return found;
}

void pdfImage(void* PR_NULLABLE context, const PrDrawImage* image, PrRect source, PrRect destination,
        PrAffine2 transform, float opacity) {
    PdfWriter& p = *static_cast<PdfWriter*>(context);
    uint32_t index = imageObject(p, image);
    PrUVec2 extent = prDrawImageExtent(image);
    float sx = (destination.right - destination.left) / (source.right - source.left);
    float sy = (destination.bottom - destination.top) / (source.bottom - source.top);
    if (!prF32IsFinite(sx) || !prF32IsFinite(sy))
        return;
    beginPainting(p);
    save(p);
    PrTextBuffer& out = content(p);
    matrix(out, transform);
    prTextBufferAppend(&out, "cm\n");
    state(p, opacity, 1, PrDrawBlendMode_Normal);
    // The source is cut from the image by a clip to the destination, and the whole image is scaled so that the source
    // lands on it. An image fills the unit square from its bottom row up, so its rows are flipped to run down.
    drawSharedAppendNumber(out, double(destination.left));
    prTextBufferAppendChar(&out, ' ');
    drawSharedAppendNumber(out, double(destination.top));
    prTextBufferAppendChar(&out, ' ');
    drawSharedAppendNumber(out, double(destination.right - destination.left));
    prTextBufferAppendChar(&out, ' ');
    drawSharedAppendNumber(out, double(destination.bottom - destination.top));
    prTextBufferAppend(&out, " re W n\n");
    float width = float(extent.x) * sx;
    float height = float(extent.y) * sy;
    float left = destination.left - source.left * sx;
    float top = destination.top - source.top * sy;
    matrix(out, PrAffine2{width, 0, 0, -height, left, top + height});
    prTextBufferAppend(&out, "cm\n/I");
    prTextBufferAppendU64(&out, index, 10);
    prTextBufferAppend(&out, " Do\n");
    restore(p);
    endPainting(p);
}

// ---------------------------------------------------------------------------------------------------------------------
// Text

PdfFont& fontOf(PdfWriter& p, const PrFont* font) {
    uint64_t id = prFontId(font);
    uint32_t found = p.fontIndex.find(id, [&](uint32_t at) { return prFontId(p.fonts.data[at].font); });
    if (found == kHashMissing) {
        found = p.fonts.count;
        PdfFont& made = p.fonts.emplace();
        made = PdfFont{};
        prFontRetain(font);
        made.font = font;
        made.format = prFontSubsetFormat(font);
        made.object = made.format != PrFontSubsetFormat_None ? allocate(p) : 0;
        uint32_t count = prFontGlyphCount(font);
        made.used.resizeZeroed(count);
        made.widths.resizeZeroed(count);
        made.unicodeStart.resizeZeroed(count);
        made.unicodeSize.resizeZeroed(count);
        prTextBufferAppendChar(&made.unicode, 0);
        p.fontIndex.insert(id, found);
    }
    return p.fonts.data[found];
}

// Finds the characters of each of the run's glyphs: from its cluster up to the next greater cluster of the run, or to
// the end of the run's text. Glyphs of one cluster share its characters, which the first of them in the run carries,
// and the others have none. Writes where each glyph's characters start in the run's text to `p.glyphStarts`, and their
// size to `p.glyphSizes`, by glyph. A run whose clusters are in the order of its text takes linear time, and any other
// run takes the time of sorting its clusters.
void glyphTexts(PdfWriter& p, const PrFontGlyphRun* run) {
    uint32_t count = run->glyphCount;
    uint32_t size = uint32_t(run->text.size);
    const uint32_t* clusters = run->clusters;
    p.glyphStarts.resize(count);
    p.glyphSizes.resize(count);
    bool ordered = true;
    for (uint32_t i = 1; i < count && ordered; ++i)
        ordered = clusters[i] >= clusters[i - 1];
    auto set = [&](uint32_t glyph, uint32_t start, uint32_t end) {
        bool valid = start < end && end <= size;
        p.glyphStarts.data[glyph] = valid ? start : 0;
        p.glyphSizes.data[glyph] = valid ? end - start : 0;
    };
    if (ordered) {
        uint32_t end = size;
        for (uint32_t i = count; i-- > 0;) {
            if (i + 1 < count && clusters[i + 1] != clusters[i])
                end = clusters[i + 1];
            bool first = i == 0 || clusters[i - 1] != clusters[i];
            set(i, clusters[i], first ? end : clusters[i]);
        }
        return;
    }
    // The glyphs by cluster, and by their order in the run within a cluster.
    p.clusterOrder.resize(count);
    for (uint32_t i = 0; i < count; ++i)
        p.clusterOrder.data[i] = uint64_t(clusters[i]) << 32 | i;
    sortItems(p.clusterOrder.span(), [](uint64_t a, uint64_t b) { return a < b; });
    for (uint32_t at = 0; at < count;) {
        uint32_t cluster = uint32_t(p.clusterOrder.data[at] >> 32);
        uint32_t past = at + 1;
        while (past < count && uint32_t(p.clusterOrder.data[past] >> 32) == cluster)
            ++past;
        uint32_t end = past < count ? uint32_t(p.clusterOrder.data[past] >> 32) : size;
        for (uint32_t k = at; k < past; ++k)
            set(uint32_t(p.clusterOrder.data[k]), cluster, k == at ? end : cluster);
        at = past;
    }
}

// The advance of a glyph in the font's file, in thousandths of an em before size-adjust, as the font's widths (W) give
// it.
double glyphWidth(const PrFont* font, uint16_t glyph) {
    float adjust = prFontSizeAdjust(font);
    return drawSharedRound(double(prFontGlyphAdvance(font, glyph, 0) / adjust * 1000), 2);
}

// Marks the run's glyphs as used, and records their characters in the font's map back to characters, each glyph's first
// characters winning. Returns true if the map gives the run's text back, so that the run needs no replacement text.
bool mapText(PdfWriter& p, PdfFont& font, const PrFontGlyphRun* run) {
    bool hasText = run->clusters != nullptr && run->text.size != 0;
    if (hasText)
        glyphTexts(p, run);
    bool same = hasText;
    uint32_t previous = 0;
    for (uint32_t i = 0; i < run->glyphCount; ++i) {
        uint16_t glyph = run->glyphs[i].id;
        if (glyph >= font.used.count)
            continue;
        if (font.used.data[glyph] == 0) {
            font.used.data[glyph] = 1;
            font.widths.data[glyph] = glyphWidth(font.font, glyph);
        }
        PrStringView text{};
        if (hasText) {
            text = PrStringView{run->text.data + p.glyphStarts.data[i], p.glyphSizes.data[i]};
            // A run whose clusters fall in the order of its text reads back in that order.
            if (i != 0 && run->clusters[i] < previous)
                same = false;
            previous = run->clusters[i];
        }
        if (font.unicodeStart.data[glyph] == 0) {
            if (text.size != 0) {
                font.unicodeStart.data[glyph] = font.unicode.size;
                font.unicodeSize.data[glyph] = uint32_t(text.size);
                prTextBufferAppend(&font.unicode, PrStringView{text.data, uint32_t(text.size)});
            }
            continue;
        }
        PrStringView mapped{font.unicode.data + font.unicodeStart.data[glyph], font.unicodeSize.data[glyph]};
        if (!(mapped == text))
            same = false;
    }
    return same;
}

// Opens a span whose replacement text is `text` (ISO 32000-2, 14.9.4).
void beginActualText(PdfWriter& p, PrStringView text) {
    prTextBufferAppend(&content(p), "/Span << /ActualText ");
    textString(content(p), text);
    prTextBufferAppend(&content(p), " >> BDC\n");
}

void glyphOutlines(PdfWriter& p, const PrFontGlyphRun* run, PrAffine2 transform, const PrDrawStroke* PR_NULLABLE stroke,
        const DrawSharedPaint* paint) {
    save(p);
    PrTextBuffer& out = content(p);
    matrix(out, transform);
    prTextBufferAppend(&out, "cm\n");
    float alpha = selectPaint(p, *paint, transform, stroke != nullptr);
    if (stroke != nullptr) {
        state(p, 1, alpha, paint->blendMode);
        strokeState(content(p), stroke);
    } else {
        state(p, alpha, 1, paint->blendMode);
    }
    for (uint32_t i = 0; i < run->glyphCount; ++i) {
        prPathClear(p.path);
        if (!prDrawGlyphPath(run, i, nullptr, p.path))
            continue;
        pathOps(content(p), p.path, prAffine2Identity());
        prTextBufferAppend(&content(p), stroke != nullptr ? "S\n" : "f\n");
    }
    restore(p);
}

// Returns the matrix with each of its entries as `matrix` writes it.
PrAffine2 writtenMatrix(PrAffine2 t) {
    return PrAffine2{
        float(drawSharedRound(double(t.a), 4)), float(drawSharedRound(double(t.b), 4)),
        float(drawSharedRound(double(t.c), 4)), float(drawSharedRound(double(t.d), 4)),
        float(drawSharedRound(double(t.e), 4)), float(drawSharedRound(double(t.f), 4)),
    };
}

// Returns true if the text matrix `next`, as written, has the linear part of the stream's text line matrix, and that
// part can be inverted, so that a move by Td from the line reaches the origin of `next`.
bool continuesLine(const PdfStream& s, PrAffine2 next) {
    PrAffine2 written = writtenMatrix(next);
    const PrAffine2& line = s.line;
    double det = double(line.a) * double(line.d) - double(line.b) * double(line.c);
    return written.a == line.a && written.b == line.b && written.c == line.c && written.d == line.d && det != 0 &&
        prF64IsFinite(det);
}

// Returns the point (x, y) of the stream's space in the space of its text line, which the text line matrix maps to the
// stream's space. The line's linear part must be invertible.
PrDVec2 onLine(const PdfStream& s, double x, double y) {
    const PrAffine2& l = s.line;
    double dx = x - double(l.e);
    double dy = y - double(l.f);
    double det = double(l.a) * double(l.d) - double(l.b) * double(l.c);
    return PrDVec2{(dx * double(l.d) - dy * double(l.c)) / det, (dy * double(l.a) - dx * double(l.b)) / det};
}

// Starts a text line at the origin of the text matrix `next`: by Td from the line before if `next` continues it, and by
// Tm otherwise. The stream must have a text object open.
void moveLine(PdfStream& s, PrAffine2 next) {
    PrTextBuffer& out = s.content;
    if (!continuesLine(s, next)) {
        s.line = writtenMatrix(next);
        matrix(out, s.line);
        prTextBufferAppend(&out, "Tm\n");
        return;
    }
    PrDVec2 t = onLine(s, double(next.e), double(next.f));
    double tx = drawSharedRound(t.x, 4);
    double ty = drawSharedRound(t.y, 4);
    drawSharedAppendNumber(out, tx);
    prTextBufferAppendChar(&out, ' ');
    drawSharedAppendNumber(out, ty);
    prTextBufferAppend(&out, " Td\n");
    PrAffine2& l = s.line;
    l.e = float(double(l.e) + tx * double(l.a) + ty * double(l.c));
    l.f = float(double(l.f) + tx * double(l.b) + ty * double(l.d));
}

// How far, in the space of a text line, a glyph may lie off the line before it starts a line of its own.
constexpr double kLineTolerance = 1e-3;

// Returns the character spacing (Tc) of the glyphs of the run from `first` that lie on the stream's text line, at the
// font size `size`: the most common gap, in the line's space, between where a glyph lies and where the width of the
// glyph before it leaves the text position, if at least half of the gaps and at least two have it. Letter spacing
// places glyphs so, and a reader that extracts the text counts character spacing as part of each glyph, not as a space
// between words.
double charSpacingOf(PdfWriter& p, const PdfFont& font, const PrFontGlyphRun* run, PrAffine2 toStream,
        uint32_t first, double size) {
    const PdfStream& s = current(p);
    p.gaps.clear();
    PrVec2 start = prAffine2MapPoint(toStream, run->glyphs[first].position);
    PrDVec2 before = onLine(s, double(start.x), double(start.y));
    bool spaced = false;
    for (uint32_t i = first + 1; i < run->glyphCount; ++i) {
        PrVec2 origin = prAffine2MapPoint(toStream, run->glyphs[i].position);
        PrDVec2 at = onLine(s, double(origin.x), double(origin.y));
        if (!(prF64Abs(at.y) <= kLineTolerance))
            break;
        uint16_t glyph = run->glyphs[i - 1].id;
        double width = glyph < font.used.count ? font.widths.data[glyph] : 1000;
        double gap = (at.x - before.x - width * size / 1000) * 1000 / size;
        if (prF64IsFinite(gap)) {
            p.gaps.push(int64_t(drawSharedRound(gap, 0)));
            spaced = spaced || p.gaps.last() != 0;
        }
        before = at;
    }
    if (!spaced)
        return 0;
    sortItems(p.gaps.span(), [](int64_t a, int64_t b) { return a < b; });
    int64_t common = 0;
    uint32_t most = 0;
    for (uint32_t i = 0; i < p.gaps.count;) {
        uint32_t end = i + 1;
        while (end < p.gaps.count && p.gaps.data[end] == p.gaps.data[i])
            ++end;
        if (end - i > most) {
            most = end - i;
            common = p.gaps.data[i];
        }
        i = end;
    }
    if (most < 2 || most * 2 < p.gaps.count)
        return 0;
    return drawSharedRound(double(common) * size / 1000, 4);
}

// Shows the run's glyphs in the stream's text object, opening one if it has none, in the document's font `fontNumber`
// and the rendering mode `mode`, with the run's transform `toStream`. The glyphs of a line are one TJ array, which
// adjusts the text position only where a glyph's place differs from where the font's width leaves it, and a glyph off
// the line starts another line by Td.
void showGlyphs(PdfWriter& p, PdfFont& font, uint32_t fontNumber, const PrFontGlyphRun* run, PrAffine2 toStream, uint8_t mode) {
    PdfStream& s = current(p);
    PrTextBuffer& out = s.content;
    if (!s.text) {
        prTextBufferAppend(&out, "BT\n");
        s.text = true;
        s.line = prAffine2Identity();
    }
    // The font's size is the run's em, which each glyph's text matrix divides out. A run whose em has no size shows
    // each glyph by a matrix of its own, at a size of one.
    float adjust = prFontSizeAdjust(run->font);
    double wanted = prF64Abs(double(run->size) * double(adjust));
    double size = drawSharedRound(wanted, 4);
    bool sized = prF64IsFinite(wanted) && size > 0;
    if (!sized)
        size = 1;
    if (s.state.font != fontNumber || s.state.fontSize != size) {
        prTextBufferAppend(&out, "/F");
        prTextBufferAppendU64(&out, fontNumber, 10);
        prTextBufferAppendChar(&out, ' ');
        drawSharedAppendNumber(out, size);
        prTextBufferAppend(&out, " Tf\n");
        s.state.font = fontNumber;
        s.state.fontSize = size;
    }
    if (s.state.renderMode != mode) {
        prTextBufferAppendU64(&out, mode, 10);
        prTextBufferAppend(&out, " Tr\n");
        s.state.renderMode = mode;
    }
    // A glyph's space has y up, and the run's has y down. Size-adjust scales the outlines that the font's file holds.
    PrAffine2 up{float(double(adjust) / size), 0, 0, float(-double(adjust) / size), 0, 0};
    bool each = run->transforms != nullptr || !sized;
    bool array = false;
    bool string = false;
    double x = 0;                       // the text position along the line, in the line's space
    auto close = [&]() {
        if (string)
            prTextBufferAppendChar(&out, '>');
        if (array)
            prTextBufferAppend(&out, "]TJ\n");
        array = false;
        string = false;
    };
    // Once the line has the run's linear part, which every glyph of a run without transforms of its own shares, a
    // glyph's origin alone finds its place on the line.
    bool linear = false;
    double spacing = 0;                 // the character spacing, which follows each glyph on the line
    for (uint32_t i = 0; i < run->glyphCount; ++i) {
        PrDVec2 at{0, 0};
        bool moved = !linear;
        if (linear) {
            PrVec2 origin = prAffine2MapPoint(toStream, run->glyphs[i].position);
            at = onLine(s, double(origin.x), double(origin.y));
            moved = !(prF64Abs(at.y) <= kLineTolerance);
        }
        if (moved) {
            PrAffine2 m = prAffine2Concat(prAffine2Concat(up, prDrawGlyphTransform(run, i)), toStream);
            close();
            moveLine(s, m);
            x = 0;
            linear = !each && continuesLine(s, m);
            spacing = linear ? charSpacingOf(p, font, run, toStream, i, size) : s.state.charSpacing;
            if (s.state.charSpacing != spacing) {
                drawSharedAppendNumber(out, spacing);
                prTextBufferAppend(&out, " Tc\n");
                s.state.charSpacing = spacing;
            }
        } else {
            // A number in a TJ array moves the text position back by thousandths of the font's size.
            double adjustment = drawSharedRound((x - at.x) * 1000 / size, 0);
            if (adjustment != 0 && prF64IsFinite(adjustment)) {
                if (string)
                    prTextBufferAppendChar(&out, '>');
                string = false;
                if (!array)
                    prTextBufferAppendChar(&out, '[');
                array = true;
                if (adjustment < 0)
                    prTextBufferAppendChar(&out, '-');
                prTextBufferAppendU64(&out, uint64_t(prF64Abs(adjustment)), 10);
                x -= adjustment * size / 1000;
            }
        }
        if (!array)
            prTextBufferAppendChar(&out, '[');
        if (!string)
            prTextBufferAppendChar(&out, '<');
        array = true;
        string = true;
        uint16_t glyph = run->glyphs[i].id;
        drawSharedAppendHex(out, glyph, 2);
        // A glyph that the font's widths do not list has the default width, 1000.
        double width = glyph < font.used.count ? font.widths.data[glyph] : 1000;
        x += width * size / 1000 + spacing;
    }
    close();
}

void pdfGlyphs(void* PR_NULLABLE context, const PrFontGlyphRun* run, PrAffine2 transform,
        const PrDrawStroke* PR_NULLABLE stroke, const DrawSharedPaint* paint) {
    PdfWriter& p = *static_cast<PdfWriter*>(context);
    PdfFont& font = fontOf(p, run->font);
    bool replaced = run->text.size != 0;
    beginPainting(p);
    if (font.format == PrFontSubsetFormat_None) {
        if (replaced)
            beginActualText(p, run->text);
        glyphOutlines(p, run, transform, stroke, paint);
        if (replaced)
            prTextBufferAppend(&content(p), "EMC\n");
        endPainting(p);
        return;
    }
    uint32_t fontNumber = uint32_t(&font - p.fonts.data);
    bool same = mapText(p, font, run);
    replaced = replaced && !same;
    if (replaced)
        beginActualText(p, run->text);
    if (stroke != nullptr) {
        // A stroke's width is in the space of the run, so the run's transform is the stroke's.
        save(p);
        matrix(content(p), transform);
        prTextBufferAppend(&content(p), "cm\n");
        float alpha = selectPaint(p, *paint, transform, true);
        state(p, 1, alpha, paint->blendMode);
        strokeState(content(p), stroke);
        showGlyphs(p, font, fontNumber, run, prAffine2Identity(), 1);
        restore(p);
    } else {
        float alpha = selectPaint(p, *paint, transform, false);
        state(p, alpha, 1, paint->blendMode);
        showGlyphs(p, font, fontNumber, run, transform, 0);
    }
    if (replaced)
        prTextBufferAppend(&content(p), "EMC\n");
    endPainting(p);
}

// ---------------------------------------------------------------------------------------------------------------------
// Clips, groups and marks

void pdfPushClip(void* PR_NULLABLE context, const PrPath* path, PrPathFillRule rule, PrAffine2 transform) {
    PdfWriter& p = *static_cast<PdfWriter*>(context);
    save(p);
    PrTextBuffer& out = content(p);
    pathOps(out, path, transform);
    prTextBufferAppend(&out, rule == PrPathFillRule_EvenOdd ? "W* n\n" : "W n\n");
}

void pdfPopClip(void* PR_NULLABLE context) {
    PdfWriter& p = *static_cast<PdfWriter*>(context);
    restore(p);
}

PdfStream& pushStream(PdfWriter& p) {
    if (p.streamDepth == p.streams.count) {
        PdfStream& made = p.streams.emplace();
        made = PdfStream{};
    }
    PdfStream& s = p.streams.data[p.streamDepth++];
    prTextBufferClear(&s.content);
    s.nextMcid = 0;
    s.form = false;
    s.saved.clear();
    s.text = false;
    s.artifact = false;
    // A form starts in the state of the stream that paints it, and a transparency group starts with the initial alphas
    // and blend mode (ISO 32000-2, 11.6.6). A page starts in the initial state.
    if (p.streamDepth > 1) {
        s.state = p.streams.data[p.streamDepth - 2].state;
    } else {
        s.state = PdfGraphicsState{.font = kHashMissing, .renderMode = 0};
    }
    s.state.extGState = kHashMissing;
    return s;
}

void pdfPushGroup(void* PR_NULLABLE context, const DrawSharedGroup* group) {
    PdfWriter& p = *static_cast<PdfWriter*>(context);
    PdfStream& s = pushStream(p);
    s.form = true;
    s.object = allocate(p);
    s.group = *group;
    s.bounds = group->bounds;
    s.structParents = tagged(p) ? p.nextStructParents++ : 0;
}

void pdfPopGroup(void* PR_NULLABLE context) {
    PdfWriter& p = *static_cast<PdfWriter*>(context);
    endArtifact(p);
    content(p);
    PdfStream& s = current(p);
    PrTextBuffer dictionary{};
    prTextBufferAppend(&dictionary, " /Type /XObject /Subtype /Form /BBox ");
    PrRect box = prRectIsEmpty(s.bounds) ? PrRect{0, 0, 1, 1} : s.bounds;
    rect(dictionary, box);
    prTextBufferAppend(&dictionary, " /Group << /S /Transparency");
    if (s.group.isolated)
        prTextBufferAppend(&dictionary, " /I true");
    if (s.group.knockout)
        prTextBufferAppend(&dictionary, " /K true");
    prTextBufferAppend(&dictionary, " >> /Resources ");
    reference(dictionary, kResources);
    if (tagged(p)) {
        prTextBufferAppend(&dictionary, " /StructParents ");
        prTextBufferAppendU64(&dictionary, s.structParents, 10);
    }
    writeStream(p, s.object, PrStringView{dictionary.data, dictionary.size},
        reinterpret_cast<const uint8_t*>(s.content.data), s.content.size);
    prTextBufferReset(&dictionary);
    uint32_t form = p.forms.count;
    p.forms.push(s.object);
    DrawSharedGroup group = s.group;
    --p.streamDepth;
    save(p);
    state(p, group.opacity, group.opacity, group.blendMode);
    prTextBufferAppend(&content(p), "/X");
    prTextBufferAppendU64(&content(p), form, 10);
    prTextBufferAppend(&content(p), " Do\n");
    restore(p);
}

// Returns the structure's node for `key`, or kPrDrawStructureNodeNone.
uint32_t nodeOfKey(const PdfWriter& p, uint64_t key) {
    uint32_t found = p.keyIndex.find(key, [&](uint32_t at) { return p.keys.data[at]; });
    return found != kHashMissing ? p.keyNodes.data[found] : kPrDrawStructureNodeNone;
}

// Opens a marked-content sequence for the mark at `index` of the open marks, with a new id in the current stream.
void openSequence(PdfWriter& p, uint32_t index) {
    PdfOpenMark& open = p.openMarks.data[index];
    uint32_t node = nodeOfKey(p, open.key);
    PdfStream& s = current(p);
    uint32_t mark = p.marks.count;
    p.marks.push(PdfMark{
        .key = open.key,
        .page = p.pages.count - 1,
        .stream = s.form ? s.object : 0,
        .mcid = s.nextMcid++,
        .structParents = s.structParents,
        .bounds = PrRect{0, 0, 0, 0},
    });
    open.mark = mark;
    PrDrawStructureType type = prDrawStructureNodeDesc(p.structure, node).type;
    PrTextBuffer& out = content(p);
    name(out, prDrawStructureTypeName(type));
    prTextBufferAppend(&out, " << /MCID ");
    prTextBufferAppendU64(&out, p.marks.last().mcid, 10);
    prTextBufferAppend(&out, " >> BDC\n");
}

void pdfPushMark(void* PR_NULLABLE context, uint64_t key) {
    PdfWriter& p = *static_cast<PdfWriter*>(context);
    if (!tagged(p) || nodeOfKey(p, key) == kPrDrawStructureNodeNone) {
        p.openMarks.push(PdfOpenMark{key, kHashMissing});
        return;
    }
    endArtifact(p);
    // Marked-content sequences with ids do not nest, so a mark inside a tagged one closes it until its pop.
    for (uint32_t i = p.openMarks.count; i-- > 0;) {
        if (p.openMarks.data[i].mark != kHashMissing) {
            prTextBufferAppend(&content(p), "EMC\n");
            break;
        }
    }
    p.openMarks.push(PdfOpenMark{key, kHashMissing});
    openSequence(p, p.openMarks.count - 1);
}

void pdfPopMark(void* PR_NULLABLE context, uint64_t key, PrRect bounds) {
    PdfWriter& p = *static_cast<PdfWriter*>(context);
    (void)key;
    PdfOpenMark open = p.openMarks.last();
    p.openMarks.pop();
    if (open.mark == kHashMissing)
        return;
    p.marks.data[open.mark].bounds = bounds;
    endArtifact(p);
    prTextBufferAppend(&content(p), "EMC\n");
    // The tagged mark around it continues in a sequence of its own.
    for (uint32_t i = p.openMarks.count; i-- > 0;) {
        if (p.openMarks.data[i].mark != kHashMissing) {
            openSequence(p, i);
            break;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Fonts

void writeFont(PdfWriter& p, PdfFont& f) {
    const PrFont* font = f.font;
    PrFontMetrics metrics = prFontMetrics(font);
    float adjust = prFontSizeAdjust(font);
    // The font's file holds outlines and metrics before size-adjust, in thousandths of an em.
    auto units = [&](float ems) { return double(ems / adjust * 1000); };
    RawArray<uint16_t> glyphs;
    for (uint32_t g = 0; g < f.used.count; ++g) {
        if (f.used.data[g] != 0)
            glyphs.push(uint16_t(g));
    }
    uint32_t file = allocate(p);
    RawArray<uint8_t> subset;
    PrFontSubsetFormat format = prFontSubset(font, glyphs.data, glyphs.count, [](void* PR_NULLABLE context, PrByteView bytes) {
        static_cast<RawArray<uint8_t>*>(context)->append(bytes.data, uint32_t(bytes.size));
    }, &subset);
    PR_ASSERT(format == f.format);
    writeStream(p, file, format == PrFontSubsetFormat_Cff ? " /Subtype /CIDFontType0C" : "", subset.data, subset.count);
    subset.reset();
    // A subset's name starts with a tag of six capitals that is unique in the document (ISO 32000-2, 9.9.2), followed
    // by the font's PostScript name (9.6.3), or its family's name if it has none.
    PrRawString family = prFontPostScriptName(font);
    if (prRawStringIsNull(&family))
        family = prFontFamilyName(font);
    PrTextBuffer base{};
    uint32_t tag = uint32_t(&f - p.fonts.data);
    for (uint32_t i = 0; i < 6; ++i) {
        prTextBufferAppendChar(&base, char('A' + tag % 26));
        tag /= 26;
    }
    prTextBufferAppendChar(&base, '+');
    PrStringView familyView = prRawStringView(&family);
    for (size_t i = 0; i < familyView.size; ++i) {
        char c = familyView.data[i];
        if (c != ' ')
            prTextBufferAppendChar(&base, c);
    }
    if (familyView.size == 0)
        prTextBufferAppend(&base, "Font");
    prRawStringReset(&family);
    PrStringView baseName{base.data, base.size};
    uint32_t descriptor = allocate(p);
    PrRect box{0, 0, 0, 0};
    for (uint32_t i = 0; i < glyphs.count; ++i)
        box = prRectUnion(box, prFontGlyphBounds(font, glyphs.data[i]));
    prTextBufferAppend(&p.out, "<< /Type /FontDescriptor /FontName ");
    name(p.out, baseName);
    prTextBufferAppend(&p.out, " /Flags 4 /FontBBox [");
    drawSharedAppendNumber(p.out, units(box.left));
    prTextBufferAppendChar(&p.out, ' ');
    drawSharedAppendNumber(p.out, units(-box.bottom));
    prTextBufferAppendChar(&p.out, ' ');
    drawSharedAppendNumber(p.out, units(box.right));
    prTextBufferAppendChar(&p.out, ' ');
    drawSharedAppendNumber(p.out, units(-box.top));
    prTextBufferAppend(&p.out, "] /ItalicAngle 0 /Ascent ");
    drawSharedAppendNumber(p.out, units(metrics.ascent));
    prTextBufferAppend(&p.out, " /Descent ");
    drawSharedAppendNumber(p.out, -units(metrics.descent));
    prTextBufferAppend(&p.out, " /CapHeight ");
    drawSharedAppendNumber(p.out, units(metrics.capHeight));
    prTextBufferAppend(&p.out, " /StemV 80 ");
    prTextBufferAppend(&p.out, format == PrFontSubsetFormat_Cff ? "/FontFile3 " : "/FontFile2 ");
    reference(p.out, file);
    prTextBufferAppend(&p.out, " >>");
    writeObject(p, descriptor);
    uint32_t cidFont = allocate(p);
    prTextBufferAppend(&p.out, "<< /Type /Font /Subtype ");
    prTextBufferAppend(&p.out, format == PrFontSubsetFormat_Cff ? "/CIDFontType0" : "/CIDFontType2");
    prTextBufferAppend(&p.out, " /BaseFont ");
    name(p.out, baseName);
    prTextBufferAppend(&p.out, " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /FontDescriptor ");
    reference(p.out, descriptor);
    if (format == PrFontSubsetFormat_TrueType)
        prTextBufferAppend(&p.out, " /CIDToGIDMap /Identity");
    prTextBufferAppend(&p.out, " /W [");
    for (uint32_t i = 0; i < glyphs.count; ++i) {
        prTextBufferAppendU64(&p.out, glyphs.data[i], 10);
        prTextBufferAppend(&p.out, " [");
        drawSharedAppendNumber(p.out, f.widths.data[glyphs.data[i]], 2);
        prTextBufferAppend(&p.out, "] ");
    }
    prTextBufferAppend(&p.out, "] >>");
    writeObject(p, cidFont);
    // The map back to characters (ISO 32000-2, 9.10.3).
    PrTextBuffer cmap{};
    prTextBufferAppend(&cmap, "/CIDInit /ProcSet findresource begin 12 dict begin begincmap\n"
        "/CIDSystemInfo << /Registry (Adobe) /Ordering (UCS) /Supplement 0 >> def\n"
        "/CMapName /Adobe-Identity-UCS def /CMapType 2 def\n1 begincodespacerange <0000> <FFFF> endcodespacerange\n");
    uint32_t pending = 0;
    PrTextBuffer entries{};
    auto flush = [&]() {
        if (pending == 0)
            return;
        prTextBufferAppendU64(&cmap, pending, 10);
        prTextBufferAppend(&cmap, " beginbfchar\n");
        prTextBufferAppend(&cmap, PrStringView{entries.data, entries.size});
        prTextBufferAppend(&cmap, "endbfchar\n");
        prTextBufferClear(&entries);
        pending = 0;
    };
    for (uint32_t i = 0; i < glyphs.count; ++i) {
        uint16_t g = glyphs.data[i];
        if (f.unicodeStart.data[g] == 0)
            continue;
        prTextBufferAppendChar(&entries, '<');
        drawSharedAppendHex(entries, g, 2);
        prTextBufferAppend(&entries, "> <");
        PrStringView text{f.unicode.data + f.unicodeStart.data[g], f.unicodeSize.data[g]};
        for (size_t at = 0; at < text.size;) {
            uint32_t c = 0;
            uint32_t size = prUtf8DecodeScalar(text, at, &c);
            at += size != 0 ? size : 1;
            if (c >= 0x10000) {
                c -= 0x10000;
                drawSharedAppendHex(entries, 0xD800 + (c >> 10), 2);
                drawSharedAppendHex(entries, 0xDC00 + (c & 0x3ff), 2);
            } else {
                drawSharedAppendHex(entries, c, 2);
            }
        }
        prTextBufferAppend(&entries, ">\n");
        if (++pending == 100)
            flush();
    }
    flush();
    prTextBufferAppend(&cmap, "endcmap CMapName currentdict /CMap defineresource pop end end\n");
    uint32_t toUnicode = allocate(p);
    writeStream(p, toUnicode, "", reinterpret_cast<const uint8_t*>(cmap.data), cmap.size);
    prTextBufferReset(&cmap);
    prTextBufferReset(&entries);
    prTextBufferAppend(&p.out, "<< /Type /Font /Subtype /Type0 /BaseFont ");
    name(p.out, baseName);
    prTextBufferAppend(&p.out, " /Encoding /Identity-H /DescendantFonts [");
    reference(p.out, cidFont);
    prTextBufferAppend(&p.out, "] /ToUnicode ");
    reference(p.out, toUnicode);
    prTextBufferAppend(&p.out, " >>");
    writeObject(p, f.object);
    prTextBufferReset(&base);
    glyphs.reset();
}

// Fills in the map back to characters of the glyphs that no run gave text, from the font's character map.
void mapFromCharacters(PdfFont& f) {
    bool missing = false;
    for (uint32_t g = 0; g < f.used.count && !missing; ++g)
        missing = f.used.data[g] != 0 && f.unicodeStart.data[g] == 0;
    if (!missing)
        return;
    // The planes that hold nearly every character that text draws: the Basic and Supplementary Multilingual Planes.
    for (uint32_t c = 0x20; c < 0x20000; ++c) {
        uint16_t g = prFontCodePointGlyph(f.font, c);
        if (g == 0 || g >= f.used.count || f.used.data[g] == 0 || f.unicodeStart.data[g] != 0)
            continue;
        char encoded[kPrUtf8MaxBytes];
        uint32_t size = prUtf8Encode(c, encoded);
        f.unicodeStart.data[g] = f.unicode.size;
        f.unicodeSize.data[g] = size;
        prTextBufferAppend(&f.unicode, PrStringView{encoded, size});
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Structure

// The objects that the document's structure becomes.
struct PdfStructure {
    RawArray<uint32_t> elements;          // by node: its structure element
    RawArray<uint32_t> firstMark;         // by node: the first mark of its content, by mark, or kHashMissing
    RawArray<uint32_t> parents;           // by mark: the element that holds it
    RawArray<uint32_t> linkAnnotations;   // the annotations of the links, in order

    // By key in the parent tree: the element of the link whose annotation has the key, or zero for a stream's key. Keys
    // past its end are streams' keys.
    RawArray<uint32_t> annotationParents;

    // The marks of each key, chained in the order that they were drawn, so that a walk over a key's marks costs the
    // number of its marks.
    RawArray<uint32_t> keyFirstMark;      // by key: its first mark, or kHashMissing
    RawArray<uint32_t> nextMark;          // by mark: the next mark of its key, or kHashMissing
};

// Chains the marks of each key, in the order that they were drawn. Every mark's key is one of the structure's keys,
// since only those open marked-content sequences.
void chainMarks(const PdfWriter& p, PdfStructure& s) {
    s.keyFirstMark.resize(p.keys.count);
    for (uint32_t k = 0; k < p.keys.count; ++k)
        s.keyFirstMark.data[k] = kHashMissing;
    s.nextMark.resize(p.marks.count);
    for (uint32_t i = p.marks.count; i-- > 0;) {
        uint32_t k = p.keyIndex.find(p.marks.data[i].key, [&](uint32_t at) { return p.keys.data[at]; });
        s.nextMark.data[i] = s.keyFirstMark.data[k];
        s.keyFirstMark.data[k] = i;
    }
}

// Returns the first mark of `key`, or kHashMissing.
uint32_t firstMarkOf(const PdfWriter& p, const PdfStructure& s, uint64_t key) {
    uint32_t k = p.keyIndex.find(key, [&](uint32_t at) { return p.keys.data[at]; });
    return k != kHashMissing ? s.keyFirstMark.data[k] : kHashMissing;
}

// Calls `fn` with each mark of `key`, in the order that they were drawn.
template <typename Fn>
void eachMark(const PdfWriter& p, const PdfStructure& s, uint64_t key, Fn fn) {
    for (uint32_t i = firstMarkOf(p, s, key); i != kHashMissing; i = s.nextMark.data[i])
        fn(i);
}

// Calls `fn` with each mark of the content of `node` and its descendants, in reading order.
template <typename Fn>
void eachContentMark(const PdfWriter& p, const PdfStructure& s, PrDrawStructureNode node, Fn fn) {
    uint32_t count = prDrawStructureItemCount(p.structure, node);
    const PrDrawStructureItem* items = prDrawStructureItems(p.structure, node);
    for (uint32_t i = 0; i < count; ++i) {
        if (items[i].node != kPrDrawStructureNodeNone)
            eachContentMark(p, s, items[i].node, fn);
        else
            eachMark(p, s, items[i].key, fn);
    }
}

// Returns the first mark of the content of `node` and its descendants, in reading order, or kHashMissing. Finds the
// first marks of the descendants on the way, so that finding every node's first mark visits each node once.
uint32_t findFirstMark(const PdfWriter& p, PdfStructure& s, PrDrawStructureNode node) {
    uint32_t count = prDrawStructureItemCount(p.structure, node);
    const PrDrawStructureItem* items = prDrawStructureItems(p.structure, node);
    uint32_t first = kHashMissing;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t mark;
        if (items[i].node != kPrDrawStructureNodeNone)
            mark = findFirstMark(p, s, items[i].node);
        else
            mark = firstMarkOf(p, s, items[i].key);
        if (first == kHashMissing)
            first = mark;
    }
    s.firstMark.data[node] = first;
    return first;
}

// Appends a destination at the top left of the mark (ISO 32000-2, 12.3.2.2).
void destination(PdfWriter& p, PrTextBuffer& out, uint32_t mark) {
    const PdfMark& m = p.marks.data[mark];
    const PdfPage& page = p.pages.data[m.page];
    PrRect r = pageRect(page, m.bounds);
    prTextBufferAppendChar(&out, '[');
    reference(out, page.object);
    prTextBufferAppend(&out, " /XYZ ");
    drawSharedAppendNumber(out, double(r.left));
    prTextBufferAppendChar(&out, ' ');
    drawSharedAppendNumber(out, double(r.bottom));
    prTextBufferAppend(&out, " null]");
}

// Writes the link annotations of a Link node, one for each page that its content falls on, and appends them to its
// element's kids as object references.
void writeLinkAnnotations(PdfWriter& p, PdfStructure& s, PrDrawStructureNode node, PrTextBuffer& kids) {
    PrDrawStructureNodeDesc desc = prDrawStructureNodeDesc(p.structure, node);
    uint32_t target = desc.destination != 0 ? firstMarkOf(p, s, desc.destination) : kHashMissing;
    if (desc.uri.size == 0 && target == kHashMissing)
        return;
    // The link's marks with bounds, in reading order. Each annotation takes the marks of one page, the page of the
    // first mark left, so the annotations follow the link's first content on each page.
    RawArray<uint32_t> marks;
    eachContentMark(p, s, node, [&](uint32_t mark) {
        if (!prRectIsEmpty(p.marks.data[mark].bounds))
            marks.push(mark);
    });
    RawArray<PrRect> rects;
    while (marks.count != 0) {
        uint32_t page = p.marks.data[marks.data[0]].page;
        rects.clear();
        uint32_t kept = 0;
        for (uint32_t i = 0; i < marks.count; ++i) {
            const PdfMark& m = p.marks.data[marks.data[i]];
            if (m.page == page)
                rects.push(pageRect(p.pages.data[page], m.bounds));
            else
                marks.data[kept++] = marks.data[i];
        }
        marks.count = kept;
        PrRect all = rects.data[0];
        for (uint32_t i = 1; i < rects.count; ++i)
            all = prRectUnion(all, rects.data[i]);
        uint32_t annotation = allocate(p);
        uint32_t key = p.nextStructParents++;
        prTextBufferAppend(&p.out, "<< /Type /Annot /Subtype /Link /Rect ");
        rect(p.out, all);
        prTextBufferAppend(&p.out, " /QuadPoints [");
        for (uint32_t i = 0; i < rects.count; ++i) {
            const PrRect& r = rects.data[i];
            float quad[8] = {r.left, r.bottom, r.right, r.bottom, r.right, r.top, r.left, r.top};
            for (float v : quad) {
                drawSharedAppendNumber(p.out, double(v));
                prTextBufferAppendChar(&p.out, ' ');
            }
        }
        prTextBufferAppend(&p.out, "] /Border [0 0 0] /F 4 /P ");
        reference(p.out, p.pages.data[page].object);
        prTextBufferAppend(&p.out, " /StructParent ");
        prTextBufferAppendU64(&p.out, key, 10);
        prTextBufferAppend(&p.out, " /Contents ");
        textString(p.out, desc.alt.size != 0 ? desc.alt : desc.uri.size != 0 ? desc.uri : desc.title);
        if (desc.uri.size != 0) {
            prTextBufferAppend(&p.out, " /A << /Type /Action /S /URI /URI ");
            literal(p.out, desc.uri);
            prTextBufferAppend(&p.out, " >>");
        } else {
            prTextBufferAppend(&p.out, " /Dest ");
            destination(p, p.out, target);
        }
        prTextBufferAppend(&p.out, " >>");
        writeObject(p, annotation);
        p.pages.data[page].annotations.push(annotation);
        s.linkAnnotations.push(annotation);
        while (s.annotationParents.count <= key)
            s.annotationParents.push(0);
        s.annotationParents.data[key] = s.elements.data[node];
        prTextBufferAppend(&kids, "<< /Type /OBJR /Obj ");
        reference(kids, annotation);
        prTextBufferAppend(&kids, " /Pg ");
        reference(kids, p.pages.data[page].object);
        prTextBufferAppend(&kids, " >> ");
    }
    rects.reset();
    marks.reset();
}

void writeElement(PdfWriter& p, PdfStructure& s, PrDrawStructureNode node, uint32_t parent) {
    PrDrawStructureNodeDesc desc = prDrawStructureNodeDesc(p.structure, node);
    uint32_t count = prDrawStructureItemCount(p.structure, node);
    const PrDrawStructureItem* items = prDrawStructureItems(p.structure, node);
    PrTextBuffer kids{};
    // The page of the element's first content, which /Pg names.
    uint32_t page = s.firstMark.data[node] != kHashMissing ? p.marks.data[s.firstMark.data[node]].page : kHashMissing;
    for (uint32_t i = 0; i < count; ++i) {
        if (items[i].node != kPrDrawStructureNodeNone) {
            writeElement(p, s, items[i].node, s.elements.data[node]);
            reference(kids, s.elements.data[items[i].node]);
            prTextBufferAppendChar(&kids, ' ');
            continue;
        }
        eachMark(p, s, items[i].key, [&](uint32_t mark) {
            const PdfMark& m = p.marks.data[mark];
            s.parents.data[mark] = s.elements.data[node];
            if (m.stream == 0 && m.page == page) {
                prTextBufferAppendU64(&kids, m.mcid, 10);
                prTextBufferAppendChar(&kids, ' ');
                return;
            }
            prTextBufferAppend(&kids, "<< /Type /MCR /Pg ");
            reference(kids, p.pages.data[m.page].object);
            if (m.stream != 0) {
                prTextBufferAppend(&kids, " /Stm ");
                reference(kids, m.stream);
            }
            prTextBufferAppend(&kids, " /MCID ");
            prTextBufferAppendU64(&kids, m.mcid, 10);
            prTextBufferAppend(&kids, " >> ");
        });
    }
    if (desc.type == PrDrawStructureType_Link)
        writeLinkAnnotations(p, s, node, kids);
    PrTextBuffer& out = p.out;
    prTextBufferAppend(&out, "<< /Type /StructElem /S ");
    name(out, prDrawStructureTypeName(desc.type));
    if (!prDrawStructureTypeIsLegacy(desc.type)) {
        prTextBufferAppend(&out, " /NS ");
        reference(out, p.namespaceObject);
    }
    prTextBufferAppend(&out, " /P ");
    reference(out, parent);
    if (page != kHashMissing) {
        prTextBufferAppend(&out, " /Pg ");
        reference(out, p.pages.data[page].object);
    }
    if (desc.alt.size != 0) {
        prTextBufferAppend(&out, " /Alt ");
        textString(out, desc.alt);
    }
    if (desc.actualText.size != 0) {
        prTextBufferAppend(&out, " /ActualText ");
        textString(out, desc.actualText);
    }
    if (desc.expansion.size != 0) {
        prTextBufferAppend(&out, " /E ");
        textString(out, desc.expansion);
    }
    if (desc.language.size != 0) {
        prTextBufferAppend(&out, " /Lang ");
        textString(out, desc.language);
    }
    if (desc.title.size != 0) {
        prTextBufferAppend(&out, " /T ");
        textString(out, desc.title);
    }
    bool cell = desc.type == PrDrawStructureType_TH || desc.type == PrDrawStructureType_TD;
    if (cell && (desc.scope != PrDrawStructureScope_None || desc.rowSpan > 1 || desc.columnSpan > 1)) {
        prTextBufferAppend(&out, " /A << /O /Table");
        constexpr PrStringView kScopes[] = {"", "Row", "Column", "Both"};
        if (desc.type == PrDrawStructureType_TH && desc.scope != PrDrawStructureScope_None) {
            prTextBufferAppend(&out, " /Scope /");
            prTextBufferAppend(&out, kScopes[desc.scope]);
        }
        if (desc.rowSpan > 1) {
            prTextBufferAppend(&out, " /RowSpan ");
            prTextBufferAppendU64(&out, desc.rowSpan, 10);
        }
        if (desc.columnSpan > 1) {
            prTextBufferAppend(&out, " /ColSpan ");
            prTextBufferAppendU64(&out, desc.columnSpan, 10);
        }
        prTextBufferAppend(&out, " >>");
    }
    prTextBufferAppend(&out, " /K [");
    prTextBufferAppend(&out, PrStringView{kids.data, kids.size});
    prTextBufferAppend(&out, "] >>");
    writeObject(p, s.elements.data[node]);
    prTextBufferReset(&kids);
}

// Writes the outline of the headings, nested by level (ISO 32000-2, 12.3.3). Returns the outline's object, or zero.
uint32_t writeOutline(PdfWriter& p, const PdfStructure& s) {
    // The headings in reading order, with their levels.
    RawArray<uint32_t> headings;
    RawArray<uint32_t> levels;
    for (uint32_t node = 0; node < prDrawStructureNodeCount(p.structure); ++node) {
        uint32_t level = prDrawStructureTypeHeadingLevel(prDrawStructureNodeDesc(p.structure, node).type);
        if (level != 0 && s.firstMark.data[node] != kHashMissing) {
            headings.push(node);
            levels.push(level);
        }
    }
    // Nodes are numbered as their producer appended them, which for a tree built in reading order is reading order.
    if (headings.count == 0) {
        headings.reset();
        levels.reset();
        return 0;
    }
    uint32_t root = allocate(p);
    RawArray<uint32_t> objects;
    for (uint32_t i = 0; i < headings.count; ++i)
        objects.push(allocate(p));
    // Each item's parent is the nearest earlier heading of a lower level, which is on the stack of open items when the
    // item is reached. Siblings and children are chained as they are found, and the root's are at index `rootItem`.
    uint32_t rootItem = headings.count;
    RawArray<uint32_t> parents;             // by item: its parent item, or kHashMissing for the root
    RawArray<uint32_t> previous;            // by item: its previous sibling, or kHashMissing
    RawArray<uint32_t> next;                // by item: its next sibling, or kHashMissing
    RawArray<uint32_t> firstChild;          // by item: its first child, or kHashMissing
    RawArray<uint32_t> lastChild;           // by item: its last child, or kHashMissing
    RawArray<uint32_t> childCount;          // by item
    RawArray<uint32_t> open;                // the items that a later item may be a child of, deepest last
    for (uint32_t i = 0; i <= headings.count; ++i) {
        firstChild.push(kHashMissing);
        lastChild.push(kHashMissing);
        childCount.push(0);
    }
    for (uint32_t i = 0; i < headings.count; ++i) {
        while (open.count != 0 && levels.data[open.last()] >= levels.data[i])
            open.pop();
        uint32_t parent = open.count != 0 ? open.last() : kHashMissing;
        uint32_t holder = parent != kHashMissing ? parent : rootItem;
        parents.push(parent);
        previous.push(lastChild.data[holder]);
        next.push(kHashMissing);
        if (lastChild.data[holder] != kHashMissing)
            next.data[lastChild.data[holder]] = i;
        else
            firstChild.data[holder] = i;
        lastChild.data[holder] = i;
        childCount.data[holder] += 1;
        open.push(i);
    }
    for (uint32_t i = 0; i < headings.count; ++i) {
        PrDrawStructureNodeDesc desc = prDrawStructureNodeDesc(p.structure, headings.data[i]);
        prTextBufferAppend(&p.out, "<< /Title ");
        textString(p.out, desc.title);
        prTextBufferAppend(&p.out, " /Parent ");
        reference(p.out, parents.data[i] == kHashMissing ? root : objects.data[parents.data[i]]);
        if (previous.data[i] != kHashMissing) {
            prTextBufferAppend(&p.out, " /Prev ");
            reference(p.out, objects.data[previous.data[i]]);
        }
        if (next.data[i] != kHashMissing) {
            prTextBufferAppend(&p.out, " /Next ");
            reference(p.out, objects.data[next.data[i]]);
        }
        if (childCount.data[i] != 0) {
            prTextBufferAppend(&p.out, " /First ");
            reference(p.out, objects.data[firstChild.data[i]]);
            prTextBufferAppend(&p.out, " /Last ");
            reference(p.out, objects.data[lastChild.data[i]]);
            prTextBufferAppend(&p.out, " /Count ");
            prTextBufferAppendU64(&p.out, childCount.data[i], 10);
        }
        prTextBufferAppend(&p.out, " /Dest ");
        destination(p, p.out, s.firstMark.data[headings.data[i]]);
        prTextBufferAppend(&p.out, " >>");
        writeObject(p, objects.data[i]);
    }
    prTextBufferAppend(&p.out, "<< /Type /Outlines /First ");
    reference(p.out, objects.data[firstChild.data[rootItem]]);
    prTextBufferAppend(&p.out, " /Last ");
    reference(p.out, objects.data[lastChild.data[rootItem]]);
    prTextBufferAppend(&p.out, " /Count ");
    prTextBufferAppendU64(&p.out, childCount.data[rootItem], 10);
    prTextBufferAppend(&p.out, " >>");
    writeObject(p, root);
    headings.reset();
    levels.reset();
    objects.reset();
    parents.reset();
    previous.reset();
    next.reset();
    firstChild.reset();
    lastChild.reset();
    childCount.reset();
    open.reset();
    return root;
}

// Writes the structure tree, its parent tree and the outline. Returns the structure tree root's object, and the
// outline's through `outline`.
uint32_t writeStructure(PdfWriter& p, uint32_t* outline) {
    PdfStructure s{};
    uint32_t nodes = prDrawStructureNodeCount(p.structure);
    uint32_t root = allocate(p);
    for (uint32_t i = 0; i < nodes; ++i)
        s.elements.push(allocate(p));
    s.parents.resize(p.marks.count);
    for (uint32_t i = 0; i < p.marks.count; ++i)
        s.parents.data[i] = 0;
    chainMarks(p, s);
    s.firstMark.resize(nodes);
    for (uint32_t i = 0; i < nodes; ++i)
        s.firstMark.data[i] = kHashMissing;
    findFirstMark(p, s, kPrDrawStructureRoot);
    // The namespace of PDF 2.0's types, which every element that is not of PDF 1.7's alone names.
    p.namespaceObject = allocate(p);
    // The types of PDF 2.0 that PDF 1.7 lacks map to the nearest of PDF 1.7's (ISO 32000-2, 14.8.6), for a reader that
    // knows only those.
    uint32_t legacy = allocate(p);
    p.legacyNamespace = legacy;
    prTextBufferAppend(&p.out, "<< /Type /Namespace /NS ");
    literal(p.out, kNamespace1);
    prTextBufferAppend(&p.out, " >>");
    writeObject(p, legacy);
    prTextBufferAppend(&p.out, "<< /Type /Namespace /NS ");
    literal(p.out, kNamespace2);
    prTextBufferAppend(&p.out, " /RoleMapNS <<");
    struct Mapped {
        PrStringView from;
        PrStringView to;
    };
    static const Mapped kMapped[] = {
        {"DocumentFragment", "Div"}, {"Aside", "Div"}, {"Title", "P"}, {"FENote", "Note"}, {"Sub", "Span"},
        {"Em", "Span"}, {"Strong", "Span"},
    };
    for (const Mapped& m : kMapped) {
        prTextBufferAppendChar(&p.out, ' ');
        name(p.out, m.from);
        prTextBufferAppend(&p.out, " [");
        name(p.out, m.to);
        prTextBufferAppendChar(&p.out, ' ');
        reference(p.out, legacy);
        prTextBufferAppendChar(&p.out, ']');
    }
    prTextBufferAppend(&p.out, " >> >>");
    writeObject(p, p.namespaceObject);
    writeElement(p, s, kPrDrawStructureRoot, root);
    // The parent tree: for each stream, its elements by marked-content id, and for each annotation, its element.
    prTextBufferAppend(&p.out, "<< /Type /StructTreeRoot /K ");
    reference(p.out, s.elements.data[kPrDrawStructureRoot]);
    prTextBufferAppend(&p.out, " /Namespaces [");
    reference(p.out, p.namespaceObject);
    prTextBufferAppendChar(&p.out, ' ');
    reference(p.out, p.legacyNamespace);
    prTextBufferAppend(&p.out, "] /ParentTree << /Nums [");
    // The marks by their key in the parent tree, in the order that they were drawn.
    RawArray<uint32_t> keyStart;
    RawArray<uint32_t> byKey;
    keyStart.resize(p.nextStructParents + 1);
    for (uint32_t key = 0; key <= p.nextStructParents; ++key)
        keyStart.data[key] = 0;
    for (uint32_t m = 0; m < p.marks.count; ++m)
        keyStart.data[p.marks.data[m].structParents + 1] += 1;
    for (uint32_t key = 0; key < p.nextStructParents; ++key)
        keyStart.data[key + 1] += keyStart.data[key];
    byKey.resize(p.marks.count);
    for (uint32_t m = 0; m < p.marks.count; ++m)
        byKey.data[keyStart.data[p.marks.data[m].structParents]++] = m;
    for (uint32_t key = 0; key < p.nextStructParents; ++key) {
        uint32_t link = key < s.annotationParents.count ? s.annotationParents.data[key] : 0;
        if (link != 0) {
            prTextBufferAppendU64(&p.out, key, 10);
            prTextBufferAppendChar(&p.out, ' ');
            reference(p.out, link);
            prTextBufferAppendChar(&p.out, ' ');
            continue;
        }
        prTextBufferAppendU64(&p.out, key, 10);
        prTextBufferAppend(&p.out, " [");
        uint32_t next = 0;
        // The counting above left each key's start at the start of the next key's marks.
        for (uint32_t at = key != 0 ? keyStart.data[key - 1] : 0; at < keyStart.data[key]; ++at) {
            uint32_t m = byKey.data[at];
            const PdfMark& mark = p.marks.data[m];
            while (next < mark.mcid) {
                prTextBufferAppend(&p.out, "null ");
                ++next;
            }
            if (s.parents.data[m] != 0)
                reference(p.out, s.parents.data[m]);
            else
                prTextBufferAppend(&p.out, "null");
            prTextBufferAppendChar(&p.out, ' ');
            ++next;
        }
        prTextBufferAppend(&p.out, "] ");
    }
    prTextBufferAppend(&p.out, "] >> /ParentTreeNextKey ");
    prTextBufferAppendU64(&p.out, p.nextStructParents, 10);
    prTextBufferAppend(&p.out, " >>");
    writeObject(p, root);
    *outline = writeOutline(p, s);
    s.elements.reset();
    s.firstMark.reset();
    s.parents.reset();
    s.linkAnnotations.reset();
    s.annotationParents.reset();
    s.keyFirstMark.reset();
    s.nextMark.reset();
    keyStart.reset();
    byKey.reset();
    return root;
}

// Collects the keys that the structure names, and the node of each.
void indexKeys(PdfWriter& p) {
    p.keys.clear();
    p.keyNodes.clear();
    p.keyIndex.clear();
    if (p.structure == nullptr)
        return;
    for (uint32_t node = 0; node < prDrawStructureNodeCount(p.structure); ++node) {
        uint32_t count = prDrawStructureItemCount(p.structure, node);
        const PrDrawStructureItem* items = prDrawStructureItems(p.structure, node);
        for (uint32_t i = 0; i < count; ++i) {
            uint64_t key = items[i].key;
            if (items[i].node != kPrDrawStructureNodeNone ||
                    p.keyIndex.find(key, [&](uint32_t at) { return p.keys.data[at]; }) != kHashMissing)
                continue;
            p.keyIndex.insert(key, p.keys.count);
            p.keys.push(key);
            p.keyNodes.push(node);
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The document

// Appends `text` as the content of an XML element, escaping the characters that markup would read.
void xmlText(PrTextBuffer& out, PrStringView text) {
    for (size_t i = 0; i < text.size; ++i) {
        char c = text.data[i];
        if (c == '&')
            prTextBufferAppend(&out, "&amp;");
        else if (c == '<')
            prTextBufferAppend(&out, "&lt;");
        else if (c == '>')
            prTextBufferAppend(&out, "&gt;");
        else
            prTextBufferAppendChar(&out, c);
    }
}

// Writes the document's metadata as an XMP packet (ISO 32000-2, 14.3.2), and returns its object. The packet gives the
// title, the format, and for a tagged document its claim of PDF/UA-2 (ISO 14289-2, 5). It is written as it is, so that
// a reader that scans the file for XMP finds it.
uint32_t writeMetadata(PdfWriter& p) {
    PrTextBuffer xmp{};
    prTextBufferAppend(&xmp, "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
        "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
        "<rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
        "<rdf:Description rdf:about=\"\" xmlns:dc=\"http://purl.org/dc/elements/1.1/\" "
        "xmlns:pdf=\"http://ns.adobe.com/pdf/1.3/\" xmlns:pdfuaid=\"http://www.aiim.org/pdfua/ns/id/\">\n"
        "<dc:format>application/pdf</dc:format>\n");
    if (p.document.title.size != 0) {
        prTextBufferAppend(&xmp, "<dc:title><rdf:Alt><rdf:li xml:lang=\"x-default\">");
        xmlText(xmp, p.document.title);
        prTextBufferAppend(&xmp, "</rdf:li></rdf:Alt></dc:title>\n");
    }
    if (p.document.language.size != 0) {
        prTextBufferAppend(&xmp, "<dc:language><rdf:Bag><rdf:li>");
        xmlText(xmp, p.document.language);
        prTextBufferAppend(&xmp, "</rdf:li></rdf:Bag></dc:language>\n");
    }
    prTextBufferAppend(&xmp, "<pdf:Producer>Prax</pdf:Producer>\n");
    if (tagged(p))
        prTextBufferAppend(&xmp, "<pdfuaid:part>2</pdfuaid:part>\n<pdfuaid:rev>2024</pdfuaid:rev>\n");
    prTextBufferAppend(&xmp, "</rdf:Description>\n</rdf:RDF>\n</x:xmpmeta>\n<?xpacket end=\"w\"?>");
    uint32_t object = allocate(p);
    emitStream(p, object, " /Type /Metadata /Subtype /XML", reinterpret_cast<const uint8_t*>(xmp.data), xmp.size, false);
    prTextBufferReset(&xmp);
    return object;
}

// Writes the cross-reference table, in which an object never written is free (ISO 32000-2, 7.5.4), the trailer that
// names the catalog, the information dictionary `info` if it is not zero, and the file's identifier, and the end of the
// file.
void writeXrefTable(PdfWriter& p, uint32_t info, PrStringView id) {
    uint64_t xref = p.written;
    prTextBufferClear(&p.out);
    prTextBufferAppend(&p.out, "xref\n0 ");
    prTextBufferAppendU64(&p.out, p.offsets.count, 10);
    prTextBufferAppend(&p.out, "\n0000000000 65535 f\r\n");
    for (uint32_t i = 1; i < p.offsets.count; ++i) {
        // Each entry is 20 bytes, with its offset in 10 digits.
        char digits[kPrU64TextMaxBytes];
        size_t count = prU64Format(p.offsets.data[i], digits, 10);
        for (size_t k = count; k < 10; ++k)
            prTextBufferAppendChar(&p.out, '0');
        prTextBufferAppend(&p.out, PrStringView{digits, count});
        prTextBufferAppend(&p.out, p.offsets.data[i] != 0 ? " 00000 n\r\n" : " 00000 f\r\n");
    }
    prTextBufferAppend(&p.out, "trailer\n<< /Size ");
    prTextBufferAppendU64(&p.out, p.offsets.count, 10);
    prTextBufferAppend(&p.out, " /Root ");
    reference(p.out, kCatalog);
    if (info != 0) {
        prTextBufferAppend(&p.out, " /Info ");
        reference(p.out, info);
    }
    prTextBufferAppend(&p.out, id);
    prTextBufferAppend(&p.out, " >>\nstartxref\n");
    prTextBufferAppendU64(&p.out, xref, 10);
    prTextBufferAppend(&p.out, "\n%%EOF\n");
    emit(p, p.out.data, p.out.size);
    prTextBufferClear(&p.out);
}

// Writes the cross-reference stream (ISO 32000-2, 7.5.8), which also finds the objects that object streams hold, with
// the trailer's entries, and the end of the file. Each entry is its type, then a field as wide as the largest offset or
// object number needs, then two bytes.
void writeXrefStream(PdfWriter& p, uint32_t info, PrStringView id) {
    flushPending(p);
    uint32_t xref = allocate(p);
    uint64_t at = p.written;
    p.offsets.data[xref] = at;
    uint64_t largest = at > p.offsets.count ? at : p.offsets.count;
    uint32_t width = 1;
    while (width < 8 && (largest >> (8 * width)) != 0)
        ++width;
    RawArray<uint8_t> entries;
    entries.resize(p.offsets.count * (width + 3));
    uint8_t* entry = entries.data;
    for (uint32_t i = 0; i < p.offsets.count; ++i) {
        uint8_t type = 0;
        uint64_t field = 0;
        uint32_t last = 0;
        if (i == 0) {
            last = 0xffff;
        } else if (p.containers.data[i] != 0) {
            type = 2;
            field = p.containers.data[i];
            last = uint32_t(p.offsets.data[i]);
        } else if (p.offsets.data[i] != 0) {
            type = 1;
            field = p.offsets.data[i];
        }
        *entry++ = type;
        for (uint32_t k = width; k-- > 0;)
            *entry++ = uint8_t(field >> (8 * k));
        *entry++ = uint8_t(last >> 8);
        *entry++ = uint8_t(last);
    }
    PrTextBuffer dictionary{};
    prTextBufferAppend(&dictionary, " /Type /XRef /Size ");
    prTextBufferAppendU64(&dictionary, p.offsets.count, 10);
    prTextBufferAppend(&dictionary, " /W [1 ");
    prTextBufferAppendU64(&dictionary, width, 10);
    prTextBufferAppend(&dictionary, " 2] /Root ");
    reference(dictionary, kCatalog);
    if (info != 0) {
        prTextBufferAppend(&dictionary, " /Info ");
        reference(dictionary, info);
    }
    prTextBufferAppend(&dictionary, id);
    writeStreamNow(p, xref, PrStringView{dictionary.data, dictionary.size}, entries.data, entries.count);
    prTextBufferReset(&dictionary);
    entries.reset();
    prTextBufferClear(&p.out);
    prTextBufferAppend(&p.out, "startxref\n");
    prTextBufferAppendU64(&p.out, at, 10);
    prTextBufferAppend(&p.out, "\n%%EOF\n");
    emit(p, p.out.data, p.out.size);
    prTextBufferClear(&p.out);
}

void resetDocument(PdfWriter& p) {
    for (uint32_t i = 0; i < p.fonts.count; ++i) {
        PdfFont& f = p.fonts.data[i];
        prFontRelease(f.font);
        f.used.reset();
        f.widths.reset();
        f.unicodeStart.reset();
        f.unicodeSize.reset();
        prTextBufferReset(&f.unicode);
    }
    for (uint32_t i = 0; i < p.pages.count; ++i)
        p.pages.data[i].annotations.reset();
    p.fonts.clear();
    p.fontIndex.clear();
    p.images.clear();
    p.imageIndex.clear();
    p.states.clear();
    p.patterns.clear();
    p.forms.clear();
    p.pages.clear();
    p.marks.clear();
    p.openMarks.clear();
    p.offsets.clear();
    p.containers.clear();
    p.packedStream = 0;
    p.packedCount = 0;
    prTextBufferClear(&p.packedIndex);
    prTextBufferClear(&p.packed);
    p.pendingCount = 0;
    p.pendingBytes = 0;
    prTextBufferClear(&p.out);
}

void destroyWriter(PrDrawWriter* writer) {
    PdfWriter* p = static_cast<PdfWriter*>(writer);
    resetDocument(*p);
    drawSharedWalkerReset(p->walker);
    for (uint32_t i = 0; i < p->streams.count; ++i) {
        prTextBufferReset(&p->streams.data[i].content);
        p->streams.data[i].saved.reset();
    }
    p->streams.reset();
    p->offsets.reset();
    p->containers.reset();
    prTextBufferReset(&p->packedIndex);
    prTextBufferReset(&p->packed);
    for (uint32_t i = 0; i < p->pending.count; ++i) {
        PdfPending& queued = p->pending.data[i];
        prTextBufferReset(&queued.dictionary);
        queued.data.reset();
        queued.encoded.reset();
    }
    p->pending.reset();
    prTextBufferReset(&p->out);
    p->pages.reset();
    p->fonts.reset();
    p->fontIndex.reset();
    p->images.reset();
    p->imageIndex.reset();
    p->states.reset();
    p->patterns.reset();
    p->forms.reset();
    p->keys.reset();
    p->keyNodes.reset();
    p->keyIndex.reset();
    p->marks.reset();
    p->openMarks.reset();
    p->glyphStarts.reset();
    p->glyphSizes.reset();
    p->clusterOrder.reset();
    p->gaps.reset();
    prTextBufferReset(&p->scratch);
    p->bytes.reset();
    p->alpha.reset();
    p->stops.reset();
    prPathDestroy(p->path);
    prMemoryFree(p);
}

void begin(PrDrawWriter* writer, const PrDrawDocumentDesc* desc) {
    PdfWriter& p = *static_cast<PdfWriter*>(writer);
    resetDocument(p);
    p.document = *desc;
    p.structure = desc->structure;
    p.written = 0;
    p.hash = 0;
    p.nextStructParents = 0;
    // Object zero is the head of the free list, and the catalog, the page tree and the resources are numbered first.
    for (uint32_t i = 0; i <= kResources; ++i)
        allocate(p);
    indexKeys(p);
    const char header[] = "%PDF-2.0\n%\xE2\xE3\xCF\xD3\n";
    emit(p, header, sizeof(header) - 1);
}

void addPage(PrDrawWriter* writer, const PrDrawPage* drawn) {
    PdfWriter& p = *static_cast<PdfWriter*>(writer);
    PdfPage& page = p.pages.emplace();
    page = PdfPage{};
    page.object = allocate(p);
    page.content = allocate(p);
    PrVec2 extent = drawn->extent;
    page.extent = extent;
    page.bleed = drawn->bleed;
    page.trim = drawn->trim;
    page.structParents = tagged(p) ? p.nextStructParents++ : 0;
    p.streamDepth = 0;
    PdfStream& s = pushStream(p);
    s.object = page.object;
    s.structParents = page.structParents;
    // The page's content is in px with y down, from its top left.
    double height = double(extent.y) * kPointsPerPx;
    drawSharedAppendNumber(s.content, kPointsPerPx);
    prTextBufferAppend(&s.content, " 0 0 ");
    drawSharedAppendNumber(s.content, -kPointsPerPx);
    prTextBufferAppend(&s.content, " 0 ");
    drawSharedAppendNumber(s.content, height);
    prTextBufferAppend(&s.content, " cm\n");
    p.openMarks.clear();
    drawSharedWalk(p.walker, drawn->list, extent);
    endArtifact(p);
    content(p);
    PdfStream& done = p.streams.data[0];
    writeStream(p, page.content, "", reinterpret_cast<const uint8_t*>(done.content.data), done.content.size);
    p.streamDepth = 0;
}

void end(PrDrawWriter* writer) {
    PdfWriter& p = *static_cast<PdfWriter*>(writer);
    for (uint32_t i = 0; i < p.fonts.count; ++i) {
        mapFromCharacters(p.fonts.data[i]);
        if (p.fonts.data[i].format != PrFontSubsetFormat_None)
            writeFont(p, p.fonts.data[i]);
    }
    uint32_t structTree = 0;
    uint32_t outline = 0;
    if (tagged(p))
        structTree = writeStructure(p, &outline);
    // The pages, each with its annotations.
    for (uint32_t i = 0; i < p.pages.count; ++i) {
        const PdfPage& page = p.pages.data[i];
        prTextBufferAppend(&p.out, "<< /Type /Page /Parent ");
        reference(p.out, kPages);
        prTextBufferAppend(&p.out, " /MediaBox [0 0 ");
        drawSharedAppendNumber(p.out, double(page.extent.x) * kPointsPerPx);
        prTextBufferAppendChar(&p.out, ' ');
        drawSharedAppendNumber(p.out, double(page.extent.y) * kPointsPerPx);
        prTextBufferAppend(&p.out, "]");
        appendBox(p.out, "BleedBox", page, page.bleed);
        appendBox(p.out, "TrimBox", page, page.trim);
        prTextBufferAppend(&p.out, " /Resources ");
        reference(p.out, kResources);
        prTextBufferAppend(&p.out, " /Contents ");
        reference(p.out, page.content);
        if (tagged(p)) {
            prTextBufferAppend(&p.out, " /StructParents ");
            prTextBufferAppendU64(&p.out, page.structParents, 10);
            prTextBufferAppend(&p.out, " /Tabs /S");
        }
        if (page.annotations.count != 0) {
            prTextBufferAppend(&p.out, " /Annots [");
            for (uint32_t k = 0; k < page.annotations.count; ++k) {
                reference(p.out, page.annotations.data[k]);
                prTextBufferAppendChar(&p.out, ' ');
            }
            prTextBufferAppendChar(&p.out, ']');
        }
        prTextBufferAppend(&p.out, " >>");
        writeObject(p, page.object);
    }
    prTextBufferAppend(&p.out, "<< /Type /Pages /Kids [");
    for (uint32_t i = 0; i < p.pages.count; ++i) {
        reference(p.out, p.pages.data[i].object);
        prTextBufferAppendChar(&p.out, ' ');
    }
    prTextBufferAppend(&p.out, "] /Count ");
    prTextBufferAppendU64(&p.out, p.pages.count, 10);
    prTextBufferAppend(&p.out, " >>");
    writeObject(p, kPages);
    // The resources that every page and form shares.
    prTextBufferAppend(&p.out, "<< /Font <<");
    for (uint32_t i = 0; i < p.fonts.count; ++i) {
        if (p.fonts.data[i].format == PrFontSubsetFormat_None)
            continue;
        prTextBufferAppend(&p.out, " /F");
        prTextBufferAppendU64(&p.out, i, 10);
        prTextBufferAppendChar(&p.out, ' ');
        reference(p.out, p.fonts.data[i].object);
    }
    prTextBufferAppend(&p.out, " >> /XObject <<");
    for (uint32_t i = 0; i < p.images.count; ++i) {
        prTextBufferAppend(&p.out, " /I");
        prTextBufferAppendU64(&p.out, i, 10);
        prTextBufferAppendChar(&p.out, ' ');
        reference(p.out, p.images.data[i].object);
    }
    for (uint32_t i = 0; i < p.forms.count; ++i) {
        prTextBufferAppend(&p.out, " /X");
        prTextBufferAppendU64(&p.out, i, 10);
        prTextBufferAppendChar(&p.out, ' ');
        reference(p.out, p.forms.data[i]);
    }
    prTextBufferAppend(&p.out, " >> /ExtGState <<");
    for (uint32_t i = 0; i < p.states.count; ++i) {
        prTextBufferAppend(&p.out, " /G");
        prTextBufferAppendU64(&p.out, i, 10);
        prTextBufferAppendChar(&p.out, ' ');
        reference(p.out, p.states.data[i].object);
    }
    prTextBufferAppend(&p.out, " >> /Pattern <<");
    for (uint32_t i = 0; i < p.patterns.count; ++i) {
        prTextBufferAppend(&p.out, " /P");
        prTextBufferAppendU64(&p.out, i, 10);
        prTextBufferAppendChar(&p.out, ' ');
        reference(p.out, p.patterns.data[i]);
    }
    prTextBufferAppend(&p.out, " >> >>");
    writeObject(p, kResources);
    // The document's information and the catalog.
    uint32_t info = 0;
    if (p.document.title.size != 0) {
        info = allocate(p);
        prTextBufferAppend(&p.out, "<< /Title ");
        textString(p.out, p.document.title);
        prTextBufferAppend(&p.out, " >>");
        writeObject(p, info);
    }
    uint32_t metadata = writeMetadata(p);
    prTextBufferAppend(&p.out, "<< /Type /Catalog /Pages ");
    reference(p.out, kPages);
    prTextBufferAppend(&p.out, " /Metadata ");
    reference(p.out, metadata);
    if (p.document.language.size != 0) {
        prTextBufferAppend(&p.out, " /Lang ");
        textString(p.out, p.document.language);
    }
    if (p.document.title.size != 0)
        prTextBufferAppend(&p.out, " /ViewerPreferences << /DisplayDocTitle true >>");
    if (tagged(p)) {
        prTextBufferAppend(&p.out, " /MarkInfo << /Marked true >> /StructTreeRoot ");
        reference(p.out, structTree);
    }
    if (outline != 0) {
        prTextBufferAppend(&p.out, " /Outlines ");
        reference(p.out, outline);
        prTextBufferAppend(&p.out, " /PageMode /UseOutlines");
    }
    prTextBufferAppend(&p.out, " >>");
    writeObject(p, kCatalog);
    flushPacked(p);
    // The file's identifier, which a document of PDF 2.0 has (ISO 32000-2, 14.4), is a hash of what precedes it. A new
    // file's two identifiers are the same.
    uint64_t made[2] = {p.hash, prHashBytes(&p.hash, sizeof(p.hash), 0x5044462d322e30ull)};
    PrTextBuffer identifier{};
    prTextBufferAppend(&identifier, " /ID [<");
    drawSharedAppendHex(identifier, made[0], 8);
    drawSharedAppendHex(identifier, made[1], 8);
    prTextBufferAppend(&identifier, "> <");
    drawSharedAppendHex(identifier, made[0], 8);
    drawSharedAppendHex(identifier, made[1], 8);
    prTextBufferAppend(&identifier, ">]");
    PrStringView id{identifier.data, identifier.size};
    if (p.options.compress != nullptr)
        writeXrefStream(p, info, id);
    else
        writeXrefTable(p, info, id);
    prTextBufferReset(&identifier);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrDrawWriter* prDrawPdfCreate(const PrDrawWriterOptions* PR_NULLABLE options) {
    PdfWriter* p = allocZeroed<PdfWriter>();
    p->destroyFn = destroyWriter;
    p->beginFn = begin;
    p->addPageFn = addPage;
    p->endFn = end;
    p->options = options != nullptr ? *options : prDrawWriterOptionsDefault();
    p->path = prPathCreate();
    DrawSharedWriterOps ops{
        .context = p,
        .knockout = true,
        .nonScalingStrokes = false,
        .gradient = pdfGradient,
        .blendMode = pdfBlendMode,
        .fill = pdfFill,
        .stroke = pdfStroke,
        .image = pdfImage,
        .glyphs = pdfGlyphs,
        .pushClip = pdfPushClip,
        .popClip = pdfPopClip,
        .pushGroup = pdfPushGroup,
        .popGroup = pdfPopGroup,
        .pushMark = pdfPushMark,
        .popMark = pdfPopMark,
    };
    drawSharedWalkerInit(p->walker, ops, p->options.glyphDecoder, p->options.rasterScale);
    return p;
}
