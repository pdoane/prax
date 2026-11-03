// The windowed view: the height index, the window of rows, the scroll position, and motion over lines as nodes.

#include "pr/text_view.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/hash.h"
#include "pr/layout_frame.h"
#include "pr/layout_query.h"
#include "pr/layout_style.h"
#include "pr/mem.h"
#include "pr/search.h"
#include "pr/text_edit_layout.h"
#include "pr/text_estimate.h"
#include "pr/text_font.h"
#include "pr/time.h"

namespace pr_impl {

namespace {

// The most lines in a chunk of the height index. A lookup walks chunks, and an edit moves the heights of one chunk.
constexpr uint32_t kChunkLines = 512;

// A change of more lines than this estimates every line again, which costs less than editing the chunks line by line.
constexpr uint32_t kSpliceLines = 4096;

// The heights of a run of lines, and their sum.
struct HeightChunk {
    Array<float> heights;           // of each line in turn, from the chunk's first line
    Array<uint8_t> exact;           // like `heights`: a row has measured the line since its text or width changed
    double sum;
};

// A sum tree of two levels over the heights of the lines. Reading nearby lines in turn costs little, because a cursor
// stays at the last chunk that was read.
struct HeightIndex {
    Array<HeightChunk> chunks;
    double total;
    uint32_t lines;
    uint32_t cursor;                // the chunk last read
    uint32_t cursorLine;            // its first line
    double cursorTop;               // its top

    void reset() {
        for (uint32_t i = 0; i < chunks.count; ++i) {
            chunks.data[i].heights.reset();
            chunks.data[i].exact.reset();
        }
        chunks.reset();
    }

    // Moves the cursor to the chunk that holds `line`, and returns the line's index in it.
    uint32_t seek(uint32_t line) {
        while (line < cursorLine) {
            --cursor;
            cursorLine -= chunks.data[cursor].heights.count;
            cursorTop -= chunks.data[cursor].sum;
        }
        while (cursor + 1 < chunks.count && line >= cursorLine + chunks.data[cursor].heights.count) {
            cursorLine += chunks.data[cursor].heights.count;
            cursorTop += chunks.data[cursor].sum;
            ++cursor;
        }
        return line - cursorLine;
    }

    float heightOf(uint32_t line) {
        uint32_t at = seek(line);
        return chunks.data[cursor].heights.data[at];
    }

    double topOf(uint32_t line) {
        uint32_t at = seek(line);
        double top = cursorTop;
        const float* heights = chunks.data[cursor].heights.data;
        for (uint32_t i = 0; i < at; ++i)
            top += double(heights[i]);
        return top;
    }

    bool isExact(uint32_t line) {
        uint32_t at = seek(line);
        return chunks.data[cursor].exact.data[at] != 0;
    }

    void set(uint32_t line, float height, bool exact) {
        uint32_t at = seek(line);
        HeightChunk& chunk = chunks.data[cursor];
        double change = double(height) - double(chunk.heights.data[at]);
        chunk.heights.data[at] = height;
        chunk.exact.data[at] = exact ? 1 : 0;
        chunk.sum += change;
        total += change;
    }

    // Returns the line at the vertical offset `y`, clamped to the document.
    uint32_t lineAt(double y) {
        while (cursor > 0 && y < cursorTop) {
            --cursor;
            cursorLine -= chunks.data[cursor].heights.count;
            cursorTop -= chunks.data[cursor].sum;
        }
        while (cursor + 1 < chunks.count && y >= cursorTop + chunks.data[cursor].sum) {
            cursorLine += chunks.data[cursor].heights.count;
            cursorTop += chunks.data[cursor].sum;
            ++cursor;
        }
        const HeightChunk& chunk = chunks.data[cursor];
        double top = cursorTop;
        uint32_t i = 0;
        while (i + 1 < chunk.heights.count && top + double(chunk.heights.data[i]) <= y)
            top += double(chunk.heights.data[i++]);
        return cursorLine + i;
    }

    void rewind() {
        cursor = 0;
        cursorLine = 0;
        cursorTop = 0;
    }

    // Removes `removed` lines at `line`, and inserts `added` of height `height`.
    void splice(uint32_t line, uint32_t removed, uint32_t added, float height) {
        for (uint32_t i = 0; i < removed; ++i) {
            uint32_t at = seek(line);
            HeightChunk& chunk = chunks.data[cursor];
            // The last line of the document keeps a chunk, so a lookup has somewhere to stop.
            total -= double(chunk.heights.data[at]);
            chunk.sum -= double(chunk.heights.data[at]);
            chunk.heights.erase(at);
            chunk.exact.erase(at);
            --lines;
            if (chunk.heights.count == 0 && chunks.count > 1) {
                chunk.heights.reset();
                chunk.exact.reset();
                chunks.erase(cursor);
                rewind();
            }
        }
        for (uint32_t i = 0; i < added; ++i) {
            uint32_t at = seek(line + i < lines ? line + i : lines);
            if (at > chunks.data[cursor].heights.count)
                at = chunks.data[cursor].heights.count;
            HeightChunk* chunk = &chunks.data[cursor];
            chunk->heights.insert(at, height);
            chunk->exact.insert(at, 0);
            chunk->sum += double(height);
            total += double(height);
            ++lines;
            if (chunk->heights.count > kChunkLines) {
                // The upper half moves to a chunk of its own.
                HeightChunk upper{};
                uint32_t half = chunk->heights.count / 2;
                upper.heights.append(chunk->heights.data + half, chunk->heights.count - half);
                upper.exact.append(chunk->exact.data + half, chunk->exact.count - half);
                for (uint32_t k = 0; k < upper.heights.count; ++k)
                    upper.sum += double(upper.heights.data[k]);
                chunk->heights.count = half;
                chunk->exact.count = half;
                chunk->sum -= upper.sum;
                chunks.insert(cursor + 1, upper);
                rewind();
            }
        }
    }
};

// A row of the window: its line, its block, and the range of the view's pieces that show its text.
struct Row {
    uint32_t line;
    PrLayoutNode box;
    uint32_t pieceStart;
    uint32_t pieceCount;
};

// A text node of a row, and the byte of the line where its text starts.
struct RowPiece {
    PrLayoutNode node;
    uint32_t start;
    uint32_t row;                   // by index of the view's rows
};

// How much of a row's height a line must change by to count as a correction, in px.
constexpr float kHeightTolerance = 0.01f;

// How many lines refinement reads between looks at the clock.
constexpr uint32_t kRefineBatch = 128;

// A guess at a line's height and at the advance of a byte until a row has been measured, in px.
constexpr float kFirstLineHeight = 18;
constexpr float kFirstAdvance = 8;

// A view: its text, the height index, the window of rows and the scroll position.
struct TextViewState {
    PrTextEditBuffer* buffer;
    PrTextEditView* edit;
    PrTextEditStream* stream;
    HeightIndex heights;

    // The scroll position: the top of the line that an anchor holds, and how far into that line the viewport starts.
    PrTextEditAnchor anchor;
    float into;

    // The scroll offset along the inline axis, as the tree holds it, which is negative toward an origin at the right or
    // the bottom.
    double scrollInline;

    float lineHeight;
    float advance;
    bool measured;                  // a row has given the line height, and the advance if there is no estimator
    float estimatedWidth;           // the inline size that the estimates assume
    bool wrap;

    // How the rows lie, from the record of a row.
    bool vertical;                  // lines run along the y axis, and rows stack along x
    bool blockReversed;             // rows stack toward the left, and block offsets are negative in the tree
    bool inlineReversed;            // lines run toward the left or the top, and inline offsets are negative in the tree

    PrTextFont* PR_NULLABLE font;           // the font of a row's record
    PrTextEstimator* PR_NULLABLE estimator; // of `font`, along the inline axis
    uint32_t refineLine;                    // the next line that refinement reads
    bool refining;                          // some line has not been read since the last estimate of every line

    PrTextViewFrameDesc frame;
    bool framed;
    bool reveal;

    Array<Row> rows;
    Array<RowPiece> pieces;
    uint32_t lastPiece;             // the piece that the last lookup by node found
    uint64_t rowsVersion;           // the version of the text that `rows` shows
    bool changedView;               // the next frame shows something different

    // What the geometry answers with, valid until its next call.
    Array<char> lineText;
    uint32_t lineTextLine;
    bool hasLineText;
    Array<PrLayoutLineStop> found;
    Array<PrTextEditLineStop> stops;

