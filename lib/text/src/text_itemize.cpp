// Itemization.
//
// Scripts are resolved first, then the orientation of each cluster from its script, then the levels, because upright
// text is strong left to right, and the runs last. A paragraph ends at a Break piece and after each line feed, for
// scripts and levels alike. Levels are resolved over the characters of each paragraph, with the controls of the
// elements that are open across it. Those controls are closed at the end of the paragraph and opened again, outermost
// first, at the start of the next paragraph.

#include "pr/text_itemize.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/ascii.h"
#include "pr/locale_common.h"
#include "pr/locale_full.h"
#include "pr/mem.h"
#include "pr/unicode.h"
#include "pr/unicode_bidi.h"
#include "pr/unicode_grapheme.h"
#include "pr/utf8.h"

namespace pr_impl {
namespace {

constexpr uint32_t kNone = 0xffffffffu;

struct TextItems {
    Array<uint8_t> levels;
    Array<PrTextParagraph> paragraphs;
    Array<uint8_t> marks;
    Array<PrScript> scripts;
    Array<bool> upright;
    Array<PrTextItemRun> runs;
    bool leveled;

    // Scratch.
    Array<uint32_t> brackets;           // pairs: the key of a bracket and the byte at which it opened
    Array<PrUnicodeBidiClass> classes;
    Array<uint32_t> codePoints;
    Array<uint32_t> bytes;              // for each class, the byte of its character, or kNone for a control
    Array<uint32_t> markOf;             // for each class, the piece that marks its control, or kNone
    Array<uint8_t> resolved;
    Array<uint32_t> open;               // the Open pieces whose controls are open
    Array<uint32_t> faces;              // by byte
};

// ---------------------------------------------------------------------------------------------------------------------
// Clusters and scripts

// Returns the end of the cluster that starts at `at` of `text`. An ASCII character followed by another ASCII character
// or by the end is a cluster of its own, one byte long, which is the case for most text.
uint32_t clusterEnd(PrStringView text, uint32_t at) {
    uint8_t byte = uint8_t(text.data[at]);
    if (byte < 0x80 && byte != '\r' && (at + 1 == text.size || uint8_t(text.data[at + 1]) < 0x80))
        return at + 1;
    return uint32_t(prUnicodeGraphemeNext(text, at));
}

// Returns true if `script` is a script of its own: not Common, Inherited or Unknown.
bool isRealScript(PrScript script) {
    return script != PrScript_Zyyy && script != PrScript_Zinh && script != PrScript_Zzzz;
}

// Returns the script of a cluster: the script of its first code point that has a script of its own, or Common if none
// has.
PrScript clusterScript(PrStringView cluster) {
    for (size_t at = 0; at < cluster.size;) {
        uint32_t codePoint = 0;
        at += prUtf8Decode(cluster, at, &codePoint);
        PrScript script = prUnicodeScript(codePoint);
        if (isRealScript(script))
            return script;
    }
    return PrScript_Zyyy;
}

// Resolves the scripts of the bytes [start, end), the text of one paragraph between Break pieces, into `scripts`.
// Common is Zyyy until it resolves, and Zzzz if its paragraph has no script.
void resolveScripts(TextItems& t, PrStringView whole, uint32_t start, uint32_t end) {
    PrStringView text{whole.data, end};
    PrScript* scripts = t.scripts.data;
    t.brackets.clear();
    PrScript current = PrScript_Zzzz;         // the script of the text before, in the paragraph
    uint32_t pending = start;                 // the first byte of the leading Common text of the paragraph
    auto endParagraph = [&](uint32_t at) {
        for (uint32_t b = pending; b < at; ++b) {
            if (scripts[b] == PrScript_Zyyy)
                scripts[b] = PrScript_Zzzz;
        }
        current = PrScript_Zzzz;
        t.brackets.clear();
    };
    for (uint32_t at = start; at < end;) {
        uint32_t next = clusterEnd(text, at);
        PrStringView cluster{whole.data + at, next - at};
        PrScript script;
        if (cluster.size == 1)
            script = (prAsciiIsLetter(cluster.data[0]) ? PrScript_Latn : PrScript_Zyyy);
        else
            script = clusterScript(cluster);
        if (cluster.data[0] == '\n') {
            endParagraph(at);
            pending = next;
            script = PrScript_Zzzz;
        } else if (isRealScript(script)) {
            // The Common text before the first script of the paragraph takes that script.
            if (current == PrScript_Zzzz) {
                for (uint32_t b = pending; b < at; ++b) {
                    if (scripts[b] == PrScript_Zyyy)
                        scripts[b] = script;
                }
            }
            current = script;
        } else {
            uint32_t codePoint = 0;
            uint32_t key = 0;
            PrUnicodeBidiBracket bracket = PrUnicodeBidiBracket_None;
            if (prUtf8Decode(cluster, 0, &codePoint) == cluster.size)
                bracket = prUnicodeBidiBracket(codePoint, &key);
            script = current == PrScript_Zzzz ? PrScript_Zyyy : current;
            if (bracket == PrUnicodeBidiBracket_Open) {
                t.brackets.push(key);
                t.brackets.push(at);
            } else if (bracket == PrUnicodeBidiBracket_Close) {
                // The bracket that it closes, and the brackets opened inside that one, are closed.
                for (uint32_t i = t.brackets.count; i >= 2; i -= 2) {
                    if (t.brackets.data[i - 2] != key)
                        continue;
                    script = scripts[t.brackets.data[i - 1]];
                    t.brackets.count = i - 2;
                    break;
                }
            }
        }
        for (uint32_t b = at; b < next; ++b)
            scripts[b] = script;
        at = next;
    }
    endParagraph(end);
}

// Sets the orientation of each byte of the Text piece from the first code point and the resolved script of its cluster.
void orient(TextItems& t, PrStringView whole, const PrTextPiece& piece) {
    PrStringView text{whole.data, piece.end};
    for (uint32_t at = piece.start; at < piece.end;) {
        uint32_t next = clusterEnd(text, at);
        uint32_t codePoint = 0;
        prUtf8Decode(text, at, &codePoint);
        bool upright = prTextIsUpright(piece.orientation, codePoint, t.scripts.data[at]);
        for (uint32_t b = at; b < next; ++b)
            t.upright.data[b] = upright;
        at = next;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Levels

// Returns the code point of a control, among which brackets are paired.
uint32_t controlCodePoint(PrUnicodeBidiClass c) {
    switch (c) {
        case PrUnicodeBidiClass_LRE: return 0x202A;
        case PrUnicodeBidiClass_RLE: return 0x202B;
        case PrUnicodeBidiClass_PDF: return 0x202C;
        case PrUnicodeBidiClass_LRO: return 0x202D;
        case PrUnicodeBidiClass_RLO: return 0x202E;
        case PrUnicodeBidiClass_LRI: return 0x2066;
        case PrUnicodeBidiClass_RLI: return 0x2067;
        case PrUnicodeBidiClass_FSI: return 0x2068;
        default: return 0x2069;
    }
}

// Returns true if the bytes [start, end) hold a character that may make a level odd: a strong right-to-left character
// that is not upright, or a control that embeds, overrides or isolates right to left. ASCII has none.
bool mayTurnRight(const TextItems& t, PrStringView whole, uint32_t start, uint32_t end) {
    PrStringView text{whole.data, end};
    for (uint32_t at = start; at < end;) {
        if (uint8_t(text.data[at]) < 0x80) {
            ++at;
            continue;
        }
        uint32_t codePoint = 0;
        uint32_t size = uint32_t(prUtf8Decode(text, at, &codePoint));
        PrUnicodeBidiClass c = prUnicodeBidiClass(codePoint);
        if (!t.upright.data[at] &&
                (c == PrUnicodeBidiClass_R || c == PrUnicodeBidiClass_AL || c == PrUnicodeBidiClass_RLE ||
                    c == PrUnicodeBidiClass_RLO || c == PrUnicodeBidiClass_RLI))
            return true;
        at += size;
    }
    return false;
}

// Returns true if some level may be odd, which only resolving the levels tells exactly. A level may be odd if the
// paragraphs of the container are right to left, or if some element or text may turn right to left. Otherwise, every
// level is even.
bool mayHaveOddLevels(const TextItems& t, const PrTextItemizeDesc& d) {
    bool overrides = d.bidi == PrTextBidi_BidiOverride || d.bidi == PrTextBidi_IsolateOverride;
    if (d.direction == PrTextDirection_Rtl && d.bidi != PrTextBidi_Plaintext)
        return true;
    for (uint32_t i = 0; i < d.pieceCount; ++i) {
        const PrTextPiece& piece = d.pieces[i];
        switch (piece.kind) {
            case PrTextPieceKind_Open:
                if (piece.bidi != PrTextBidi_Normal && piece.direction == PrTextDirection_Rtl)
                    return true;
                break;
            case PrTextPieceKind_Object:
                if ((piece.bidi == PrTextBidi_Embed || piece.bidi == PrTextBidi_BidiOverride) &&
                        piece.direction == PrTextDirection_Rtl)
                    return true;
                break;
            case PrTextPieceKind_Text:
                // A left-to-right override of the container sets every character left to right.
                if (!overrides && mayTurnRight(t, d.text, piece.start, piece.end))
                    return true;
                break;
            default: break;
        }
    }
    return false;
}

// The paragraphs being resolved: the classes, code points and bytes of the paragraph being gathered, and the byte at
// which it started.
struct Resolver {
    TextItems& t;
    const PrTextItemizeDesc& d;

    // The direction of each paragraph: the container's, or taken from its text under plaintext.
    PrUnicodeBidiDirection direction;

    // The override of the container around each paragraph, or PrUnicodeBidiClass_PDF if none.
    PrUnicodeBidiClass override;

    uint32_t start;

    // Adds a character of class `c` at byte `byte`, or a control if `byte` is kNone. If `piece` is not kNone, the
    // control is an isolate initiator or terminator that piece `piece` marks.
    void add(PrUnicodeBidiClass c, uint32_t codePoint, uint32_t byte, uint32_t piece = kNone) {
        t.classes.push(c);
        t.codePoints.push(codePoint);
        t.bytes.push(byte);
        t.markOf.push(piece);
    }
    void control(PrUnicodeBidiClass c, uint32_t piece = kNone) { add(c, controlCodePoint(c), kNone, piece); }

    // Adds the controls that the element of the Open piece `i` adds at its start, which mark the piece if `marks` is
    // set, or the controls that its Close piece `i` adds at its end.
    void open(uint32_t i, bool marks) {
        const PrTextPiece& piece = d.pieces[i];
        bool rtl = piece.direction == PrTextDirection_Rtl;
        uint32_t mark = marks ? i : kNone;
        switch (piece.bidi) {
            case PrTextBidi_Embed: control(rtl ? PrUnicodeBidiClass_RLE : PrUnicodeBidiClass_LRE); break;
            case PrTextBidi_Isolate: control(rtl ? PrUnicodeBidiClass_RLI : PrUnicodeBidiClass_LRI, mark); break;
            case PrTextBidi_BidiOverride: control(rtl ? PrUnicodeBidiClass_RLO : PrUnicodeBidiClass_LRO); break;
            case PrTextBidi_IsolateOverride:
                control(PrUnicodeBidiClass_FSI, mark);
                control(rtl ? PrUnicodeBidiClass_RLO : PrUnicodeBidiClass_LRO);
                break;
            case PrTextBidi_Plaintext: control(PrUnicodeBidiClass_FSI, mark); break;
            default: break;
        }
    }
    void close(uint32_t i) {
        switch (d.pieces[i].bidi) {
            case PrTextBidi_Embed:
            case PrTextBidi_BidiOverride: control(PrUnicodeBidiClass_PDF); break;
            case PrTextBidi_Isolate:
            case PrTextBidi_Plaintext: control(PrUnicodeBidiClass_PDI, i); break;
            case PrTextBidi_IsolateOverride:
                control(PrUnicodeBidiClass_PDF);
                control(PrUnicodeBidiClass_PDI, i);
                break;
            default: break;
        }
    }

    // Starts a paragraph at byte `at` with the override of the container, and the controls of the elements open across
    // its start, outermost first.
    void begin(uint32_t at) {
        start = at;
        t.classes.clear();
        t.codePoints.clear();
        t.bytes.clear();
        t.markOf.clear();
        if (override != PrUnicodeBidiClass_PDF)
            control(override);
        for (uint32_t i = 0; i < t.open.count; ++i)
            open(t.open.data[i], false);
    }

    // Resolves the paragraph gathered, which ends before byte `end`, and records it. Each byte takes the level of its
    // character. A byte whose character X9 removes takes the level before it, or the level of the paragraph at its
    // start.
    void end(uint32_t end) {
        if (end == start)
            return;
        uint32_t count = t.classes.count;
        t.resolved.resize(count);
        uint8_t level = prUnicodeBidiResolve(t.classes.data, t.codePoints.data, count, direction, t.resolved.data);
        uint8_t previous = level;
        uint32_t byte = start;
        for (uint32_t i = 0; i < count; ++i) {
            if (t.markOf.data[i] != kNone && t.resolved.data[i] != kPrUnicodeBidiRemoved)
                t.marks.data[t.markOf.data[i]] = t.resolved.data[i];
            if (t.bytes.data[i] == kNone)
                continue;
            uint8_t at = t.resolved.data[i] == kPrUnicodeBidiRemoved ? previous : t.resolved.data[i];
            uint32_t next = end;
            for (uint32_t j = i + 1; j < count; ++j) {
                if (t.bytes.data[j] != kNone) {
                    next = t.bytes.data[j];
                    break;
                }
            }
            for (; byte < next; ++byte)
                t.levels.data[byte] = at;
            previous = at;
        }
        t.paragraphs.push(PrTextParagraph{start, end, level});
    }

    // Ends the paragraph at a separator at byte `at`, and starts the next paragraph after it.
    void separator(uint32_t at, uint32_t codePoint, uint32_t size) {
        add(PrUnicodeBidiClass_B, codePoint, at);
        end(at + size);
        begin(at + size);
    }
};

// Resolves the levels into `t.levels`, `t.paragraphs` and `t.marks`, and returns false if every level is even.
bool resolveLevels(TextItems& t, const PrTextItemizeDesc& d) {
    t.levels.clear();
    t.paragraphs.clear();
    t.marks.clear();
    if (!mayHaveOddLevels(t, d))
        return false;
    t.levels.resize(uint32_t(d.text.size));
    prMemZero(t.levels.data, d.text.size);
    t.marks.resize(d.pieceCount);
    prMemFill(t.marks.data, kPrTextLevelNone, d.pieceCount);
    bool overrides = d.bidi == PrTextBidi_BidiOverride || d.bidi == PrTextBidi_IsolateOverride;
    bool rightToLeft = d.direction == PrTextDirection_Rtl;
    PrUnicodeBidiDirection direction = PrUnicodeBidiDirection_LeftToRight;
    if (d.bidi == PrTextBidi_Plaintext)
        direction = PrUnicodeBidiDirection_Auto;
    else if (rightToLeft)
        direction = PrUnicodeBidiDirection_RightToLeft;
    PrUnicodeBidiClass override = PrUnicodeBidiClass_LRO;
    if (!overrides)
        override = PrUnicodeBidiClass_PDF;
    else if (rightToLeft)
        override = PrUnicodeBidiClass_RLO;
    Resolver r{.t = t, .d = d, .direction = direction, .override = override, .start = 0};
    t.open.clear();
    r.begin(0);
    for (uint32_t i = 0; i < d.pieceCount; ++i) {
        const PrTextPiece& piece = d.pieces[i];
        switch (piece.kind) {
            case PrTextPieceKind_Text:
                for (uint32_t at = piece.start; at < piece.end;) {
                    uint32_t codePoint = 0;
                    uint32_t size = uint32_t(prUtf8Decode(d.text, at, &codePoint));
                    if (codePoint == '\n')
                        r.separator(at, codePoint, size);
                    else
                        r.add(t.upright.data[at] ? PrUnicodeBidiClass_L : prUnicodeBidiClass(codePoint), codePoint, at);
                    at += size;
                }
                break;
            case PrTextPieceKind_Open:
                if (piece.bidi != PrTextBidi_Normal) {
                    r.open(i, true);
                    t.open.push(i);
                }
                break;
            case PrTextPieceKind_Close:
                if (piece.bidi != PrTextBidi_Normal) {
                    PR_ASSERT(t.open.count != 0 && d.pieces[t.open.last()].bidi == piece.bidi);
                    r.close(i);
                    t.open.pop();
                }
                break;
            case PrTextPieceKind_Object: {
                bool strong = piece.bidi == PrTextBidi_Embed || piece.bidi == PrTextBidi_BidiOverride;
                PrUnicodeBidiClass c = PrUnicodeBidiClass_L;
                if (!strong)
                    c = PrUnicodeBidiClass_ON;
                else if (piece.direction == PrTextDirection_Rtl)
                    c = PrUnicodeBidiClass_R;
                r.add(c, 0xFFFC, piece.start);
                break;
            }
            case PrTextPieceKind_Break:
                if (piece.end > piece.start) {
                    uint32_t codePoint = 0;
                    uint32_t size = uint32_t(prUtf8Decode(d.text, piece.start, &codePoint));
                    r.separator(piece.start, codePoint, size);
                } else {
                    // A paragraph ends with no separator, and the elements open across it open again after it.
                    r.end(piece.start);
                    r.begin(piece.start);
                }
                break;
            default: break;
        }
    }
    r.end(uint32_t(d.text.size));
    // If every level is even, nothing is reordered.
    bool odd = false;
    for (uint32_t i = 0; i < t.paragraphs.count && !odd; ++i)
        odd = (t.paragraphs.data[i].level & 1) != 0;
    for (uint32_t b = 0; b < t.levels.count && !odd; ++b)
        odd = (t.levels.data[b] & 1) != 0;
    if (!odd) {
        t.levels.clear();
        t.paragraphs.clear();
        t.marks.clear();
    }
    return odd;
}

// ---------------------------------------------------------------------------------------------------------------------
// Runs

// The runs being made: the text of one style that is being gathered, and the runs whose context is open.
struct Runs {
    TextItems& t;
    const PrTextItemizeDesc& d;
    uint32_t spanStart;             // the gathered text of one style, with no boundary inside
    uint32_t spanEnd;
    uint32_t spanPiece;             // the first piece of the span
    bool spanOpen;
    uint32_t contextStart;          // the byte at which the context of the runs since `contextRuns` starts
    uint32_t contextRuns;

    bool unshaped(uint32_t b) const { return d.text.data[b] == '\n' || (d.unshaped != nullptr && d.unshaped[b]); }

    // Divides the gathered text into runs, by face, script, orientation, level and unshaped bytes.
    void flushSpan() {
        if (!spanOpen)
            return;
        spanOpen = false;
        PrTextFont* PR_NULLABLE font = d.pieces[spanPiece].font;
        uint32_t* face = t.faces.data;
        uint32_t ascii[128];
        PrScript asciiScript[128];
        for (uint32_t c = 0; c < 128; ++c)
            ascii[c] = kNone;
        PrStringView text{d.text.data, spanEnd};
        for (uint32_t at = spanStart; at < spanEnd;) {
            uint32_t next = clusterEnd(text, at);
            PrScript script = t.scripts.data[at];
            uint32_t chosen = kPrTextMatchNone;
            uint8_t byte = uint8_t(text.data[at]);
            if (font == nullptr) {
                chosen = kPrTextMatchNone;
            } else if (next == at + 1 && byte < 0x80) {
                // An ASCII character of one script takes one face throughout.
                if (ascii[byte] == kNone || asciiScript[byte] != script) {
                    ascii[byte] = prTextFontMatch(font, PrStringView{text.data + at, 1}, script);
                    asciiScript[byte] = script;
                }
                chosen = ascii[byte];
            } else {
                chosen = prTextFontMatch(font, PrStringView{text.data + at, next - at}, script);
            }
            for (uint32_t b = at; b < next; ++b)
                face[b] = chosen;
            at = next;
        }
        const PrScript* scripts = t.scripts.data;
        const bool* upright = t.upright.data;
        const uint8_t* levels = t.leveled ? t.levels.data : nullptr;
        auto levelAt = [&](uint32_t b) { return levels != nullptr ? levels[b] : uint8_t(0); };
        for (uint32_t at = spanStart; at < spanEnd;) {
            if (unshaped(at)) {
                ++at;
                continue;
            }
            uint32_t end = at + 1;
            while (end < spanEnd && !unshaped(end) && face[end] == face[at] && scripts[end] == scripts[at] &&
                    upright[end] == upright[at] && levelAt(end) == levelAt(at))
                ++end;
            t.runs.push(PrTextItemRun{
                .start = at,
                .end = end,
                .contextStart = contextStart,
                .contextEnd = 0,
                .piece = spanPiece,
                .face = face[at],
                .script = scripts[at],
                .level = levelAt(at),
                .upright = upright[at],
            });
            at = end;
        }
    }

    // Ends the context of the runs made since the context started, at `end`, and starts the next context at `next`.
    void closeContext(uint32_t end, uint32_t next) {
        flushSpan();
        for (uint32_t r = contextRuns; r < t.runs.count; ++r)
            t.runs.data[r].contextEnd = end;
        contextRuns = t.runs.count;
        contextStart = next;
    }

    // Gathers the bytes [start, end) of Text piece `i` into the span of its style, or starts a new span.
    void gather(uint32_t start, uint32_t end, uint32_t i) {
        if (start == end)
            return;
        if (spanOpen && start == spanEnd && d.pieces[spanPiece].style == d.pieces[i].style) {
            spanEnd = end;
            return;
        }
        flushSpan();
        spanOpen = true;
        spanStart = start;
        spanEnd = end;
        spanPiece = i;
    }

    // Gathers Text piece `i`. Its line feeds are forced breaks, which end shaping and its context.
    void text(uint32_t i) {
        const PrTextPiece& piece = d.pieces[i];
        uint32_t from = piece.start;
        for (uint32_t b = piece.start; b < piece.end; ++b) {
            if (d.text.data[b] != '\n')
                continue;
            gather(from, b, i);
            closeContext(b, b + 1);
            from = b + 1;
        }
        gather(from, piece.end, i);
    }
};

} // namespace
} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrTextItems : TextItems {};

bool prTextIsUpright(PrTextOrientation orientation, uint32_t codePoint, PrScript script) {
    switch (orientation) {
        case PrTextOrientation_Horizontal:
        case PrTextOrientation_Sideways: return false;
        case PrTextOrientation_Upright:
            // A vertical-only script keeps its intrinsic orientation under Upright, the same as under Mixed.
            if (script != PrScript_Mong && script != PrScript_Phag)
                return true;
            break;
        default: break;
    }
    return prUnicodeVerticalOrientation(codePoint) != PrUnicodeVerticalOrientation_Rotated;
}

PrTextItems* prTextItemsCreate(void) {
    return allocZeroed<PrTextItems>();
}

void prTextItemsDestroy(PrTextItems* PR_NULLABLE items) {
    if (items == nullptr)
        return;
    items->levels.reset();
    items->paragraphs.reset();
    items->marks.reset();
    items->scripts.reset();
    items->upright.reset();
    items->runs.reset();
    items->brackets.reset();
    items->classes.reset();
    items->codePoints.reset();
    items->bytes.reset();
    items->markOf.reset();
    items->resolved.reset();
    items->open.reset();
    items->faces.reset();
    prMemoryFree(items);
}

void prTextItemsItemize(PrTextItems* items, const PrTextItemizeDesc* desc) {
    const PrTextItemizeDesc& d = *desc;
    uint32_t size = uint32_t(d.text.size);
    items->runs.clear();
    items->scripts.resize(size);
    items->upright.resize(size);
    items->faces.resize(size);
    prMemZero(items->upright.data, size * sizeof(bool));
    // Resolves scripts, by the paragraphs that Break pieces end, and then the orientation of each Text piece.
    uint32_t from = 0;
    for (uint32_t i = 0; i <= d.pieceCount; ++i) {
        if (i < d.pieceCount && d.pieces[i].kind != PrTextPieceKind_Break)
            continue;
        uint32_t to = i < d.pieceCount ? d.pieces[i].start : size;
        if (to > from)
            resolveScripts(*items, d.text, from, to);
        if (i < d.pieceCount) {
            for (uint32_t b = d.pieces[i].start; b < d.pieces[i].end; ++b)
                items->scripts.data[b] = PrScript_Zzzz;
            from = d.pieces[i].end;
        }
    }
    for (uint32_t i = 0; i < d.pieceCount; ++i) {
        if (d.pieces[i].kind == PrTextPieceKind_Text && d.pieces[i].orientation != PrTextOrientation_Horizontal)
            orient(*items, d.text, d.pieces[i]);
    }
    items->leveled = resolveLevels(*items, d);
    Runs runs{
        .t = *items,
        .d = d,
        .spanStart = 0,
        .spanEnd = 0,
        .spanPiece = 0,
        .spanOpen = false,
        .contextStart = 0,
        .contextRuns = 0,
    };
    for (uint32_t i = 0; i < d.pieceCount; ++i) {
        const PrTextPiece& piece = d.pieces[i];
        switch (piece.kind) {
            case PrTextPieceKind_Text:
                if (piece.end > piece.start)
                    runs.text(i);
                break;
            case PrTextPieceKind_Open:
            case PrTextPieceKind_Close:
                if (piece.breaksShaping)
                    runs.closeContext(piece.start, piece.start);
                break;
            case PrTextPieceKind_Anchor: break;
            default:
                // An object, a break and a boundary end shaping and its context.
                runs.closeContext(piece.start, piece.end);
                break;
        }
    }
    runs.closeContext(size, size);
}

const uint8_t* PR_NULLABLE prTextItemsLevels(const PrTextItems* items) {
    return items->leveled ? items->levels.data : nullptr;
}

uint32_t prTextItemsParagraphCount(const PrTextItems* items) {
    return items->leveled ? items->paragraphs.count : 0;
}

const PrTextParagraph* PR_NULLABLE prTextItemsParagraphs(const PrTextItems* items) {
    return items->leveled ? items->paragraphs.data : nullptr;
}

const uint8_t* PR_NULLABLE prTextItemsMarks(const PrTextItems* items) {
    return items->leveled ? items->marks.data : nullptr;
}

const PrScript* PR_NULLABLE prTextItemsScripts(const PrTextItems* items) {
    return items->scripts.data;
}

const bool* PR_NULLABLE prTextItemsUpright(const PrTextItems* items) {
    return items->upright.data;
}

uint32_t prTextItemsRunCount(const PrTextItems* items) {
    return items->runs.count;
}

const PrTextItemRun* PR_NULLABLE prTextItemsRuns(const PrTextItems* items) {
    return items->runs.data;
}
