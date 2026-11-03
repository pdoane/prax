// PostScript outlines.

#include "font/src/cff.h"

#include "pr/float.h"

namespace pr_impl {

namespace {

constexpr uint32_t kCff = 0x43464620;            // 'CFF '
constexpr uint32_t kCff2 = 0x43464632;           // 'CFF2'

// The most values that a charstring's stack holds, the deepest that its subroutines nest, and the size of its transient
// array.
constexpr uint32_t kMaxStack = 513;
constexpr uint32_t kMaxCallDepth = 10;
constexpr uint32_t kTransientSize = 32;

// The most regions that a CFF2 blend reads scalars for.
constexpr uint32_t kMaxRegions = 64;

// The operators of DICTs that the face reads.
enum {
    DictOp_Charset = 15,
    DictOp_CharStrings = 17,
    DictOp_Private = 18,
    DictOp_Subrs = 19,
    DictOp_Vsindex = 22,
    DictOp_Blend = 23,
    DictOp_Vstore = 24,
    DictOp_Ros = 0x0c1e,
    DictOp_FdArray = 0x0c24,
    DictOp_FdSelect = 0x0c25,
};
typedef uint16_t DictOp;

// The operators of charstrings.
enum {
    CharOp_Hstem = 1,
    CharOp_Vstem = 3,
    CharOp_Vmoveto = 4,
    CharOp_Rlineto = 5,
    CharOp_Hlineto = 6,
    CharOp_Vlineto = 7,
    CharOp_Rrcurveto = 8,
    CharOp_Callsubr = 10,
    CharOp_Return = 11,
    CharOp_Escape = 12,
    CharOp_Endchar = 14,
    CharOp_Vsindex = 15,
    CharOp_Blend = 16,
    CharOp_Hstemhm = 18,
    CharOp_Hintmask = 19,
    CharOp_Cntrmask = 20,
    CharOp_Rmoveto = 21,
    CharOp_Hmoveto = 22,
    CharOp_Vstemhm = 23,
    CharOp_Rcurveline = 24,
    CharOp_Rlinecurve = 25,
    CharOp_Vvcurveto = 26,
    CharOp_Hhcurveto = 27,
    CharOp_ShortInt = 28,
    CharOp_Callgsubr = 29,
    CharOp_Vhcurveto = 30,
    CharOp_Hvcurveto = 31,
    CharOp_Fixed = 255,
};
typedef uint8_t CharOp;

// The operators of charstrings that follow the escape.
enum {
    EscapeOp_And = 3,
    EscapeOp_Or = 4,
    EscapeOp_Not = 5,
    EscapeOp_Abs = 9,
    EscapeOp_Add = 10,
    EscapeOp_Sub = 11,
    EscapeOp_Div = 12,
    EscapeOp_Neg = 14,
    EscapeOp_Eq = 15,
    EscapeOp_Drop = 18,
    EscapeOp_Put = 20,
    EscapeOp_Get = 21,
    EscapeOp_Ifelse = 22,
    EscapeOp_Random = 23,
    EscapeOp_Mul = 24,
    EscapeOp_Sqrt = 26,
    EscapeOp_Dup = 27,
    EscapeOp_Exch = 28,
    EscapeOp_Index = 29,
    EscapeOp_Roll = 30,
    EscapeOp_Hflex = 34,
    EscapeOp_Flex = 35,
    EscapeOp_Hflex1 = 36,
    EscapeOp_Flex1 = 37,
};
typedef uint8_t EscapeOp;

// The SIDs of the glyphs that StandardEncoding gives each code, which seac names its components by.
constexpr uint8_t kStandardEncoding[256] = {
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
    17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
    33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48,
    49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64,
    65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79, 80,
    81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 96, 97, 98, 99, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110,
    0, 111, 112, 113, 114, 0, 115, 116, 117, 118, 119, 120, 121, 122, 0, 123,
    0, 124, 125, 126, 127, 128, 129, 130, 131, 0, 132, 133, 0, 134, 135, 136,
    137, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 138, 0, 139, 0, 0, 0, 0, 140, 141, 142, 143, 0, 0, 0, 0,
    0, 144, 0, 0, 0, 145, 0, 0, 146, 147, 148, 149, 0, 0, 0, 0,
};

// Opens the INDEX at `at` of the table. A CFF2 INDEX counts its items in 32 bits.
CffIndex indexAt(SfntTable table, uint32_t at, bool cff2) {
    CffIndex index{};
    index.table = table;
    index.count = cff2 ? table.uint32At(at) : table.uint16At(at);
    uint32_t header = cff2 ? 4 : 2;
    if (index.count == 0) {
        index.end = at + header;
        return index;
    }
    index.offsetSize = table.uint8At(at + header);
    index.offsets = at + header + 1;
    if (index.offsetSize < 1 || index.offsetSize > 4 || uint64_t(index.count) * index.offsetSize > table.size) {
        index.count = 0;
        index.end = at + header;
        return index;
    }
    index.data = index.offsets + (index.count + 1) * index.offsetSize - 1;
    auto offset = [&](uint32_t i) {
        uint32_t value = 0;
        for (uint32_t b = 0; b < index.offsetSize; ++b)
            value = value << 8 | table.uint8At(index.offsets + i * index.offsetSize + b);
        return value;
    };
    index.end = index.data + offset(index.count);
    return index;
}

// Returns the bytes of item `i` of the INDEX, or an empty table.
SfntTable indexItem(const CffIndex& index, uint32_t i) {
    if (i >= index.count)
        return SfntTable{nullptr, 0};
    auto offset = [&](uint32_t k) {
        uint32_t value = 0;
        for (uint32_t b = 0; b < index.offsetSize; ++b)
            value = value << 8 | index.table.uint8At(index.offsets + k * index.offsetSize + b);
        return value;
    };
    uint32_t start = index.data + offset(i);
    uint32_t end = index.data + offset(i + 1);
    if (end < start || end > index.table.size)
        return SfntTable{nullptr, 0};
    return SfntTable{index.table.data + start, end - start};
}

// A DICT's operator and the operands before it.
struct DictEntry {
    DictOp op;
    uint32_t count;
    double operands[48];
};

// Calls `visit` with each entry of the DICT. CFF2's blend leaves its operands on the stack, because the face reads no
// operand that a font blends.
template <typename Visit>
void readDict(SfntTable dict, Visit visit) {
    DictEntry entry{};
    uint32_t at = 0;
    while (at < dict.size) {
        uint8_t b0 = dict.uint8At(at);
        double value = 0;
        if (b0 <= 21 || b0 == DictOp_Vsindex || b0 == DictOp_Blend || b0 == DictOp_Vstore) {
            entry.op = b0;
            ++at;
            if (b0 == 12) {
                entry.op = DictOp(0x0c00 | dict.uint8At(at));
                ++at;
            }
            if (b0 == DictOp_Blend)
                continue;
            visit(entry);
            entry.count = 0;
            continue;
        }
        if (b0 >= 32 && b0 <= 246) {
            value = double(int32_t(b0) - 139);
            at += 1;
        } else if (b0 >= 247 && b0 <= 250) {
            value = double((int32_t(b0) - 247) * 256 + dict.uint8At(at + 1) + 108);
            at += 2;
        } else if (b0 >= 251 && b0 <= 254) {
            value = double(-(int32_t(b0) - 251) * 256 - dict.uint8At(at + 1) - 108);
            at += 2;
        } else if (b0 == 28) {
            value = double(dict.int16At(at + 1));
            at += 3;
        } else if (b0 == 29) {
            value = double(int32_t(dict.uint32At(at + 1)));
            at += 5;
        } else if (b0 == 30) {
            // A real number, as nibbles, which the face reads only to skip.
            ++at;
            while (at < dict.size) {
                uint8_t nibbles = dict.uint8At(at++);
                if ((nibbles & 0x0f) == 0x0f || (nibbles >> 4) == 0x0f)
                    break;
            }
        } else {
            ++at;
            continue;
        }
        if (entry.count < 48)
            entry.operands[entry.count++] = value;
    }
}

// Reads the private DICT at the offset and size, and the local subroutines that it names.
CffPrivate readPrivate(SfntTable table, uint32_t offset, uint32_t size, bool cff2) {
    CffPrivate p{};
    if (offset > table.size || size > table.size - offset)
        return p;
    SfntTable dict{table.data + offset, size};
    uint32_t subrs = 0;
    readDict(dict, [&](const DictEntry& e) {
        if (e.op == DictOp_Subrs && e.count >= 1)
            subrs = uint32_t(e.operands[e.count - 1]);
        else if (e.op == DictOp_Vsindex && e.count >= 1)
            p.vsindex = uint32_t(e.operands[e.count - 1]);
    });
    if (subrs != 0)
        p.subrs = indexAt(table, offset + subrs, cff2);
    return p;
}

// Returns the font dictionary of the glyph.
uint32_t fontDictOf(const CffFace& face, uint32_t glyph) {
    const SfntTable& s = face.fdSelect;
    if (s.size == 0)
        return 0;
    switch (s.uint8At(0)) {
        case 0:
            return s.uint8At(1 + glyph);
        case 3:
        case 4: {
            bool wide = s.uint8At(0) == 4;
            uint32_t ranges = wide ? s.uint32At(1) : s.uint16At(1);
            uint32_t at = wide ? 5 : 3;
            uint32_t size = wide ? 6 : 3;
            for (uint32_t i = 0; i < ranges; ++i) {
                uint32_t first = wide ? s.uint32At(at + i * size) : s.uint16At(at + i * size);
                uint32_t next = wide ? s.uint32At(at + (i + 1) * size) : s.uint16At(at + (i + 1) * size);
                if (glyph >= first && glyph < next)
                    return wide ? s.uint16At(at + i * size + 4) : s.uint8At(at + i * size + 2);
            }
            return 0;
        }
        default:
            return 0;
    }
}

// Returns the bias that subroutine numbers are offset by, which centers them on zero.
int32_t subrBias(uint32_t count) {
    return count < 1240 ? 107 : count < 33900 ? 1131 : 32768;
}

// Returns the glyph that the charset names `sid`, or 0 for none.
uint32_t glyphOfSid(const CffFace& face, uint32_t sid) {
    if (face.charset.size == 0)
        return sid < face.charStrings.count ? sid : 0;
    const SfntTable& c = face.charset;
    uint32_t glyphs = face.charStrings.count;
    switch (c.uint8At(0)) {
        case 0:
            for (uint32_t g = 1; g < glyphs; ++g) {
                if (c.uint16At(1 + (g - 1) * 2) == sid)
                    return g;
            }
            return 0;
        case 1:
        case 2: {
            bool wide = c.uint8At(0) == 2;
            uint32_t at = 1;
            for (uint32_t g = 1; g < glyphs && at + 3 <= c.size;) {
                uint32_t first = c.uint16At(at);
                uint32_t left = wide ? c.uint16At(at + 2) : c.uint8At(at + 2);
                at += wide ? 4 : 3;
                if (sid >= first && sid <= first + left)
                    return g + (sid - first);
                g += left + 1;
            }
            return 0;
        }
        default:
            return 0;
    }
}

// The interpretation of one glyph's charstrings.
struct Interpreter {
    const CffFace& face;
    SfntCoords coords;
    FontGlyphOutline* out;
    CffIndex localSubrs;
    uint32_t vsindex;
    float scalars[kMaxRegions];
    uint32_t regionCount;
    bool scalarsRead;