    Array<char> bytes;
    Array<PrLayoutHighlight> highlights;
    Array<PrLayoutCaret> carets;
    Array<PrTextEditSelection> scratch;
    Array<PrTextEditChange> changes;

    // The spans of the window, in the frame that the view declares, the spans of each layer after those of the layer
    // before it. Each layer's spans end at its entry of `layerEnds`.
    Array<PrTextViewSpan> spans;
    uint32_t layerEnds[kPrTextViewMaxSpanLayers];
    uint32_t layerCount;

    Array<PrTextViewGap> gaps;
};

uint32_t lineBytes(const PrTextEditBuffer* buffer, uint32_t line) {
    return prTextEditBufferLineEnd(buffer, line) - prTextEditBufferLineStart(buffer, line);
}

// The parts of the window that the view keys by line.
enum {
    RowPart_Row,
    RowPart_Gap,                    // the block that fills the gap before a line
};
typedef uint8_t RowPart;

// Returns the key of `part` of `line`, which keeps the line's nodes across frames so that layout reuses what it shaped.
// A key is a node's identity in the tree, and the view's own address is unique, so it is the identity of the view's
// rows, as a DOM node's address is its own.
uint64_t rowKey(const TextViewState* v, RowPart part, uint32_t line) {
    return hashMixKeys(uint64_t(reinterpret_cast<uintptr_t>(v)), uint64_t(line) << 1 | part);
}

// Returns the key of the sheet that holds the rows of lines that do not wrap, past every key of a line's part.
uint64_t sheetKey(const TextViewState* v) {
    return hashMixKeys(uint64_t(reinterpret_cast<uintptr_t>(v)), uint64_t(1) << 40);
}

// Returns the size of the gap before `line`, or zero.
float gapBefore(const TextViewState* v, uint32_t line) {
    const Array<PrTextViewGap>& gaps = v->gaps;
    uint32_t at = partitionPoint(0, gaps.count, [&](uint32_t i) { return gaps.data[i].line < line; });
    return at < gaps.count && gaps.data[at].line == line ? gaps.data[at].size : 0;
}

// Returns the space that the gaps add to the height of `line`: the gap before it, and for the last line, the gap after
// it.
float gapOf(const TextViewState* v, uint32_t line) {
    float gap = gapBefore(v, line);
    uint32_t lines = prTextEditBufferLineCount(v->buffer);
    return line + 1 == lines ? gap + gapBefore(v, lines) : gap;
}

// Returns the height that the view expects of a line of `bytes` bytes.
float estimateHeight(const TextViewState* v, uint32_t bytes) {
    if (!v->wrap || v->estimatedWidth <= 0)
        return v->lineHeight;
    uint32_t perRow = uint32_t(v->estimatedWidth / v->advance);
    perRow = perRow != 0 ? perRow : 1;
    return v->lineHeight * float(1 + (bytes != 0 ? (bytes - 1) / perRow : 0));
}

// Returns the size of a box along the block axis or the inline axis.
float blockSizeOf(const TextViewState* v, PrRect box) {
    return v->vertical ? box.right - box.left : box.bottom - box.top;
}

float inlineSizeOf(const TextViewState* v, PrRect box) {
    return v->vertical ? box.bottom - box.top : box.right - box.left;
}

// Returns the port's size along the block axis or the inline axis.
float portBlock(const TextViewState* v) {
    return v->vertical ? v->frame.extent.x : v->frame.extent.y;
}

float portInline(const TextViewState* v) {
    return v->vertical ? v->frame.extent.y : v->frame.extent.x;
}

// Returns the room along the inline axis that the gutter leaves the text.
float textInline(const TextViewState* v) {
    float room = portInline(v) - (v->frame.gutter != nullptr ? v->frame.gutterSize : 0);
    return room > 0 ? room : 0;
}

// The physical side of a row where its lines start, by the rows' writing mode and direction.
PrRectSide inlineStartSide(const TextViewState* v) {
    if (v->vertical)
        return v->inlineReversed ? PrRectSide_Bottom : PrRectSide_Top;
    return v->inlineReversed ? PrRectSide_Right : PrRectSide_Left;
}

// Returns a record of `box` with the font of a row, so that what it holds lies in the rows' writing mode, filled with
// `fill` unless it has zero alpha.
PrLayoutStyleIndex rowPartStyle(TextViewState* v, const PrLayoutBoxStyle& box, PrColor16 fill) {
    PrLayoutTree* tree = v->frame.tree;
    PrLayoutStyle style = prLayoutStyleDefault();
    style.groups[PrLayoutGroup_Box] = prLayoutBoxStyleIntern(tree, &box);
    style.groups[PrLayoutGroup_Font] = prLayoutStyleRecord(tree, v->frame.rowStyle).groups[PrLayoutGroup_Font];
    if (fill.a > 0) {
        PrLayoutDecorationStyle decoration = prLayoutDecorationStyleDefault();
        decoration.background = fill;
        style.groups[PrLayoutGroup_Decoration] = prLayoutDecorationStyleIntern(tree, &decoration);
    }
    return prLayoutStyleIntern(tree, &style);
}

// Returns the record of the sheet that holds the rows of lines that do not wrap: a block as long as the longest of
// them, and at least as long as the port, so that every row, and its fill, spans whatever the lines scroll through.
PrLayoutStyleIndex sheetStyle(TextViewState* v) {
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_Block;
    PrLayoutLength& size = v->vertical ? box.height : box.width;
    PrLayoutLength& least = v->vertical ? box.minHeight : box.minWidth;
    size = prLayoutLengthMaxContent();
    least = prLayoutLengthPercent(100);
    return rowPartStyle(v, box, PrColor16{});
}

// Returns the record of a row that holds a gutter's cell and the text's block, filled with `fill`: a flex row of the
// two along the inline axis.
PrLayoutStyleIndex wrapperStyle(TextViewState* v, PrColor16 fill) {
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_Flex;
    return rowPartStyle(v, box, fill);
}

// Returns the record of a gutter's cell, which spans its row along the block axis and sticks to the port's inline start
// while lines that do not wrap scroll along it.
PrLayoutStyleIndex cellStyle(TextViewState* v) {
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_Block;
    box.position = PrLayoutPosition_Sticky;
    box.inset[inlineStartSide(v)] = prLayoutLengthPx(0);
    PrLayoutLength& size = v->vertical ? box.height : box.width;
    size = prLayoutLengthPx(v->frame.gutterSize);
    PrLayoutTree* tree = v->frame.tree;
    PrLayoutStyle style = prLayoutStyleRecord(tree, rowPartStyle(v, box, v->frame.gutterBackground));
    PrLayoutFlexStyle flex = prLayoutFlexStyleDefault();
    flex.shrink = 0;
    style.groups[PrLayoutGroup_Flex] = prLayoutFlexStyleIntern(tree, &flex);
    return prLayoutStyleIntern(tree, &style);
}

// Returns the record of the block that fills a gap of `size` px.
PrLayoutStyleIndex gapStyle(TextViewState* v, float size) {
    PrLayoutBoxStyle box = prLayoutBoxStyleDefault();
    box.display = PrLayoutDisplay_Block;
    if (v->vertical)
        box.width = prLayoutLengthPx(size);
    else
        box.height = prLayoutLengthPx(size);
    return rowPartStyle(v, box, v->frame.gapColor);
}

// Estimates the height of every line from its length. Its cost is proportional to the number of lines. Refinement then
// reads the text of each.
void estimateAll(TextViewState* v) {
    HeightIndex& index = v->heights;
    index.reset();
    index.total = 0;
    index.lines = prTextEditBufferLineCount(v->buffer);
    index.rewind();
    for (uint32_t line = 0; line < index.lines;) {
        HeightChunk chunk{};
        uint32_t count = index.lines - line < kChunkLines / 2 ? index.lines - line : kChunkLines / 2;
        chunk.heights.resize(count);
        chunk.exact.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            float height = estimateHeight(v, lineBytes(v->buffer, line + i));
            if (v->gaps.count != 0)
                height += gapOf(v, line + i);
            chunk.heights.data[i] = height;
            chunk.exact.data[i] = 0;
            chunk.sum += double(height);
        }
        index.total += chunk.sum;
        index.chunks.push(chunk);
        line += count;
    }
    v->refineLine = 0;
    v->refining = v->wrap && v->estimator != nullptr;
}

// Returns the height that the estimator gives a line from its text.
float refinedHeight(TextViewState* v, uint32_t line) {
    uint32_t start = prTextEditBufferLineStart(v->buffer, line);
    uint32_t end = prTextEditBufferLineEnd(v->buffer, line);
    PrStringView text = prTextEditBufferChunk(v->buffer, start);
    if (text.size < end - start) {
        // The line lies across the gap.
        v->bytes.resize(end - start);
        prTextEditBufferCopy(v->buffer, start, end, v->bytes.data);
        text = PrStringView{v->bytes.data, v->bytes.count};
    }
    text.size = end - start;
    float gap = v->gaps.count != 0 ? gapOf(v, line) : 0;
    return v->lineHeight * float(prTextEstimatorLineCount(v->estimator, text, v->estimatedWidth)) + gap;
}

// Brings the height index up to the changes that the buffer has made.
void followEdits(TextViewState* v) {
    PrTextEditRecord records[64];
    for (;;) {
        uint32_t count = prTextEditStreamTake(v->stream, records, 64);
        if (count == 0)
            return;
        for (uint32_t i = 0; i < count; ++i) {
            const PrTextEditRecord& r = records[i];
            uint32_t removed = r.oldEndPoint.line - r.startPoint.line + 1;
            uint32_t added = r.newEndPoint.line - r.startPoint.line + 1;
            if (removed > kSpliceLines || added > kSpliceLines) {
                estimateAll(v);
                // The stream's later records are in what this estimate has already read.
                while (prTextEditStreamTake(v->stream, records, 64) != 0) {
                }
                return;
            }
            // An edit within a line keeps its height until a row measures it again, so typing moves nothing.
            if (removed == 1 && added == 1)
                continue;
            // The lengths are of the text as it stands, which is close enough for one more record to correct.
            v->heights.splice(r.startPoint.line, removed, added, v->lineHeight);
            if (v->refineLine > r.startPoint.line) {
                uint32_t past = v->refineLine > r.oldEndPoint.line ? v->refineLine - r.oldEndPoint.line - 1 : 0;
                v->refineLine = r.startPoint.line + added + past;
            }
            if (v->wrap && v->estimator != nullptr) {
                uint32_t lines = prTextEditBufferLineCount(v->buffer);
                for (uint32_t k = 0; k < added && r.startPoint.line + k < lines; ++k) {
                    uint32_t line = r.startPoint.line + k;
                    v->heights.set(line, refinedHeight(v, line), false);
                }
            }
        }
    }
}

double scrollTop(TextViewState* v) {
    uint32_t offset = prTextEditAnchorOffset(v->buffer, v->anchor);
    uint32_t line = prTextEditBufferPointFromOffset(v->buffer, offset).line;
    return v->heights.topOf(line) + double(v->into);
}

// Returns the greatest scroll offset.
double scrollLimit(TextViewState* v) {
    double limit = v->heights.total - double(portBlock(v));
    return limit > 0 ? limit : 0;
}

void setScroll(TextViewState* v, double y) {
    double limit = scrollLimit(v);
    y = y > limit ? limit : y;
    y = y < 0 ? 0 : y;
    uint32_t line = v->heights.lineAt(y);
    double top = v->heights.topOf(line);
    prTextEditAnchorRemove(v->buffer, v->anchor);
    v->anchor = prTextEditAnchorCreate(v->buffer, prTextEditBufferLineStart(v->buffer, line), PrTextEditBias_Left);
    v->into = float(y - top);
}

// Places the tree's scroll offset for the rows that it holds.
void placeScroll(TextViewState* v) {
    if (!v->framed || v->rows.count == 0)
        return;
    double y = scrollTop(v) - v->heights.topOf(v->rows.data[0].line);
    float block = float(y > 0 ? y : 0);
    block = v->blockReversed ? -block : block;
    float along = float(v->wrap ? 0 : v->scrollInline);
    PrVec2 offset = v->vertical ? PrVec2{block, along} : PrVec2{along, block};
    prLayoutNodeSetScroll(v->frame.tree, v->frame.parent, prLayoutVecFromPx(offset));
}

PrTextEditPosition positionOf(const TextViewState* v, uint32_t offset, PrTextEditAffinity affinity) {
    PrTextEditPoint point = prTextEditBufferPointFromOffset(v->buffer, offset);
    return PrTextEditPosition{uint64_t(point.line) + 1, point.column, affinity};
}

uint32_t offsetOf(const TextViewState* v, PrTextEditPosition position) {
    return prTextEditBufferLineStart(v->buffer, uint32_t(position.node - 1)) + position.offset;
}

// Brings the inline scroll offset within the range of the last layout, which the rows' scroll was clamped to, so that a
// scroll past the longest line in the window leaves nothing to take back.
void clampInline(TextViewState* v) {
    PrLayoutScrollInfo info{};
    if (v->wrap || !prLayoutNodeScrollInfo(v->frame.tree, v->frame.parent, &info))
        return;
    PrRect range = prLayoutRectToPx(info.range);
    double low = double(v->vertical ? range.top : range.left);
    double high = double(v->vertical ? range.bottom : range.right);
    v->scrollInline = v->scrollInline < low ? low : (v->scrollInline > high ? high : v->scrollInline);
}

// Adds `delta` to the inline scroll offset, which stays on the side of the origin that the direction gives it.
void scrollInlineBy(TextViewState* v, double delta) {
    v->scrollInline += delta;
    if (v->inlineReversed)
        v->scrollInline = v->scrollInline < 0 ? v->scrollInline : 0;
    else
        v->scrollInline = v->scrollInline > 0 ? v->scrollInline : 0;
}

// Copies a line's text, which stays valid until another line is asked for.
PrStringView lineTextOf(TextViewState* v, uint32_t line) {
    if (!v->hasLineText || v->lineTextLine != line) {
        uint32_t start = prTextEditBufferLineStart(v->buffer, line);
        uint32_t end = prTextEditBufferLineEnd(v->buffer, line);
        v->lineText.resize(end - start);
        prTextEditBufferCopy(v->buffer, start, end, v->lineText.data);
        v->lineTextLine = line;
        v->hasLineText = true;
    }
    return PrStringView{v->lineText.data, v->lineText.count};
}

// Returns the row of `line` if it shows the text as it stands, or null.
const Row* PR_NULLABLE rowOf(const TextViewState* v, uint32_t line) {
    if (v->rows.count == 0 || v->rowsVersion != prTextEditBufferVersion(v->buffer))
        return nullptr;
    uint32_t first = v->rows.data[0].line;
    return line >= first && line - first < v->rows.count ? &v->rows.data[line - first] : nullptr;
}

// Returns the piece of `row` that shows byte `column` of its line. An offset where two pieces meet is in the second, or
// in the first if it stands upstream.
const RowPiece& pieceAt(const TextViewState* v, const Row& row, uint32_t column, PrTextEditAffinity affinity) {
    const RowPiece* pieces = v->pieces.data + row.pieceStart;
    uint32_t i = 0;
    while (i + 1 < row.pieceCount && pieces[i + 1].start <= column)
        ++i;
    if (affinity == PrTextEditAffinity_Upstream && i > 0 && pieces[i].start == column)
        --i;
    return pieces[i];
}

// Returns the piece of the window that shows the text node `node`, or null. Stops of one line come mostly from one
// node, so the search starts at the last piece that it found.
const RowPiece* PR_NULLABLE pieceOfNode(TextViewState* v, PrLayoutNode node) {
    if (v->lastPiece < v->pieces.count && v->pieces.data[v->lastPiece].node == node)
        return &v->pieces.data[v->lastPiece];
    for (uint32_t i = 0; i < v->pieces.count; ++i) {
        if (v->pieces.data[i].node == node) {
            v->lastPiece = i;
            return &v->pieces.data[i];
        }
    }
    return nullptr;
}

// Answers for a line outside the window: the clusters of its bytes as one line, at the advance of the view.
bool estimatedLine(TextViewState* v, uint32_t line, PrTextEditLine* out) {
    PrLayoutLineInfo info{};
    if (v->rows.count != 0)
        prLayoutNodeLineInfo(v->frame.tree, v->pieces.data[0].node, 0, &info);
    PrStringView text = lineTextOf(v, line);
    PrTextEditGeometry own = prTextEditGeometryFromText(text, info.direction);
    PrTextEditLine answered{};
    PrTextEditPosition at{uint64_t(line) + 1, 0, PrTextEditAffinity_Downstream};
    bool ok = own.line(own.context, at, PrTextEditLineStep_Same, &answered);
    v->stops.clear();
    for (uint32_t i = 0; ok && i < answered.stopCount; ++i) {
        PrTextEditLineStop stop = answered.stops[i];
        stop.along *= v->advance;
        v->stops.push(stop);
    }
    prTextEditGeometryReset(&own);
    if (!ok)
        return false;
    *out = answered;
    out->stops = v->stops.data;
    out->flow = prTextEditFlowFromWritingMode(info.writingMode);
    return true;
}

bool geometryLine(void* PR_NULLABLE context, PrTextEditPosition at, PrTextEditLineStep step, PrTextEditLine* out) {
    TextViewState* v = static_cast<TextViewState*>(context);
    uint32_t line = uint32_t(at.node - 1);
    uint32_t count = prTextEditBufferLineCount(v->buffer);
    const Row* row = rowOf(v, line);
    if (row != nullptr) {
        PrLayoutLineInfo info{};
        PrLayoutLineStep layoutStep = PrLayoutLineStep_Same;
        if (step == PrTextEditLineStep_Next)
            layoutStep = PrLayoutLineStep_Next;
        else if (step == PrTextEditLineStep_Previous)
            layoutStep = PrLayoutLineStep_Previous;
        const RowPiece& piece = pieceAt(v, *row, at.offset, at.affinity);
        PrLayoutTextPosition position{piece.node, at.offset - piece.start};
        uint32_t stops = 0;
        // A line of the gutter, which holds no text of the document, is stepped past to the next line of a row.
        for (uint32_t attempt = 0; attempt < 4; ++attempt) {
            uint32_t capacity = v->found.count;
            stops = prLayoutLineStops(v->frame.tree, v->frame.parent, position, at.affinity, layoutStep, &info,
                v->found.data, capacity);
            if (stops > capacity) {
                v->found.resize(stops);
                prLayoutLineStops(v->frame.tree, v->frame.parent, position, at.affinity, layoutStep, &info,
                    v->found.data, stops);
            }
            if (stops == 0 || layoutStep == PrLayoutLineStep_Same || pieceOfNode(v, v->found.data[0].position.node))
                break;
            position = v->found.data[0].position;
        }
        if (stops != 0) {
            v->stops.clear();
            uint32_t startStop = 0;
            uint32_t endStop = 0;
            for (uint32_t i = 0; i < stops; ++i) {
                const PrLayoutLineStop& stop = v->found.data[i];
                const RowPiece* holder = pieceOfNode(v, stop.position.node);
                if (holder == nullptr)
                    continue;
                uint32_t held = v->rows.data[holder->row].line;
                uint32_t offset = holder->start + stop.position.offset;
                // What a row shows past its line, so that an empty line has a place, is no place for a caret.
                uint32_t bytes = lineBytes(v->buffer, held);
                if (offset > bytes)
                    continue;
                // Where two runs meet, each has a stop at one place, which is one place for a caret, with the text
                // after it.
                PrTextEditLineStop* last = v->stops.count != 0 ? &v->stops.last() : nullptr;
                if (last != nullptr && last->position.node == uint64_t(held) + 1 && last->position.offset == offset &&
                        last->along == stop.along) {
                    last->position.affinity = PrTextEditAffinity_Downstream;
                } else {
                    v->stops.push(PrTextEditLineStop{{uint64_t(held) + 1, offset, stop.affinity}, stop.along});
                }
                if (i == info.startStop)
                    startStop = v->stops.count - 1;
                if (i == info.endStop || offset == bytes)
                    endStop = v->stops.count - 1;
            }
            if (v->stops.count != 0) {
                *out = PrTextEditLine{
                    .stops = v->stops.data,
                    .stopCount = v->stops.count,
                    .flow = prTextEditFlowFromWritingMode(info.writingMode),
                    .direction = info.direction,
                    .startStop = startStop,
                    .endStop = endStop,
                };
                return true;
            }
        }
    }
    // Past the window's edge, or outside it, the next line is the next logical line.
    if (step == PrTextEditLineStep_Next) {
        if (line + 1 >= count)
            return false;
        line += 1;
    } else if (step == PrTextEditLineStep_Previous) {
        if (line == 0)
            return false;
        line -= 1;
    }
    return estimatedLine(v, line, out);
}

PrStringView geometryText(void* PR_NULLABLE context, uint64_t node) {
    return lineTextOf(static_cast<TextViewState*>(context), uint32_t(node - 1));
}

uint64_t geometryStep(void* PR_NULLABLE context, uint64_t node, PrTextEditLineStep step, bool* contiguous) {
    const TextViewState* v = static_cast<const TextViewState*>(context);
    *contiguous = false;
    if (step == PrTextEditLineStep_Next)
        return node < prTextEditBufferLineCount(v->buffer) ? node + 1 : 0;
    return node > 1 ? node - 1 : 0;
}

PrTextEditGeometry geometryOf(TextViewState* v) {
    return PrTextEditGeometry{.context = v, .line = geometryLine, .nodeText = geometryText, .nodeStep = geometryStep};
}

// Writes the selections in the order of the document to `out`.
void selectionsInOrder(const TextViewState* v, Array<PrTextEditSelection>& out) {
    uint32_t count = prTextEditViewSelectionCount(v->edit);
    const PrTextEditSelection* selections = prTextEditViewSelections(v->edit);
    out.clear();
    // The selections ascend, but for the last.
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t low = prTextEditSelectionStart(selections[i]);
        uint32_t at = i;
        while (i + 1 == count && at > 0) {
            const PrTextEditSelection& before = out.data[at - 1];
            if ((prTextEditSelectionStart(before)) <= low)
                break;
            --at;
        }
        out.insert(at, selections[i]);
    }
}

// Gives the view `selections`, with the last as primary, and shows the result.
void setSelections(TextViewState* v, const PrTextEditSelection* selections, uint32_t count) {
    prTextEditViewSetSelections(v->edit, selections, count);
    v->reveal = true;
    v->changedView = true;
}

void setPrimary(TextViewState* v, PrTextEditSelection selection) {
    setSelections(v, &selection, 1);
}

// Returns `s` after `motion`, as prTextEditSelectionMove does for a text of one node.
PrTextEditSelection movedSelection(TextViewState* v, PrTextEditSelection s, PrTextEditMotion motion,
        PrTextEditSelectionMove kind) {
    PrTextEditGeometry g = geometryOf(v);
    bool extend = kind == PrTextEditSelectionMove_Extend;
    uint32_t low = prTextEditSelectionStart(s);
    uint32_t high = prTextEditSelectionEnd(s);
    PrTextEditPosition at = positionOf(v, s.caret, s.affinity);
    if (motion == PrTextEditMotion_TextStart || motion == PrTextEditMotion_TextEnd) {
        uint32_t to = motion == PrTextEditMotion_TextStart ? 0 : prTextEditBufferSize(v->buffer);
        return PrTextEditSelection{.anchor = extend ? s.anchor : to, .caret = to};
    }
    bool between = motion == PrTextEditMotion_LinePrevious || motion == PrTextEditMotion_LineNext;
    PrTextEditLine line{};
    PrTextDirection direction = PrTextDirection_Ltr;
    if (g.line(g.context, at, PrTextEditLineStep_Same, &line))
        direction = line.direction;
    PrTextEditSelectionEnd end = PrTextEditSelectionEnd_None;
    if (!extend && low != high)
        end = prTextEditMotionCollapseEnd(motion, direction);
    if (end != PrTextEditSelectionEnd_None) {
        uint32_t collapsed = end == PrTextEditSelectionEnd_Low ? low : high;
        if (!between)
            return PrTextEditSelection{.anchor = collapsed, .caret = collapsed};
        at = positionOf(v, collapsed, PrTextEditAffinity_Downstream);
    }
    float goal = s.goal;
    bool hasGoal = between && s.hasGoal && end == PrTextEditSelectionEnd_None;
    PrTextEditPosition moved = prTextEditMotionMove(&g, at, motion, &goal, &hasGoal);
    uint32_t to = offsetOf(v, moved);
    return PrTextEditSelection{
        .anchor = extend ? s.anchor : to,
        .caret = to,
        .goal = hasGoal ? goal : 0,
        .hasGoal = hasGoal,
        .affinity = moved.affinity,
    };
}