    double stack[kMaxStack];
    uint32_t count;
    double transient[kTransientSize];
    PrVec2 point;                   // the current point
    PrVec2 start;                   // the start of the open contour
    PrVec2 offset;                  // where a seac component is placed
    uint32_t stems;
    bool widthRead;
    bool open;                      // a contour has a point
    bool movePending;               // a moveto has set the start of a contour that has no point yet
    bool ended;
    uint32_t seed;                  // of the random operator
};

// Returns an interpreter of the glyph's charstring, which reads the subroutines and variation data of its font
// dictionary.
Interpreter interpreterFor(const CffFace& face, SfntCoords coords, FontGlyphOutline* out, uint32_t glyph) {
    uint32_t fd = fontDictOf(face, glyph);
    const CffPrivate* p = fd < face.privates.count ? &face.privates.data[fd] : nullptr;
    return Interpreter{
        .face = face,
        .coords = coords,
        .out = out,
        .localSubrs = p != nullptr ? p->subrs : CffIndex{},
        .vsindex = p != nullptr ? p->vsindex : 0,
    };
}

void closeContour(Interpreter& in) {
    if (!in.open)
        return;
    FontGlyphOutline& o = *in.out;
    uint32_t first = o.ends.count != 0 ? o.ends.last() : 0;
    // The closing line ends on the first point, which is not repeated.
    uint32_t last = o.points.count - 1;
    if (last > first && o.kinds.data[last] == OutlinePoint_On && o.points.data[last].x == o.points.data[first].x &&
            o.points.data[last].y == o.points.data[first].y) {
        o.points.pop();
        o.kinds.pop();
    }
    fontOutlineEndContour(o);
    in.open = false;
}

void moveTo(Interpreter& in, PrVec2 to) {
    closeContour(in);
    in.point = to;
    in.start = to;
    in.movePending = true;
}

// Starts the contour that a pending moveto set, at its first segment.
void beginSegment(Interpreter& in) {
    if (!in.movePending && in.open)
        return;
    if (!in.open) {
        fontOutlinePush(*in.out, PrVec2{in.start.x + in.offset.x, in.start.y + in.offset.y}, OutlinePoint_On);
        in.open = true;
    }
    in.movePending = false;
}

void lineTo(Interpreter& in, PrVec2 to) {
    if (to.x == in.point.x && to.y == in.point.y)
        return;
    beginSegment(in);
    fontOutlinePush(*in.out, PrVec2{to.x + in.offset.x, to.y + in.offset.y}, OutlinePoint_On);
    in.point = to;
}

void curveTo(Interpreter& in, PrVec2 c1, PrVec2 c2, PrVec2 to) {
    beginSegment(in);
    fontOutlinePush(*in.out, PrVec2{c1.x + in.offset.x, c1.y + in.offset.y}, OutlinePoint_Cubic);
    fontOutlinePush(*in.out, PrVec2{c2.x + in.offset.x, c2.y + in.offset.y}, OutlinePoint_Cubic);
    fontOutlinePush(*in.out, PrVec2{to.x + in.offset.x, to.y + in.offset.y}, OutlinePoint_On);
    in.point = to;
}

// Reads the region scalars of the glyph's variation data, once.
void readScalars(Interpreter& in) {
    if (in.scalarsRead)
        return;
    in.scalarsRead = true;
    if (in.face.variations.size != 0)
        in.regionCount =
            sfntVariationScalars(in.face.variations, uint16_t(in.vsindex), in.coords, in.scalars, kMaxRegions);
    else
        in.regionCount = 0;
    if (in.regionCount > kMaxRegions)
        in.regionCount = kMaxRegions;
}

// Drops the width from the stack of the first stack-clearing operator, if the stack holds one more operand than the
// operator takes: an odd count for the operators of pairs, or `expected` plus one.
void readWidth(Interpreter& in, bool pairs, uint32_t expected) {
    if (in.widthRead || in.face.cff2)
        return;
    in.widthRead = true;
    bool extra = pairs ? (in.count % 2) == 1 : in.count == expected + 1;
    if (extra && in.count > 0) {
        for (uint32_t i = 1; i < in.count; ++i)
            in.stack[i - 1] = in.stack[i];
        --in.count;
    }
}

bool run(Interpreter& in, SfntTable code, uint32_t depth);

// Draws a seac's components: the base at the origin, then the accent at (adx, ady).
void seac(Interpreter& in, double adx, double ady, uint32_t base, uint32_t accent) {
    if (in.face.cid || base > 255 || accent > 255)
        return;
    uint32_t baseGlyph = glyphOfSid(in.face, kStandardEncoding[base]);
    uint32_t accentGlyph = glyphOfSid(in.face, kStandardEncoding[accent]);
    if (baseGlyph == 0 || accentGlyph == 0)
        return;
    for (int32_t component = 0; component < 2; ++component) {
        uint32_t glyph = component == 0 ? baseGlyph : accentGlyph;
        Interpreter part = interpreterFor(in.face, in.coords, in.out, 0);
        part.offset = component == 0 ? PrVec2{0, 0} : PrVec2{float(adx), float(ady)};
        run(part, indexItem(in.face.charStrings, glyph), 0);
        closeContour(part);
    }
}

bool run(Interpreter& in, SfntTable code, uint32_t depth) {
    if (depth > kMaxCallDepth)
        return false;
    uint32_t at = 0;
    while (at < code.size && !in.ended) {
        uint8_t b0 = code.uint8At(at);
        if (b0 >= 32 || b0 == CharOp_ShortInt) {
            double value = 0;
            if (b0 == CharOp_ShortInt) {
                value = double(code.int16At(at + 1));
                at += 3;
            } else if (b0 <= 246) {
                value = double(int32_t(b0) - 139);
                at += 1;
            } else if (b0 <= 250) {
                value = double((int32_t(b0) - 247) * 256 + code.uint8At(at + 1) + 108);
                at += 2;
            } else if (b0 <= 254) {
                value = double(-(int32_t(b0) - 251) * 256 - code.uint8At(at + 1) - 108);
                at += 2;
            } else {
                value = double(int32_t(code.uint32At(at + 1))) / 65536;
                at += 5;
            }
            if (in.count < kMaxStack)
                in.stack[in.count++] = value;
            continue;
        }
        ++at;
        double* s = in.stack;
        uint32_t n = in.count;
        PrVec2& p = in.point;
        switch (b0) {
            case CharOp_Hstem:
            case CharOp_Vstem:
            case CharOp_Hstemhm:
            case CharOp_Vstemhm:
                readWidth(in, true, 0);
                in.stems += in.count / 2;
                in.count = 0;
                break;
            case CharOp_Hintmask:
            case CharOp_Cntrmask:
                readWidth(in, true, 0);
                in.stems += in.count / 2;
                in.count = 0;
                at += (in.stems + 7) / 8;
                break;
            case CharOp_Rmoveto:
                readWidth(in, false, 2);
                if (in.count >= 2)
                    moveTo(in, PrVec2{p.x + float(in.stack[in.count - 2]), p.y + float(in.stack[in.count - 1])});
                in.count = 0;
                break;
            case CharOp_Hmoveto:
                readWidth(in, false, 1);
                if (in.count >= 1)
                    moveTo(in, PrVec2{p.x + float(in.stack[in.count - 1]), p.y});
                in.count = 0;
                break;
            case CharOp_Vmoveto:
                readWidth(in, false, 1);
                if (in.count >= 1)
                    moveTo(in, PrVec2{p.x, p.y + float(in.stack[in.count - 1])});
                in.count = 0;
                break;
            case CharOp_Rlineto:
                for (uint32_t i = 0; i + 1 < n; i += 2)
                    lineTo(in, PrVec2{p.x + float(s[i]), p.y + float(s[i + 1])});
                in.count = 0;
                break;
            case CharOp_Hlineto:
            case CharOp_Vlineto: {
                bool horizontal = b0 == CharOp_Hlineto;
                for (uint32_t i = 0; i < n; ++i, horizontal = !horizontal)
                    lineTo(in, horizontal ? PrVec2{p.x + float(s[i]), p.y} : PrVec2{p.x, p.y + float(s[i])});
                in.count = 0;
                break;
            }
            case CharOp_Rrcurveto:
            case CharOp_Rcurveline: {
                uint32_t i = 0;
                uint32_t curves = b0 == CharOp_Rcurveline && n >= 2 ? n - 2 : n;
                for (; i + 5 < curves; i += 6) {
                    PrVec2 c1{p.x + float(s[i]), p.y + float(s[i + 1])};
                    PrVec2 c2{c1.x + float(s[i + 2]), c1.y + float(s[i + 3])};
                    curveTo(in, c1, c2, PrVec2{c2.x + float(s[i + 4]), c2.y + float(s[i + 5])});
                }
                if (b0 == CharOp_Rcurveline && i + 1 < n)
                    lineTo(in, PrVec2{p.x + float(s[i]), p.y + float(s[i + 1])});
                in.count = 0;
                break;
            }
            case CharOp_Rlinecurve: {
                uint32_t i = 0;
                for (; i + 7 < n; i += 2)
                    lineTo(in, PrVec2{p.x + float(s[i]), p.y + float(s[i + 1])});
                if (i + 5 < n) {
                    PrVec2 c1{p.x + float(s[i]), p.y + float(s[i + 1])};
                    PrVec2 c2{c1.x + float(s[i + 2]), c1.y + float(s[i + 3])};
                    curveTo(in, c1, c2, PrVec2{c2.x + float(s[i + 4]), c2.y + float(s[i + 5])});
                }
                in.count = 0;
                break;
            }
            case CharOp_Vvcurveto:
            case CharOp_Hhcurveto: {
                bool vertical = b0 == CharOp_Vvcurveto;
                uint32_t i = 0;
                float first = 0;
                if (n % 4 == 1) {
                    first = float(s[0]);
                    i = 1;
                }
                for (; i + 3 < n; i += 4) {
                    PrVec2 c1 = vertical ? PrVec2{p.x + first, p.y + float(s[i])} : PrVec2{p.x + float(s[i]), p.y + first};
                    PrVec2 c2{c1.x + float(s[i + 1]), c1.y + float(s[i + 2])};
                    PrVec2 to = vertical ? PrVec2{c2.x, c2.y + float(s[i + 3])} : PrVec2{c2.x + float(s[i + 3]), c2.y};
                    curveTo(in, c1, c2, to);
                    first = 0;
                }
                in.count = 0;
                break;
            }
            case CharOp_Hvcurveto:
            case CharOp_Vhcurveto: {
                bool horizontal = b0 == CharOp_Hvcurveto;
                uint32_t i = 0;
                for (; i + 3 < n; i += 4, horizontal = !horizontal) {
                    // The last curve takes a fifth operand, if there is one, along its other axis.
                    float extra = n - i == 5 ? float(s[i + 4]) : 0;
                    PrVec2 c1 = horizontal ? PrVec2{p.x + float(s[i]), p.y} : PrVec2{p.x, p.y + float(s[i])};
                    PrVec2 c2{c1.x + float(s[i + 1]), c1.y + float(s[i + 2])};
                    PrVec2 to;
                    if (horizontal)
                        to = PrVec2{c2.x + extra, c2.y + float(s[i + 3])};
                    else
                        to = PrVec2{c2.x + float(s[i + 3]), c2.y + extra};
                    curveTo(in, c1, c2, to);
                }
                in.count = 0;
                break;
            }
            case CharOp_Callsubr:
            case CharOp_Callgsubr: {
                if (n == 0)
                    return false;
                const CffIndex& subrs = b0 == CharOp_Callsubr ? in.localSubrs : in.face.globalSubrs;
                int32_t number = int32_t(s[n - 1]) + subrBias(subrs.count);
                --in.count;
                if (number < 0 || !run(in, indexItem(subrs, uint32_t(number)), depth + 1))
                    return false;
                break;
            }
            case CharOp_Return:
                return true;
            case CharOp_Endchar:
                // A width precedes no operands, or the four of a seac.
                if (!in.widthRead && !in.face.cff2 && (in.count == 1 || in.count == 5)) {
                    for (uint32_t i = 1; i < in.count; ++i)
                        s[i - 1] = s[i];
                    --in.count;
                }
                in.widthRead = true;
                if (in.count == 4 && !in.face.cff2) {
                    closeContour(in);
                    seac(in, s[0], s[1], uint32_t(s[2]), uint32_t(s[3]));
                }
                closeContour(in);
                in.ended = true;
                in.count = 0;
                return true;
            case CharOp_Vsindex:
                if (n >= 1 && !in.scalarsRead)
                    in.vsindex = uint32_t(s[n - 1]);
                in.count = 0;
                break;
            case CharOp_Blend: {
                if (n == 0)
                    return false;
                readScalars(in);
                uint32_t values = uint32_t(s[n - 1]);
                uint32_t k = in.regionCount;
                uint64_t needed = uint64_t(values) * (k + 1) + 1;
                if (needed > n)
                    return false;
                uint32_t base = n - uint32_t(needed);
                for (uint32_t v = 0; v < values; ++v) {
                    double value = s[base + v];
                    for (uint32_t r = 0; r < k; ++r)
                        value += s[base + values + v * k + r] * double(in.scalars[r]);
                    s[base + v] = value;
                }
                in.count = base + values;
                break;
            }
            case CharOp_Escape: {
                uint8_t b1 = code.uint8At(at++);
                switch (b1) {
                    case EscapeOp_Flex:
                    case EscapeOp_Flex1:
                    case EscapeOp_Hflex:
                    case EscapeOp_Hflex1: {
                        PrVec2 points[6];
                        PrVec2 origin = p;
                        if (b1 == EscapeOp_Flex && n >= 12) {
                            PrVec2 at2 = p;
                            for (uint32_t i = 0; i < 6; ++i) {
                                at2 = PrVec2{at2.x + float(s[i * 2]), at2.y + float(s[i * 2 + 1])};
                                points[i] = at2;
                            }
                        } else if (b1 == EscapeOp_Hflex && n >= 7) {
                            points[0] = PrVec2{p.x + float(s[0]), p.y};
                            points[1] = PrVec2{points[0].x + float(s[1]), points[0].y + float(s[2])};
                            points[2] = PrVec2{points[1].x + float(s[3]), points[1].y};
                            points[3] = PrVec2{points[2].x + float(s[4]), points[2].y};
                            points[4] = PrVec2{points[3].x + float(s[5]), origin.y};
                            points[5] = PrVec2{points[4].x + float(s[6]), origin.y};
                        } else if (b1 == EscapeOp_Hflex1 && n >= 9) {
                            points[0] = PrVec2{p.x + float(s[0]), p.y + float(s[1])};
                            points[1] = PrVec2{points[0].x + float(s[2]), points[0].y + float(s[3])};
                            points[2] = PrVec2{points[1].x + float(s[4]), points[1].y};
                            points[3] = PrVec2{points[2].x + float(s[5]), points[2].y};
                            points[4] = PrVec2{points[3].x + float(s[6]), points[3].y + float(s[7])};
                            points[5] = PrVec2{points[4].x + float(s[8]), origin.y};
                        } else if (b1 == EscapeOp_Flex1 && n >= 11) {
                            PrVec2 at2 = p;
                            for (uint32_t i = 0; i < 5; ++i) {
                                at2 = PrVec2{at2.x + float(s[i * 2]), at2.y + float(s[i * 2 + 1])};
                                points[i] = at2;
                            }
                            float dx = points[4].x - origin.x;
                            float dy = points[4].y - origin.y;
                            if (prF32Abs(dx) > prF32Abs(dy))
                                points[5] = PrVec2{at2.x + float(s[10]), origin.y};
                            else
                                points[5] = PrVec2{origin.x, at2.y + float(s[10])};
                        } else {
                            in.count = 0;
                            break;
                        }
                        curveTo(in, points[0], points[1], points[2]);
                        curveTo(in, points[3], points[4], points[5]);
                        in.count = 0;
                        break;
                    }
                    case EscapeOp_And:
                        if (n >= 2) {
                            s[n - 2] = (s[n - 2] != 0 && s[n - 1] != 0) ? 1 : 0;
                            --in.count;
                        }
                        break;
                    case EscapeOp_Or:
                        if (n >= 2) {
                            s[n - 2] = (s[n - 2] != 0 || s[n - 1] != 0) ? 1 : 0;
                            --in.count;
                        }
                        break;
                    case EscapeOp_Not:
                        if (n >= 1)
                            s[n - 1] = s[n - 1] == 0 ? 1 : 0;
                        break;
                    case EscapeOp_Abs:
                        if (n >= 1 && s[n - 1] < 0)
                            s[n - 1] = -s[n - 1];
                        break;
                    case EscapeOp_Add:
                    case EscapeOp_Sub:
                    case EscapeOp_Mul:
                    case EscapeOp_Div:
                        if (n >= 2) {
                            double a = s[n - 2];
                            double b = s[n - 1];
                            if (b1 == EscapeOp_Add)
                                s[n - 2] = a + b;
                            else if (b1 == EscapeOp_Sub)
                                s[n - 2] = a - b;
                            else if (b1 == EscapeOp_Mul)
                                s[n - 2] = a * b;
                            else
                                s[n - 2] = b != 0 ? a / b : 0;
                            --in.count;
                        }
                        break;
                    case EscapeOp_Neg:
                        if (n >= 1)
                            s[n - 1] = -s[n - 1];
                        break;
                    case EscapeOp_Eq:
                        if (n >= 2) {
                            s[n - 2] = s[n - 2] == s[n - 1] ? 1 : 0;
                            --in.count;
                        }
                        break;
                    case EscapeOp_Drop:
                        if (n >= 1)
                            --in.count;
                        break;
                    case EscapeOp_Put:
                        if (n >= 2) {
                            int32_t i = int32_t(s[n - 1]);
                            if (i >= 0 && uint32_t(i) < kTransientSize)
                                in.transient[i] = s[n - 2];
                            in.count -= 2;
                        }
                        break;
                    case EscapeOp_Get:
                        if (n >= 1) {
                            int32_t i = int32_t(s[n - 1]);
                            s[n - 1] = i >= 0 && uint32_t(i) < kTransientSize ? in.transient[i] : 0;
                        }
                        break;
                    case EscapeOp_Ifelse:
                        if (n >= 4) {
                            s[n - 4] = s[n - 2] <= s[n - 1] ? s[n - 4] : s[n - 3];
                            in.count -= 3;
                        }
                        break;
                    case EscapeOp_Random:
                        in.seed = in.seed * 1103515245u + 12345u;
                        if (in.count < kMaxStack)
                            s[in.count++] = double((in.seed >> 16) & 0x7fff) / 32768.0 + 1.0 / 65536;
                        break;
                    case EscapeOp_Sqrt:
                        if (n >= 1)
                            s[n - 1] = s[n - 1] > 0 ? double(prF32Sqrt(float(s[n - 1]))) : 0;
                        break;
                    case EscapeOp_Dup:
                        if (n >= 1 && n < kMaxStack)
                            s[in.count++] = s[n - 1];
                        break;
                    case EscapeOp_Exch:
                        if (n >= 2) {
                            double t = s[n - 1];
                            s[n - 1] = s[n - 2];
                            s[n - 2] = t;
                        }
                        break;
                    case EscapeOp_Index:
                        if (n >= 1) {
                            int32_t i = int32_t(s[n - 1]);
                            if (i < 0)
                                i = 0;
                            s[n - 1] = uint32_t(i) + 1 < n ? s[n - 2 - uint32_t(i)] : 0;
                        }
                        break;
                    case EscapeOp_Roll:
                        if (n >= 2) {
                            int32_t span = int32_t(s[n - 2]);
                            int32_t shift = int32_t(s[n - 1]);
                            in.count -= 2;
                            if (span > 0 && uint32_t(span) <= in.count) {
                                double* items = s + in.count - uint32_t(span);
                                shift = ((shift % span) + span) % span;
                                double copy[kMaxStack];
                                for (int32_t i = 0; i < span; ++i)
                                    copy[(i + shift) % span] = items[i];
                                for (int32_t i = 0; i < span; ++i)
                                    items[i] = copy[i];
                            }
                        }
                        break;
                    default:
                        in.count = 0;
                        break;
                }
                break;
            }
            default:
                in.count = 0;
                break;
        }
    }
    return true;
}

} // namespace

bool cffOpen(const Sfnt& sfnt, CffFace* out) {
    *out = CffFace{};
    SfntTable table = sfntTable(sfnt, kCff2);
    bool cff2 = table.size != 0;
    if (!cff2)
        table = sfntTable(sfnt, kCff);
    if (table.size < 4 || table.uint8At(0) != (cff2 ? 2 : 1))
        return false;
    out->table = table;
    out->cff2 = cff2;
    SfntTable top{nullptr, 0};
    uint32_t afterTop = 0;
    if (cff2) {
        uint32_t header = table.uint8At(2);
        uint32_t length = table.uint16At(3);
        if (header + length > table.size)
            return false;
        top = SfntTable{table.data + header, length};
        afterTop = header + length;
    } else {
        CffIndex names = indexAt(table, table.uint8At(2), false);
        CffIndex tops = indexAt(table, names.end, false);
        CffIndex strings = indexAt(table, tops.end, false);
        top = indexItem(tops, 0);
        afterTop = strings.end;
    }
    out->globalSubrs = indexAt(table, afterTop, cff2);
    uint32_t charStrings = 0;
    uint32_t privateSize = 0;
    uint32_t privateOffset = 0;
    uint32_t fdArray = 0;
    uint32_t fdSelect = 0;
    uint32_t vstore = 0;
    uint32_t charset = 0;
    readDict(top, [&](const DictEntry& e) {
        double last = e.count != 0 ? e.operands[e.count - 1] : 0;
        switch (e.op) {
            case DictOp_CharStrings:
                charStrings = uint32_t(last);
                break;
            case DictOp_Private:
                if (e.count >= 2) {
                    privateSize = uint32_t(e.operands[e.count - 2]);
                    privateOffset = uint32_t(last);
                }
                break;
            case DictOp_FdArray:
                fdArray = uint32_t(last);
                break;
            case DictOp_FdSelect:
                fdSelect = uint32_t(last);
                break;
            case DictOp_Vstore:
                vstore = uint32_t(last);
                break;
            case DictOp_Charset:
                charset = uint32_t(last);
                break;
            case DictOp_Ros:
                out->cid = true;
                break;
            default:
                break;
        }
    });
    if (charStrings == 0)
        return false;
    out->charStrings = indexAt(table, charStrings, cff2);
    if (out->charStrings.count == 0)
        return false;
    if (fdArray != 0) {
        CffIndex fonts = indexAt(table, fdArray, cff2);
        for (uint32_t i = 0; i < fonts.count; ++i) {
            uint32_t size = 0;
            uint32_t offset = 0;
            readDict(indexItem(fonts, i), [&](const DictEntry& e) {
                if (e.op == DictOp_Private && e.count >= 2) {
                    size = uint32_t(e.operands[e.count - 2]);
                    offset = uint32_t(e.operands[e.count - 1]);
                }
            });
            out->privates.push(readPrivate(table, offset, size, cff2));
        }
        if (fdSelect != 0 && fonts.count > 1)
            out->fdSelect = table.from(fdSelect);
    } else {
        out->privates.push(readPrivate(table, privateOffset, privateSize, cff2));
    }
    // The CFF2 VariationStore is an ItemVariationStore after a 16-bit length.
    if (cff2 && vstore != 0)
        out->variations = table.from(vstore + 2);
    // Charsets 0, 1 and 2 are predefined; seac reads only the ISOAdobe one, whose glyphs are their SIDs.
    if (!cff2 && charset > 2)
        out->charset = table.from(charset);
    return true;
}

void cffReset(CffFace& face) {
    face.privates.reset();
}

uint32_t cffGlyphCount(const CffFace& face) {
    return face.charStrings.count;
}

bool cffLoad(const CffFace& face, SfntCoords coords, uint32_t glyph, FontGlyphOutline* out) {
    fontOutlineClear(*out);
    if (glyph >= face.charStrings.count)
        return false;
    Interpreter in = interpreterFor(face, coords, out, glyph);
    run(in, indexItem(face.charStrings, glyph), 0);
    closeContour(in);
    return true;
}

} // namespace pr_impl