// Returns where a logical motion from a caret lands.
uint32_t landing(TextViewState* v, PrTextEditSelection s, PrTextEditMotion motion) {
    PrTextEditGeometry g = geometryOf(v);
    float goal = 0;
    bool hasGoal = false;
    return offsetOf(v, prTextEditMotionMove(&g, positionOf(v, s.caret, s.affinity), motion, &goal, &hasGoal));
}

// Returns where a deletion by `command` from a caret reaches.
uint32_t deletionEnd(TextViewState* v, PrTextEditSelection s, PrTextEditCommand command) {
    switch (command) {
        case PrTextEditCommand_DeleteBackward: {
            PrTextEditPoint point = prTextEditBufferPointFromOffset(v->buffer, s.caret);
            if (point.column == 0)
                return s.caret != 0 ? s.caret - 1 : 0;
            return s.caret - point.column + prTextEditBackspaceStart(lineTextOf(v, point.line), point.column);
        }
        case PrTextEditCommand_DeleteForward: return landing(v, s, PrTextEditMotion_ClusterAfter);
        case PrTextEditCommand_DeleteWordBefore: return landing(v, s, PrTextEditMotion_WordBefore);
        default: return landing(v, s, PrTextEditMotion_WordAfter);
    }
}

// Removes what each selection holds, and for a selection that holds nothing, what `command` reaches from its caret, if
// it is a deletion. Ranges that meet join, since a transaction's changes do not overlap.
void eraseSelections(TextViewState* v, PrTextEditCommand command) {
    selectionsInOrder(v, v->scratch);
    v->changes.clear();
    bool deleting = true;
    for (uint32_t i = 0; i < v->scratch.count; ++i) {
        const PrTextEditSelection& s = v->scratch.data[i];
        uint32_t low = prTextEditSelectionStart(s);
        uint32_t high = prTextEditSelectionEnd(s);
        if (low != high) {
            deleting = false;
        } else if (command != PrTextEditCommand_None) {
            uint32_t at = deletionEnd(v, s, command);
            low = at < s.caret ? at : s.caret;
            high = at < s.caret ? s.caret : at;
        }
        if (low == high)
            continue;
        if (v->changes.count != 0 && v->changes.last().end >= low) {
            uint32_t reach = v->changes.last().end;
            v->changes.last().end = high > reach ? high : reach;
            continue;
        }
        v->changes.push(PrTextEditChange{.start = low, .end = high});
    }
    if (v->changes.count == 0)
        return;
    PrTextEditMerge merge = deleting ? PrTextEditMerge_Deleting : PrTextEditMerge_None;
    prTextEditViewApply(v->edit, v->changes.data, v->changes.count, PrTextEditOrigin_Local, merge);
    v->reveal = true;
    v->changedView = true;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrTextView : TextViewState {};

PrTextView* prTextViewCreate(PrTextEditBuffer* buffer) {
    PrTextView* v = allocZeroed<PrTextView>();
    v->buffer = buffer;
    v->edit = prTextEditViewCreate(buffer);
    v->stream = prTextEditStreamCreate(buffer);
    v->anchor = prTextEditAnchorCreate(buffer, 0, PrTextEditBias_Left);
    v->lineHeight = kFirstLineHeight;
    v->advance = kFirstAdvance;
    estimateAll(v);
    return v;
}

void prTextViewDestroy(PrTextView* PR_NULLABLE view) {
    if (view == nullptr)
        return;
    prTextEstimatorDestroy(view->estimator);
    prTextEditAnchorRemove(view->buffer, view->anchor);
    prTextEditStreamDestroy(view->stream);
    prTextEditViewDestroy(view->edit);
    view->heights.reset();
    view->rows.reset();
    view->pieces.reset();
    view->lineText.reset();
    view->found.reset();
    view->stops.reset();
    view->bytes.reset();
    view->highlights.reset();
    view->carets.reset();
    view->scratch.reset();
    view->changes.reset();
    view->spans.reset();
    view->gaps.reset();
    prMemoryFree(view);
}

PrTextEditView* prTextViewEdit(const PrTextView* view) {
    return view->edit;
}

void prTextViewFrame(PrTextView* view, const PrTextViewFrameDesc* desc) {
    PrTextView* v = view;
    v->frame = *desc;
    v->framed = true;
    v->hasLineText = false;
    followEdits(v);
    PrLayoutStyle record = prLayoutStyleRecord(desc->tree, desc->rowStyle);
    PrLayoutFontStyle fontStyle = prLayoutFontStyleRecord(desc->tree, record.groups[PrLayoutGroup_Font]);
    PrLayoutWritingMode mode = fontStyle.writingMode;
    bool vertical = mode != PrLayoutWritingMode_HorizontalTb;
    bool rtl = fontStyle.direction == PrTextDirection_Rtl;
    bool changedShape = v->wrap != desc->wrap || v->vertical != vertical || v->font != fontStyle.font;
    v->vertical = vertical;
    v->blockReversed = mode == PrLayoutWritingMode_VerticalRl || mode == PrLayoutWritingMode_SidewaysRl;
    v->inlineReversed = vertical ? rtl != (mode == PrLayoutWritingMode_SidewaysLr) : rtl;
    if (v->font != fontStyle.font) {
        prTextEstimatorDestroy(v->estimator);
        v->estimator = nullptr;
        v->font = fontStyle.font;
        if (v->font != nullptr) {
            v->estimator = prTextEstimatorCreate(v->font, vertical ? PrTextAxis_Vertical : PrTextAxis_Horizontal);
            v->advance = prTextEstimatorByteAdvance(v->estimator);
        }
    }
    float inlineSize = textInline(v);
    changedShape = changedShape || (desc->wrap && v->estimatedWidth != inlineSize);
    v->wrap = desc->wrap;
    if (changedShape) {
        v->estimatedWidth = inlineSize;
        estimateAll(v);
    }
    HeightIndex& index = v->heights;
    float port = portBlock(v);
    if (v->reveal) {
        // Near enough to show the caret's row. prTextViewLaidOut settles it, by the row's own place.
        uint32_t caretLine = prTextEditBufferPointFromOffset(v->buffer, prTextEditViewPrimary(v->edit).caret).line;
        double top = index.topOf(caretLine);
        double height = double(index.heightOf(caretLine));
        double y = scrollTop(v);
        // A line taller than the viewport shows from its top, and prTextViewLaidOut then finds the caret in it.
        if (top < y || (top + height > y + double(port) && height > double(port) && top > y))
            setScroll(v, top);
        else if (top + height > y + double(port) && height <= double(port))
            setScroll(v, top + height - double(port));
    }
    double y = scrollTop(v);
    uint32_t lines = prTextEditBufferLineCount(v->buffer);
    // The window holds a page of overscan each way.
    uint32_t first = index.lineAt(y - double(port));
    double bottom = y + 2 * double(port);
    v->rows.clear();
    v->pieces.clear();
    v->carets.clear();
    uint32_t selectionCount = prTextEditViewSelectionCount(v->edit);
    const PrTextEditSelection* selections = prTextEditViewSelections(v->edit);
    uint32_t runCount = desc->styleCount != 0 ? prTextEditBufferStyleRunCount(v->buffer) : 0;
    const PrTextEditStyleRun* runs = runCount != 0 ? prTextEditBufferStyleRuns(v->buffer) : nullptr;
    // The last run that starts at or before the window, so that each line continues from where the last ended.
    uint32_t run = 0;
    if (runCount != 0) {
        uint32_t windowStart = prTextEditBufferLineStart(v->buffer, first);
        uint32_t after = partitionPoint(1, runCount, [&](uint32_t i) { return runs[i].start <= windowStart; });
        run = after - 1;
    }
    // The window's lines, and the spans of their text, which the caller gives once for the whole window.
    uint32_t last = first;
    for (double probe = index.topOf(first); last < lines && probe < bottom; ++last)
        probe += double(index.heightOf(last));
    v->spans.clear();
    v->layerCount = desc->spanLayerCount < kPrTextViewMaxSpanLayers ? desc->spanLayerCount : kPrTextViewMaxSpanLayers;
    // Where each layer's spans start, and the first of them that the line being declared may reach.
    uint32_t layerSpan[kPrTextViewMaxSpanLayers];
    for (uint32_t l = 0; l < v->layerCount; ++l) {
        const PrTextViewSpanLayer& layer = desc->spanLayers[l];
        uint32_t at = v->spans.count;
        layerSpan[l] = at;
        if (last > first) {
            uint32_t windowStart = prTextEditBufferLineStart(v->buffer, first);
            uint32_t windowEnd = prTextEditBufferLineEnd(v->buffer, last - 1);
            uint32_t room = v->spans.capacity > at + 64 ? v->spans.capacity - at : 64;
            v->spans.resize(at + room);
            uint32_t total = layer.fn(layer.context, windowStart, windowEnd, v->spans.data + at, room);
            if (total > room) {
                v->spans.resize(at + total);
                layer.fn(layer.context, windowStart, windowEnd, v->spans.data + at, total);
            }
            v->spans.count = at + total;
        }
        v->layerEnds[l] = v->spans.count;
    }
    // Lines that wrap lie in the port, and lines that do not in a sheet as long as the longest of them.
    PrLayoutNode sheet = desc->parent;
    if (!v->wrap)
        sheet = prLayoutTreeFrameElement(desc->tree, desc->parent, sheetKey(v), sheetStyle(v));
    // With a gutter or colored lines, a row is a flex row of the gutter's cell and the text's block.
    bool wrapped = desc->gutter != nullptr || desc->lineColor != nullptr;
    PrLayoutStyleIndex plainWrapper = wrapped ? wrapperStyle(v, PrColor16{}) : 0;
    PrLayoutStyleIndex cell = desc->gutter != nullptr ? cellStyle(v) : 0;
    for (uint32_t line = first; line < last; ++line) {
        uint32_t start = prTextEditBufferLineStart(v->buffer, line);
        uint32_t end = prTextEditBufferLineEnd(v->buffer, line);
        // An empty line shows a space, so that it has a line box and a place for a caret.
        v->bytes.resize(end - start);
        prTextEditBufferCopy(v->buffer, start, end, v->bytes.data);
        if (start == end)
            v->bytes.push(' ');
        float gap = v->gaps.count != 0 ? gapBefore(v, line) : 0;
        if (gap > 0)
            prLayoutTreeFrameElement(desc->tree, sheet, rowKey(v, RowPart_Gap, line), gapStyle(v, gap));
        PrLayoutNode box = kPrLayoutNodeNone;
        PrLayoutNode holder = kPrLayoutNodeNone;
        if (wrapped) {
            PrColor16 fill = desc->lineColor != nullptr ? desc->lineColor(desc->lineColorContext, line) : PrColor16{};
            PrLayoutStyleIndex wrapper = fill.a > 0 ? wrapperStyle(v, fill) : plainWrapper;
            box = prLayoutTreeFrameElement(desc->tree, sheet, rowKey(v, RowPart_Row, line), wrapper);
            if (desc->gutter != nullptr)
                desc->gutter(desc->gutterContext, desc->tree, prLayoutTreeFrameElement(desc->tree, box, 0, cell), line);
            holder = prLayoutTreeFrameElement(desc->tree, box, 0, desc->rowStyle);
        } else {
            box = prLayoutTreeFrameElement(desc->tree, sheet, rowKey(v, RowPart_Row, line), desc->rowStyle);
            holder = box;
        }
        uint32_t rowIndex = v->rows.count;
        uint32_t pieceStart = v->pieces.count;
        while (run + 1 < runCount && runs[run + 1].start <= start)
            ++run;
        for (uint32_t l = 0; l < v->layerCount; ++l) {
            while (layerSpan[l] < v->layerEnds[l] && v->spans.data[layerSpan[l]].end <= start)
                ++layerSpan[l];
        }
        bool plain = runCount == 0 || (runs[run].style == 0 && (run + 1 == runCount || runs[run + 1].start >= end));
        if (plain || start == end) {
            PrStringView all{v->bytes.data, v->bytes.count};
            v->pieces.push(RowPiece{prLayoutTreeFrameText(desc->tree, holder, 0, all), 0, rowIndex});
        } else {
            // Each run in the line is an inline box with its text in it, so that shaping continues across them.
            for (uint32_t r = run; r < runCount && runs[r].start < end; ++r) {
                uint32_t from = runs[r].start > start ? runs[r].start - start : 0;
                uint32_t to = r + 1 < runCount && runs[r + 1].start < end ? runs[r + 1].start - start : end - start;
                PrTextEditStyle style = runs[r].style < desc->styleCount ? runs[r].style : 0;
                PrLayoutNode runBox = prLayoutTreeFrameElement(desc->tree, holder, 0, desc->styles[style]);
                PrStringView part{v->bytes.data + from, to - from};
                v->pieces.push(RowPiece{prLayoutTreeFrameText(desc->tree, runBox, 0, part), from, rowIndex});
            }
        }
        Row row{line, box, pieceStart, v->pieces.count - pieceStart};
        v->rows.push(row);
        // A highlight is of one text node, so a selection has one in each piece that it reaches.
        for (uint32_t p = 0; p < row.pieceCount; ++p) {
            const RowPiece& piece = v->pieces.data[pieceStart + p];
            uint32_t pieceFrom = start + piece.start;
            uint32_t pieceTo = p + 1 < row.pieceCount ? start + v->pieces.data[pieceStart + p + 1].start : end;
            v->highlights.clear();
            // The spans paint first, layer by layer, so that the selection paints over them.
            for (uint32_t l = 0; l < v->layerCount; ++l) {
                for (uint32_t k = layerSpan[l]; k < v->layerEnds[l] && v->spans.data[k].start < pieceTo; ++k) {
                    const PrTextViewSpan& painted = v->spans.data[k];
                    uint32_t from = painted.start > pieceFrom ? painted.start : pieceFrom;
                    uint32_t to = painted.end < pieceTo ? painted.end : pieceTo;
                    if (from < to) {
                        v->highlights.push(PrLayoutHighlight{
                            .start = from - pieceFrom,
                            .end = to - pieceFrom,
                            .color = painted.color,
                            .background = painted.background,
                            .underline = painted.underline,
                            .underlineStyle = painted.underlineStyle,
                        });
                    }
                }
            }
            for (uint32_t i = 0; i < selectionCount; ++i) {
                uint32_t low = prTextEditSelectionStart(selections[i]);
                uint32_t high = prTextEditSelectionEnd(selections[i]);
                uint32_t from = low > pieceFrom ? low : pieceFrom;
                uint32_t to = high < pieceTo ? high : pieceTo;
                if (from < to) {
                    v->highlights.push(PrLayoutHighlight{
                        .start = from - pieceFrom,
                        .end = to - pieceFrom,
                        .background = desc->selection,
                    });
                }
            }
            if (v->highlights.count != 0)
                prLayoutTreeFrameSetHighlights(desc->tree, piece.node, v->highlights.data, v->highlights.count);
        }
        for (uint32_t i = 0; desc->showCaret && i < selectionCount; ++i) {
            const PrTextEditSelection& s = selections[i];
            if (s.anchor != s.caret || s.caret < start || s.caret > end)
                continue;
            const RowPiece& piece = pieceAt(v, row, s.caret - start, s.affinity);
            v->carets.push(PrLayoutCaret{
                .position = {piece.node, s.caret - start - piece.start},
                .affinity = s.affinity,
                .color = desc->caret,
            });
        }
    }
    float trailing = last == lines && v->gaps.count != 0 ? gapBefore(v, lines) : 0;
    if (trailing > 0)
        prLayoutTreeFrameElement(desc->tree, sheet, rowKey(v, RowPart_Gap, lines), gapStyle(v, trailing));
    if (v->carets.count != 0) {
        prLayoutTreeFrameSetCarets(desc->tree, v->carets.data, v->carets.count);
        prLayoutTreeSetCaretsHidden(desc->tree, desc->caretHidden);
    }
    v->rowsVersion = prTextEditBufferVersion(v->buffer);
    placeScroll(v);
}

bool prTextViewLaidOut(PrTextView* view) {
    PrTextView* v = view;
    if (!v->framed)
        return false;
    bool corrected = false;
    float wrappedLineHeight = 0;    // from a row of several visual lines, if no row of one has given the line height
    for (uint32_t i = 0; i < v->rows.count; ++i) {
        const Row& row = v->rows.data[i];
        PrRect box = prLayoutRectToPx(prLayoutNodeBounds(v->frame.tree, row.box, PrLayoutCoordinates_Layout));
        float height = blockSizeOf(v, box);
        if (height <= 0)
            continue;
        float gap = v->gaps.count != 0 ? gapOf(v, row.line) : 0;
        if (!v->measured && row.pieceCount == 1) {
            // A row of one visual line gives the line height, and the advance if there is no estimator. Every estimate
            // takes them.
            uint32_t bytes = lineBytes(v->buffer, row.line);
            PrLayoutRect part{};
            uint32_t boxes = 0;
            if (bytes != 0) {
                boxes = prLayoutNodeTextRangeBoxes(v->frame.tree, v->pieces.data[row.pieceStart].node, 0, bytes,
                    PrLayoutCoordinates_Layout, &part, 1);
            }
            float along = inlineSizeOf(v, prLayoutRectToPx(part));
            if (boxes == 1 && along > 0 && along < textInline(v)) {
                if (v->estimator == nullptr)
                    v->advance = along / float(bytes);
                v->lineHeight = height;
                v->measured = true;
                estimateAll(v);
                corrected = true;
            } else if (boxes > 1 && wrappedLineHeight == 0) {
                wrappedLineHeight = height / float(boxes);
            }
        }
        float was = v->heights.heightOf(row.line);
        if (absF32(was - (height + gap)) > kHeightTolerance)
            corrected = true;
        v->heights.set(row.line, height + gap, true);
    }
    // A window in which every row wraps still says how tall a line is, so that the estimates do not stay at the first
    // guess.
    if (!v->measured && wrappedLineHeight > 0 && absF32(wrappedLineHeight - v->lineHeight) > kHeightTolerance) {
        v->lineHeight = wrappedLineHeight;
        estimateAll(v);
        corrected = true;
    }
    double limit = scrollLimit(v);
    if (scrollTop(v) > limit)
        setScroll(v, limit);
    clampInline(v);
    placeScroll(v);
    if (v->reveal) {
        v->reveal = false;
        PrRect caret{};
        if (prTextViewCaretRect(v, &caret)) {
            PrRect port = prLayoutRectToPx(prLayoutNodeBounds(v->frame.tree, v->frame.parent,
                PrLayoutCoordinates_Viewport));
            // The caret against the port along each axis, in physical coordinates.
            float blockLow = v->vertical ? caret.left : caret.top;
            float blockHigh = v->vertical ? caret.right : caret.bottom;
            float portBlockLow = v->vertical ? port.left : port.top;
            float portBlockHigh = v->vertical ? port.right : port.bottom;
            double shift = 0;
            if (blockLow < portBlockLow)
                shift = double(blockLow - portBlockLow);
            else if (blockHigh > portBlockHigh)
                shift = double(blockHigh - portBlockHigh);
            if (shift != 0) {
                setScroll(v, scrollTop(v) + (v->blockReversed ? -shift : shift));
                corrected = true;
            }
            if (!v->wrap) {
                float inlineLow = v->vertical ? caret.top : caret.left;
                float inlineHigh = inlineLow + 1;
                float portInlineLow = v->vertical ? port.top : port.left;
                float portInlineHigh = v->vertical ? port.bottom : port.right;
                if (inlineLow < portInlineLow)
                    scrollInlineBy(v, double(inlineLow - portInlineLow));
                else if (inlineHigh > portInlineHigh)
                    scrollInlineBy(v, double(inlineHigh - portInlineHigh));
            }
            placeScroll(v);
        } else {
            // The caret's row is not in the window yet, which the next frame's estimate brings about.
            v->reveal = true;
            corrected = true;
        }
    }
    bool again = corrected || v->changedView;
    v->changedView = false;
    return again;
}

bool prTextViewRefine(PrTextView* view, double milliseconds) {
    PrTextView* v = view;
    if (!v->refining)
        return false;
    followEdits(v);
    if (!v->refining)
        return false;
    double end = prTimeMonotonicMilliseconds() + milliseconds;
    uint32_t lines = prTextEditBufferLineCount(v->buffer);
    bool changed = false;
    uint32_t line = v->refineLine;
    while (line < lines) {
        uint32_t batchEnd = lines - line < kRefineBatch ? lines : line + kRefineBatch;
        for (; line < batchEnd; ++line) {
            if (v->heights.isExact(line))
                continue;
            float height = refinedHeight(v, line);
            if (absF32(v->heights.heightOf(line) - height) > kHeightTolerance) {
                v->heights.set(line, height, false);
                changed = true;
            }
        }
        if (prTimeMonotonicMilliseconds() >= end)
            break;
    }
    v->refineLine = line;
    v->refining = line < lines;
    return v->refining || changed;
}

PrTextViewInfo prTextViewInfo(const PrTextView* view) {
    PrTextView* v = const_cast<PrTextView*>(view);
    return PrTextViewInfo{
        .offset = scrollTop(v),
        .inlineOffset = v->wrap ? 0 : v->scrollInline,
        .documentSize = v->heights.total,
        .portSize = portBlock(v),
        .lineCount = prTextEditBufferLineCount(v->buffer),
        .rowCount = v->rows.count,
        .caret = prTextEditBufferPointFromOffset(v->buffer, prTextEditViewPrimary(v->edit).caret),
    };
}

void prTextViewScrollTo(PrTextView* view, double offset) {
    followEdits(view);
    setScroll(view, offset);
    view->changedView = true;
}

void prTextViewScrollBy(PrTextView* view, PrVec2 delta) {
    PrTextView* v = view;
    followEdits(v);
    float block = v->vertical ? delta.x : delta.y;
    block = v->blockReversed ? -block : block;
    // Lines that wrap have nowhere to scroll along, so a wheel that turns only one way still moves through the
    // document.
    if (v->vertical && v->wrap)
        block += delta.y;
    setScroll(v, scrollTop(v) + double(block));
    if (!v->wrap)
        scrollInlineBy(v, double(v->vertical ? delta.y : delta.x));
    v->changedView = true;
}

void prTextViewScrollInlineTo(PrTextView* view, double offset) {
    followEdits(view);
    if (view->wrap)
        return;
    view->scrollInline = 0;
    scrollInlineBy(view, offset);
    view->changedView = true;
}

void prTextViewReveal(PrTextView* view) {
    view->reveal = true;
    view->changedView = true;
}

bool prTextViewRun(PrTextView* view, PrTextEditCommand command, PrTextEditSelectionMove kind) {
    PrTextView* v = view;
    v->hasLineText = false;
    followEdits(v);
    switch (command) {
        case PrTextEditCommand_DeleteBackward:
        case PrTextEditCommand_DeleteForward:
        case PrTextEditCommand_DeleteWordBefore:
        case PrTextEditCommand_DeleteWordAfter: eraseSelections(v, command); return true;
        case PrTextEditCommand_InsertLineBreak: prTextViewInsert(v, "\n", PrTextEditMerge_None); return true;
        case PrTextEditCommand_SelectAll:
            setPrimary(v, PrTextEditSelection{.anchor = 0, .caret = prTextEditBufferSize(v->buffer)});
            return true;
        default: break;
    }
    PrTextEditGeometry g = geometryOf(v);
    uint32_t count = prTextEditViewSelectionCount(v->edit);
    v->scratch.clear();
    for (uint32_t i = 0; i < count; ++i) {
        PrTextEditSelection s = prTextEditViewSelections(v->edit)[i];
        PrTextEditMotion motion = prTextEditMotionFromCommand(&g, positionOf(v, s.caret, s.affinity), command,
            v->frame.arrowMotion);
        if (motion == PrTextEditMotion_None)
            return false;
        v->scratch.push(movedSelection(v, s, motion, kind));
    }
    setSelections(v, v->scratch.data, v->scratch.count);
    return true;
}

void prTextViewRunPage(PrTextView* view, PrTextViewPage page, PrTextEditSelectionMove kind) {
    PrTextView* v = view;
    v->hasLineText = false;
    followEdits(v);
    double distance = double(portBlock(v)) - double(v->lineHeight);
    distance = page == PrTextViewPage_Up ? -distance : distance;
    uint32_t count = prTextEditViewSelectionCount(v->edit);
    v->scratch.clear();
    for (uint32_t i = 0; i < count; ++i) {
        PrTextEditSelection s = prTextEditViewSelections(v->edit)[i];
        PrTextEditPoint point = prTextEditBufferPointFromOffset(v->buffer, s.caret);
        uint32_t line = v->heights.lineAt(v->heights.topOf(point.line) + distance);
        if (line == point.line && distance > 0 && line + 1 < prTextEditBufferLineCount(v->buffer))
            line += 1;
        if (line == point.line && distance < 0 && line > 0)
            line -= 1;
        // The goal stands as an estimate on a line that may be outside the window, and the rows settle what shows.
        float goal = s.hasGoal ? s.goal : float(point.column) * v->advance;
        PrTextEditLine answered{};
        uint32_t to = prTextEditBufferLineStart(v->buffer, line);
        if (estimatedLine(v, line, &answered)) {
            float nearest = kInfinityF32;
            for (uint32_t k = 0; k < answered.stopCount; ++k) {
                float away = absF32(answered.stops[k].along - goal);
                if (away < nearest) {
                    nearest = away;
                    to = prTextEditBufferLineStart(v->buffer, line) + answered.stops[k].position.offset;
                }
            }
        }
        v->scratch.push(PrTextEditSelection{
            .anchor = kind == PrTextEditSelectionMove_Extend ? s.anchor : to,
            .caret = to,
            .goal = goal,
            .hasGoal = true,
            .affinity = PrTextEditAffinity_Downstream,
        });
    }
    setScroll(v, scrollTop(v) + distance);
    setSelections(v, v->scratch.data, v->scratch.count);
}

void prTextViewInsert(PrTextView* view, PrStringView text, PrTextEditMerge merge) {
    prTextEditViewInsert(view->edit, text, PrTextEditOrigin_Local, merge);
    view->reveal = true;
    view->changedView = true;
}

void prTextViewEraseSelection(PrTextView* view) {
    eraseSelections(view, PrTextEditCommand_None);
}

PrString prTextViewSelectedText(const PrTextView* view) {
    PrTextView* v = const_cast<PrTextView*>(view);
    selectionsInOrder(v, v->scratch);
    v->bytes.clear();
    bool any = false;
    for (uint32_t i = 0; i < v->scratch.count; ++i) {
        const PrTextEditSelection& s = v->scratch.data[i];
        uint32_t low = prTextEditSelectionStart(s);
        uint32_t high = prTextEditSelectionEnd(s);
        if (low == high)
            continue;
        if (any)
            v->bytes.push('\n');
        any = true;
        uint32_t at = v->bytes.count;
        v->bytes.resize(at + (high - low));
        prTextEditBufferCopy(v->buffer, low, high, v->bytes.data + at);
    }
    return prStringCopy(PrStringView{v->bytes.data, v->bytes.count});
}

void prTextViewAddCaretOnLine(PrTextView* view, PrTextEditLineStep step) {
    PrTextView* v = view;
    if (step == PrTextEditLineStep_Same)
        return;
    v->hasLineText = false;
    followEdits(v);
    PrTextEditSelection primary = prTextEditViewPrimary(v->edit);
    PrTextEditMotion motion =
        step == PrTextEditLineStep_Next ? PrTextEditMotion_LineNext : PrTextEditMotion_LinePrevious;
    PrTextEditSelection caret{
        .anchor = primary.caret,
        .caret = primary.caret,
        .goal = primary.goal,
        .hasGoal = primary.hasGoal,
        .affinity = primary.affinity,
    };
    caret = movedSelection(v, caret, motion, PrTextEditSelectionMove_Collapse);
    if (caret.caret == primary.caret)
        return;
    uint32_t count = prTextEditViewSelectionCount(v->edit);
    v->scratch.clear();
    v->scratch.append(prTextEditViewSelections(v->edit), count);
    v->scratch.push(caret);
    setSelections(v, v->scratch.data, v->scratch.count);
}

void prTextViewAddCaret(PrTextView* view, uint32_t offset, PrTextEditAffinity affinity) {
    PrTextView* v = view;
    uint32_t count = prTextEditViewSelectionCount(v->edit);
    v->scratch.clear();
    v->scratch.append(prTextEditViewSelections(v->edit), count);
    v->scratch.push(PrTextEditSelection{.anchor = offset, .caret = offset, .affinity = affinity});
    setSelections(v, v->scratch.data, v->scratch.count);
}

void prTextViewKeepPrimary(PrTextView* view) {
    setPrimary(view, prTextEditViewPrimary(view->edit));
}

bool prTextViewOffsetFromPoint(PrTextView* view, PrVec2 point, uint32_t* offset, PrTextEditAffinity* affinity) {
    PrTextView* v = view;
    if (!v->framed)
        return false;
    if (v->frame.gutter != nullptr) {
        // A point over the gutter stands for the start of its line's text, past the gutter.
        PrRect port = prLayoutRectToPx(prLayoutNodeBounds(v->frame.tree, v->frame.parent,
            PrLayoutCoordinates_Viewport));
        float past = v->frame.gutterSize + 1;
        switch (inlineStartSide(v)) {
            case PrRectSide_Left: point.x = point.x > port.left + past ? point.x : port.left + past; break;
            case PrRectSide_Right: point.x = point.x < port.right - past ? point.x : port.right - past; break;
            case PrRectSide_Top: point.y = point.y > port.top + past ? point.y : port.top + past; break;
            default: point.y = point.y < port.bottom - past ? point.y : port.bottom - past; break;
        }
    }
    PrLayoutVec at = prLayoutVecFromPx(point);
    PrLayoutTextPosition position{};
    if (!prLayoutTextPositionFromPoint(v->frame.tree, v->frame.parent, at, &position))
        return false;
    uint32_t within = 0;
    PrLayoutTextAffinity side = 0;
    if (!prLayoutTextOffsetNear(v->frame.tree, position.node, at, &within, &side))
        return false;
    const RowPiece* piece = pieceOfNode(v, position.node);
    if (piece == nullptr)
        return false;
    uint32_t line = v->rows.data[piece->row].line;
    uint32_t bytes = lineBytes(v->buffer, line);
    within += piece->start;
    within = within < bytes ? within : bytes;
    *offset = prTextEditBufferLineStart(v->buffer, line) + within;
    *affinity = side;
    return true;
}

void prTextViewUnitAround(PrTextView* view, uint32_t offset, PrTextEditUnit unit, uint32_t* low, uint32_t* high) {
    PrTextEditPoint point = prTextEditBufferPointFromOffset(view->buffer, offset);
    uint32_t start = prTextEditBufferLineStart(view->buffer, point.line);
    if (unit == PrTextEditUnit_All) {
        *low = 0;
        *high = prTextEditBufferSize(view->buffer);
        return;
    }
    view->hasLineText = false;
    uint32_t first = 0;
    uint32_t last = 0;
    prTextEditUnitAround(lineTextOf(view, point.line), point.column, unit, &first, &last);
    *low = start + first;
    *high = start + last;
}

bool prTextViewCaretRect(PrTextView* view, PrRect* out) {
    PrTextView* v = view;
    PrTextEditSelection s = prTextEditViewPrimary(v->edit);
    PrTextEditPoint point = prTextEditBufferPointFromOffset(v->buffer, s.caret);
    if (v->rows.count == 0)
        return false;
    uint32_t first = v->rows.data[0].line;
    if (point.line < first || point.line - first >= v->rows.count)
        return false;
    const RowPiece& piece = pieceAt(v, v->rows.data[point.line - first], point.column, s.affinity);
    PrLayoutRect rect{};
    if (!prLayoutNodeTextCaret(v->frame.tree, piece.node, point.column - piece.start, s.affinity,
            PrLayoutCoordinates_Viewport, &rect))
        return false;
    *out = prLayoutRectToPx(rect);
    return true;
}

void prTextViewSetGaps(PrTextView* view, const PrTextViewGap* PR_NULLABLE gaps, uint32_t count) {
    PrTextView* v = view;
    followEdits(v);
    uint32_t lines = prTextEditBufferLineCount(v->buffer);
    // A gap's line holds its space in the height index, and the last line holds the gap after it.
    auto adjust = [&](const PrTextViewGap& gap, float sign) {
        if (gap.line > lines)
            return;
        uint32_t line = gap.line < lines ? gap.line : lines - 1;
        v->heights.set(line, v->heights.heightOf(line) + sign * gap.size, v->heights.isExact(line));
    };
    for (uint32_t i = 0; i < v->gaps.count; ++i)
        adjust(v->gaps.data[i], -1);
    v->gaps.clear();
    if (count != 0)
        v->gaps.append(gaps, count);
    for (uint32_t i = 0; i < v->gaps.count; ++i)
        adjust(v->gaps.data[i], 1);
    // The scroll position is a line and a distance into it, which hold.
    v->changedView = true;
}
