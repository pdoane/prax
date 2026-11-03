// Reading a Lottie animation's JSON into its model.
//
// lottie-spec - https://lottie.github.io/lottie-spec/

#include "lottie/src/lottie_parse.h"

#include "pr/base64.h"
#include "pr/count_of.h"
#include "pr/draw_lower.h"
#include "pr/float.h"
#include "pr/json.h"
#include "pr/memory.h"
#include "pr/search.h"
#include "pr/sort.h"
#include "pr/string_ops.h"
#include "pr/vec.h"

#include "lottie/src/lottie_expression.h"
#include "lottie/src/lottie_model.h"
#include "lottie/src/lottie_text.h"

namespace pr_impl {

namespace {

struct Parser {
    PrJson* doc;
    LottieAnimation* a;
    RawArray<float> scratch;

    // Differences that older exporters wrote, corrected while reading: fill and stroke colors out of 255 before 4.1.9,
    // and a path's closedness on its item or mask rather than on its shapes before 4.4.18.
    bool colorsOf255;
    bool closedOnItem;

    int8_t closed;              // the closedness applied to shapes as they are read; -1 keeps each shape's own
    uint32_t parent;            // the parent node of what is being read; kLottieNone for none
};

// Returns true if the animation's version, "major.minor.patch", is before the given one. A file with no version counts
// as current.
bool before(PrStringView version, uint32_t major, uint32_t minor, uint32_t patch) {
    if (version.data == nullptr)
        return false;
    uint32_t parts[3] = {0, 0, 0};
    uint32_t part = 0;
    for (size_t i = 0; i < version.size && part < 3; ++i) {
        char c = version.data[i];
        if (c == '.')
            ++part;
        else if (prAsciiIsDigit(c))
            parts[part] = parts[part] * 10 + uint32_t(c - '0');
    }
    uint32_t minimum[3] = {major, minor, patch};
    for (uint32_t i = 0; i < 3; ++i) {
        if (parts[i] != minimum[i])
            return parts[i] < minimum[i];
    }
    return false;
}

uint32_t member(const Parser& p, uint32_t object, const char* key) {
    return prJsonMember(p.doc, object, key);
}

// The parser's document, read as prJsonKind, prJsonCount, prJsonNumber, prJsonElement, prJsonFirst and prJsonAfter read
// it.
PrJsonKind kindOf(const Parser& p, uint32_t index) {
    return prJsonKind(p.doc, index);
}

uint32_t sizeOf(const Parser& p, uint32_t index) {
    return prJsonCount(p.doc, index);
}

double numberAt(const Parser& p, uint32_t index, double fallback) {
    return prJsonNumber(p.doc, index, fallback);
}

uint32_t elementOf(const Parser& p, uint32_t array, uint32_t at) {
    return prJsonElement(p.doc, array, at);
}

uint32_t firstIn(const Parser& p, uint32_t container) {
    return prJsonFirst(p.doc, container);
}

uint32_t nextIn(const Parser& p, uint32_t container, uint32_t index) {
    return prJsonAfter(p.doc, container, index);
}

double number(const Parser& p, uint32_t object, const char* key, double fallback) {
    return numberAt(p, member(p, object, key), fallback);
}

// Reads a number as an integer, truncated and saturated.
int32_t integer(const Parser& p, uint32_t object, const char* key, double fallback) {
    return prF64SaturateI32(number(p, object, key, fallback));
}

// Reads a number as a count, truncated and saturated.
uint32_t count(const Parser& p, uint32_t object, const char* key, uint32_t fallback) {
    return prF64SaturateU32(number(p, object, key, fallback));
}

// Reads a number as one of an enumeration's values. Returns `fallback` if the number cannot be one.
uint8_t enumeration(const Parser& p, uint32_t object, const char* key, uint8_t fallback) {
    int32_t value = integer(p, object, key, fallback);
    return value >= 0 && value <= 0xff ? uint8_t(value) : fallback;
}

bool flag(const Parser& p, uint32_t object, const char* key) {
    return number(p, object, key, 0) != 0;
}

PrStringView text(const Parser& p, uint32_t object, const char* key) {
    return prJsonString(p.doc, member(p, object, key));
}

LottieString keep(Parser& p, PrStringView text) {
    LottieString s{p.a->strings.count, uint32_t(text.size)};
    if (text.size > 0)
        p.a->strings.append(text.data, uint32_t(text.size));
    return s;
}

// Adds a node under the parser's parent, named by `object`. Returns kLottieNone if the parser has no parent.
uint32_t addNode(Parser& p, LottieNodeKind kind, LottieRole role, uint32_t object) {
    if (p.parent == kLottieNone)
        return kLottieNone;
    LottieNode node{};
    node.kind = kind;
    node.role = role;
    node.name = keep(p, text(p, object, "nm"));
    node.matchName = keep(p, text(p, object, "mn"));
    node.index = integer(p, object, "ix", 0);
    node.parent = p.parent;
    node.firstChild = kLottieNone;
    node.lastChild = kLottieNone;
    node.nextSibling = kLottieNone;
    node.property = kLottieNone;
    node.layer = kLottieNone;
    node.comp = kLottieNone;
    uint32_t index = p.a->nodes.count;
    p.a->nodes.push(node);
    LottieNode& parent = p.a->nodes.data[p.parent];
    if (parent.lastChild == kLottieNone)
        parent.firstChild = index;
    else
        p.a->nodes.data[parent.lastChild].nextSibling = index;
    parent.lastChild = index;
    ++parent.childCount;
    return index;
}

// Makes the property a node of its role, through which expressions find it. A property of kLottieNone passes through.
uint32_t attach(Parser& p, uint32_t property, LottieRole role, uint32_t object) {
    if (property == kLottieNone)
        return property;
    uint32_t node = addNode(p, LottieNodeKind_Property, role, object);
    if (node != kLottieNone) {
        p.a->nodes.data[node].property = property;
        p.a->properties.data[property].node = node;
    }
    return property;
}

// ---------------------------------------------------------------------------------------------------------------------
// Values

// Returns an array's first two numbers as a point. Missing numbers are zero.
PrVec2 pointOf(const Parser& p, uint32_t array) {
    uint32_t x = firstIn(p, array);
    uint32_t y = x == kPrJsonMissing ? kPrJsonMissing : nextIn(p, array, x);
    return PrVec2{float(numberAt(p, x, 0)), float(numberAt(p, y, 0))};
}

// Appends a Bézier shape object (`c`, `v`, `i` and `o`) to `out`, with its tangents as points. The shape has `count`
// vertices if `count` is not kLottieNone, and otherwise as many vertices as the object has. A shape with too few
// vertices repeats its last vertex.
void readShape(const Parser& p, uint32_t shape, uint32_t count, RawArray<float>& out) {
    uint32_t v = member(p, shape, "v");
    uint32_t in = member(p, shape, "i");
    uint32_t o = member(p, shape, "o");
    uint32_t has = sizeOf(p, v);
    if (count == kLottieNone)
        count = has;
    bool closed = p.closed >= 0 ? p.closed != 0 : flag(p, shape, "c");
    out.push(closed ? 1.0f : 0.0f);
    out.push(float(count));
    // Walks the three arrays together. A vertex past the end of an array repeats the array's last vertex.
    uint32_t vertex = firstIn(p, v);
    uint32_t inTangent = firstIn(p, in);
    uint32_t outTangent = firstIn(p, o);
    PrVec2 point{0, 0};
    PrVec2 before{0, 0};
    PrVec2 after{0, 0};
    for (uint32_t k = 0; k < count; ++k) {
        if (vertex != kPrJsonMissing) {
            point = pointOf(p, vertex);
            vertex = nextIn(p, v, vertex);
        }
        if (inTangent != kPrJsonMissing) {
            before = pointOf(p, inTangent);
            inTangent = nextIn(p, in, inTangent);
        }
        if (outTangent != kPrJsonMissing) {
            after = pointOf(p, outTangent);
            outTangent = nextIn(p, o, outTangent);
        }
        out.push(point.x);
        out.push(point.y);
        out.push(point.x + before.x);
        out.push(point.y + before.y);
        out.push(point.x + after.x);
        out.push(point.y + after.y);
    }
}

enum {
    ValueKind_Numbers,
    ValueKind_Shape,
};
typedef uint8_t ValueKind;

// Reads a keyframe's value into `out`, replacing its contents. The value is a number, an array of numbers, or a bezier
// shape, bare or in an array of one. `dims` is the number of floats in a value, or kLottieNone for as many as the first
// value has.
void readValue(const Parser& p, uint32_t value, ValueKind kind, uint32_t dims, RawArray<float>& out, float scale = 1) {
    out.clear();
    if (kind == ValueKind_Shape) {
        uint32_t shape = kindOf(p, value) == PrJsonKind_Array ? elementOf(p, value, 0) : value;
        uint32_t count = dims == kLottieNone ? kLottieNone : (dims - 2) / 6;
        readShape(p, shape, count, out);
        return;
    }
    if (kindOf(p, value) == PrJsonKind_Array) {
        for (uint32_t i = firstIn(p, value); i != kPrJsonMissing; i = nextIn(p, value, i))
            out.push(float(numberAt(p, i, 0)));
    } else if (kindOf(p, value) == PrJsonKind_Number) {
        out.push(float(numberAt(p, value, 0)));
    }
    if (dims != kLottieNone) {
        // Short values are padded with zeros, and long ones are truncated.
        while (out.count < dims)
            out.push(0);
        out.count = dims;
    }
    for (uint32_t i = 0; i < out.count && scale != 1; ++i)
        out.data[i] *= scale;
}

uint32_t pool(Parser& p, const RawArray<float>& values) {
    uint32_t offset = uint32_t(p.a->numbers.count);
    if (values.count > 0)
        p.a->numbers.append(values.data, values.count);
    return offset;
}

uint32_t fixedProperty(Parser& p, const float* values, uint32_t dims) {
    LottieProperty property{};
    property.expression = kLottieNone;
    property.node = kLottieNone;
    property.value = uint32_t(p.a->numbers.count);
    property.dims = dims;
    p.a->numbers.append(values, dims);
    p.a->properties.push(property);
    return p.a->properties.count - 1;
}

// Returns an easing's `x` or `y` at `dim`. The easing gives a single number, or one number per dimension.
float easingAt(const Parser& p, uint32_t easing, const char* axis, uint32_t dim, float fallback) {
    uint32_t value = member(p, easing, axis);
    if (kindOf(p, value) == PrJsonKind_Array) {
        uint32_t count = sizeOf(p, value);
        if (count == 0)
            return fallback;
        return float(numberAt(p, elementOf(p, value, dim < count ? dim : 0), double(fallback)));
    }
    return float(numberAt(p, value, double(fallback)));
}

// Returns true if `c` lies on the line through `a` and `b`, within a small tolerance.
bool onLine(const float* a, const float* b, const float* c) {
    double x1 = double(a[0]), y1 = double(a[1]), x2 = double(b[0]), y2 = double(b[1]);
    double x3 = double(c[0]), y3 = double(c[1]);
    double det = x1 * y2 + y1 * x3 + x2 * y3 - x3 * y2 - y3 * x1 - x2 * y1;
    return det > -0.001 && det < 0.001;
}

// Returns true if a keyframe from `s` to `e` with tangents `to` and `ti` moves along a straight line. Such a keyframe
// interpolates per dimension rather than along its path, which gives the same motion without measuring the path.
bool straight(const float* s, const float* e, const float* to, const float* ti, uint32_t dims) {
    bool same = true;
    bool still = true;
    for (uint32_t i = 0; i < dims; ++i) {
        same = same && s[i] == e[i];
        still = still && to[i] == 0 && ti[i] == 0;
    }
    if (same)
        return still;
    if (dims == 2) {
        float o[2] = {s[0] + to[0], s[1] + to[1]};
        float i[2] = {e[0] + ti[0], e[1] + ti[1]};
        return onLine(s, e, o) && onLine(s, e, i);
    }
    if (dims == 3) {
        // The tangent points must lie on the line in each of the three planes.
        float o[3] = {s[0] + to[0], s[1] + to[1], s[2] + to[2]};
        float i[3] = {e[0] + ti[0], e[1] + ti[1], e[2] + ti[2]};
        auto on3 = [](const float* a, const float* b, const float* c) {
            float axy[2] = {a[0], a[1]}, bxy[2] = {b[0], b[1]}, cxy[2] = {c[0], c[1]};
            float axz[2] = {a[0], a[2]}, bxz[2] = {b[0], b[2]}, cxz[2] = {c[0], c[2]};
            float ayz[2] = {a[1], a[2]}, byz[2] = {b[1], b[2]}, cyz[2] = {c[1], c[2]};
            return onLine(axy, bxy, cxy) && onLine(axz, bxz, cxz) && onLine(ayz, byz, cyz);
        };
        return on3(s, e, o) && on3(s, e, i);
    }
    return false;
}

// Reads an animatable property, `{"k": value}` or `{"a": 1, "k": [keyframes]}`, into the animation. Returns kLottieNone
// if it is absent. `dims` is the number of floats in a value, or kLottieNone for as many as the first value has.
uint32_t readValues(Parser& p, uint32_t object, ValueKind kind, uint32_t dims, float scale) {
    if (kindOf(p, object) != PrJsonKind_Object)
        return kLottieNone;
    uint32_t k = member(p, object, "k");
    if (k == kPrJsonMissing)
        return kLottieNone;
    uint32_t first = elementOf(p, k, 0);
    bool keyframed =
        kindOf(p, first) == PrJsonKind_Object && (kind == ValueKind_Numbers || member(p, first, "t") != kPrJsonMissing);
    if (!keyframed) {
        readValue(p, k, kind, dims, p.scratch, scale);
        if (p.scratch.count == 0)
            return kLottieNone;
        return fixedProperty(p, p.scratch.data, p.scratch.count);
    }

    LottieProperty property{};
    property.expression = kLottieNone;
    property.node = kLottieNone;
    uint32_t count = sizeOf(p, k);
    // Every value takes as many floats as the first start value.
    for (uint32_t i = firstIn(p, k); i != kPrJsonMissing && dims == kLottieNone; i = nextIn(p, k, i)) {
        uint32_t s = member(p, i, "s");
        if (s != kPrJsonMissing) {
            readValue(p, s, kind, kLottieNone, p.scratch);
            dims = p.scratch.count;
        }
    }
    if (dims == kLottieNone || dims == 0)
        return kLottieNone;
    property.dims = dims;
    property.keyframe = p.a->keyframes.count;
    property.keyframeCount = count;
    uint32_t previousEnd = kLottieNone;
    for (uint32_t frame = firstIn(p, k); frame != kPrJsonMissing; frame = nextIn(p, k, frame)) {
        LottieKeyframe kf{};
        kf.time = number(p, frame, "t", 0);
        kf.hold = flag(p, frame, "h");
        kf.easing = kLottieNone;
        kf.spatial = kLottieNone;
        uint32_t s = member(p, frame, "s");
        if (s != kPrJsonMissing) {
            readValue(p, s, kind, dims, p.scratch, scale);
            kf.start = pool(p, p.scratch);
        } else if (previousEnd != kLottieNone) {
            kf.start = previousEnd;
        } else {
            p.scratch.clear();
            while (p.scratch.count < dims)
                p.scratch.push(0);
            kf.start = pool(p, p.scratch);
        }
        uint32_t e = member(p, frame, "e");
        if (e != kPrJsonMissing) {
            readValue(p, e, kind, dims, p.scratch, scale);
            kf.end = pool(p, p.scratch);
        } else {
            kf.end = kf.start;
        }
        previousEnd = kf.end;
        uint32_t o = member(p, frame, "o");
        uint32_t in = member(p, frame, "i");
        if (o != kPrJsonMissing && in != kPrJsonMissing) {
            uint32_t easingDims = kindOf(p, member(p, o, "x")) == PrJsonKind_Array && kind == ValueKind_Numbers ? dims : 1;
            kf.easing = p.a->numbers.count;
            kf.easingDims = easingDims;
            for (uint32_t d = 0; d < easingDims; ++d) {
                p.a->numbers.push(easingAt(p, o, "x", d, 0));
                p.a->numbers.push(easingAt(p, o, "y", d, 0));
                p.a->numbers.push(easingAt(p, in, "x", d, 1));
                p.a->numbers.push(easingAt(p, in, "y", d, 1));
            }
        }
        uint32_t to = member(p, frame, "to");
        uint32_t ti = member(p, frame, "ti");
        if (kind == ValueKind_Numbers && kindOf(p, to) == PrJsonKind_Array && kindOf(p, ti) == PrJsonKind_Array) {
            kf.spatial = p.a->numbers.count;
            readValue(p, to, kind, dims, p.scratch);
            pool(p, p.scratch);
            readValue(p, ti, kind, dims, p.scratch);
            pool(p, p.scratch);
        }
        p.a->keyframes.push(kf);
    }
    // A keyframe ends where the next keyframe starts, if the next one specifies a start. The last keyframe holds its
    // start.
    uint32_t next = firstIn(p, k);
    for (uint32_t i = 0; i < count; ++i) {
        LottieKeyframe& kf = p.a->keyframes.data[property.keyframe + i];
        next = nextIn(p, k, next);
        if (next != kPrJsonMissing && member(p, next, "s") != kPrJsonMissing)
            kf.end = p.a->keyframes.data[property.keyframe + i + 1].start;
        if (kf.spatial != kLottieNone) {
            const float* values = p.a->numbers.data;
            if (straight(values + kf.start, values + kf.end, values + kf.spatial, values + kf.spatial + dims, dims))
                kf.spatial = kLottieNone;
        }
    }
    p.a->properties.push(property);
    return p.a->properties.count - 1;
}

uint32_t readProperty(Parser& p, uint32_t object, ValueKind kind = ValueKind_Numbers, uint32_t dims = kLottieNone,
        float scale = 1) {
    uint32_t property = readValues(p, object, kind, dims, scale);
    PrStringView expression = text(p, object, "x");
    if (property != kLottieNone && expression.data != nullptr && expression.size > 0) {
        LottieString source = keep(p, expression);
        p.a->properties.data[property].expression = source.offset;
        p.a->properties.data[property].expressionSize = source.size;
    }
    return property;
}

// Reads a number property, with the fixed value `fallback` if it is absent.
uint32_t readScalar(Parser& p, uint32_t object, const char* key, float fallback) {
    uint32_t property = readProperty(p, member(p, object, key), ValueKind_Numbers, 1);
    if (property == kLottieNone)
        property = fixedProperty(p, &fallback, 1);
    return property;
}

// ---------------------------------------------------------------------------------------------------------------------
// Transforms

uint32_t readTransform(Parser& p, uint32_t object) {
    LottieTransform t{};
    uint32_t outer = p.parent;
    uint32_t group = addNode(p, LottieNodeKind_Group, LottieRole_Transform, object);
    if (group != kLottieNone)
        p.parent = group;
    auto read = [&](const char* key, LottieRole role, uint32_t dims) {
        uint32_t property = member(p, object, key);
        return attach(p, readProperty(p, property, ValueKind_Numbers, dims), role, property);
    };
    t.anchor = read("a", LottieRole_AnchorPoint, kLottieNone);
    uint32_t position = member(p, object, "p");
    t.position = kLottieNone;
    t.positionX = kLottieNone;
    t.positionY = kLottieNone;
    t.positionZ = kLottieNone;
    if (flag(p, position, "s")) {
        auto axis = [&](const char* key, LottieRole role) {
            uint32_t property = member(p, position, key);
            return attach(p, readProperty(p, property, ValueKind_Numbers, 1), role, property);
        };
        t.positionX = axis("x", LottieRole_PositionX);
        t.positionY = axis("y", LottieRole_PositionY);
        t.positionZ = axis("z", LottieRole_PositionZ);
    } else {
        t.position = attach(p, readProperty(p, position), LottieRole_Position, position);
    }
    t.scale = read("s", LottieRole_Scale, kLottieNone);
    t.rotationX = read("rx", LottieRole_RotationX, 1);
    t.rotationY = read("ry", LottieRole_RotationY, 1);
    t.orientation = read("or", LottieRole_Orientation, 3);
    t.rotation = read("r", LottieRole_Rotation, 1);
    if (t.rotation == kLottieNone)
        t.rotation = read("rz", LottieRole_Rotation, 1);
    t.skew = read("sk", LottieRole_Skew, 1);
    t.skewAxis = read("sa", LottieRole_SkewAxis, 1);
    t.opacity = read("o", LottieRole_Opacity, 1);
    t.startOpacity = read("so", LottieRole_Start, 1);
    t.endOpacity = read("eo", LottieRole_End, 1);
    p.parent = outer;
    p.a->transforms.push(t);
    return p.a->transforms.count - 1;
}

// ---------------------------------------------------------------------------------------------------------------------
// Shapes

PrDrawBlendMode blendOf(double bm, bool* additive) {
    static const PrDrawBlendMode kModes[] = {
        PrDrawBlendMode_Normal,     PrDrawBlendMode_Multiply,   PrDrawBlendMode_Screen,
        PrDrawBlendMode_Overlay,    PrDrawBlendMode_Darken,     PrDrawBlendMode_Lighten,
        PrDrawBlendMode_ColorDodge, PrDrawBlendMode_ColorBurn,  PrDrawBlendMode_HardLight,
        PrDrawBlendMode_SoftLight,  PrDrawBlendMode_Difference, PrDrawBlendMode_Exclusion,
        PrDrawBlendMode_Hue,        PrDrawBlendMode_Saturation, PrDrawBlendMode_Color,
        PrDrawBlendMode_Luminosity,
    };
    *additive = bm == 16;
    if (bm == 17)
        return PrDrawBlendMode_HardMix;
    uint32_t index = uint32_t(bm);
    return bm >= 0 && index < countOf(kModes) ? kModes[index] : PrDrawBlendMode_Normal;
}

// Returns true if each of the table's entries is in order relative to the next.
template <typename T, size_t N, typename InOrder>
constexpr bool sortedTable(const T (&table)[N], InOrder inOrder) {
    for (size_t i = 1; i < N; ++i) {
        if (!inOrder(table[i - 1], table[i]))
            return false;
    }
    return true;
}

// Returns an item type's two letters as one number, which sorts in the same order as the letters. Returns zero for any
// other length.
constexpr uint16_t itemCode(const char* ty, size_t size) {
    return size == 2 ? uint16_t(uint8_t(ty[0]) << 8 | uint8_t(ty[1])) : 0;
}

struct ItemName {
    uint16_t code;
    LottieItemKind kind;

    bool operator<(uint16_t other) const { return code < other; }
};

// The item kind of each type, sorted by code.
constexpr ItemName kItemNames[] = {
    {itemCode("el", 2), LottieItemKind_Ellipse},      {itemCode("fl", 2), LottieItemKind_Fill},
    {itemCode("gf", 2), LottieItemKind_GradientFill}, {itemCode("gr", 2), LottieItemKind_Group},
    {itemCode("gs", 2), LottieItemKind_GradientStroke}, {itemCode("mm", 2), LottieItemKind_Merge},
    {itemCode("no", 2), LottieItemKind_NoStyle},      {itemCode("op", 2), LottieItemKind_OffsetPath},
    {itemCode("pb", 2), LottieItemKind_PuckerBloat},  {itemCode("rc", 2), LottieItemKind_Rect},
    {itemCode("rd", 2), LottieItemKind_RoundCorners}, {itemCode("rp", 2), LottieItemKind_Repeater},
    {itemCode("sh", 2), LottieItemKind_Path},         {itemCode("sr", 2), LottieItemKind_Star},
    {itemCode("st", 2), LottieItemKind_Stroke},       {itemCode("tm", 2), LottieItemKind_Trim},
    {itemCode("tr", 2), LottieItemKind_Transform},    {itemCode("tw", 2), LottieItemKind_Twist},
    {itemCode("zz", 2), LottieItemKind_ZigZag},
};
static_assert(sortedTable(kItemNames, [](const ItemName& a, const ItemName& b) { return a.code < b.code; }),
    "kItemNames is in the order of its codes");

LottieItemKind itemKindOf(PrStringView ty) {
    uint16_t code = itemCode(ty.data, ty.size);
    uint32_t at = lowerBound(kItemNames, uint32_t(countOf(kItemNames)), code);
    return at < countOf(kItemNames) && kItemNames[at].code == code ? kItemNames[at].kind : LottieItemKind_Unknown;
}

void readStroke(Parser& p, uint32_t object, LottieItem& item) {
    item.props[LottieProp_Width] = readScalar(p, object, "w", 0);
    // Caps and joins are round if the file leaves them out.
    double lc = number(p, object, "lc", 2);
    double lj = number(p, object, "lj", 2);
    item.cap = lc == 1 ? PrPathCap_Butt : lc == 3 ? PrPathCap_Square : PrPathCap_Round;
    item.join = lj == 1 ? PrPathJoin_Miter : lj == 3 ? PrPathJoin_Bevel : PrPathJoin_Round;
    item.miterLimit = lj == 1 ? float(number(p, object, "ml", 4)) : 4;
    uint32_t dashes = member(p, object, "d");
    item.first = p.a->dashes.count;
    item.count = 0;
    for (uint32_t i = firstIn(p, dashes); i != kPrJsonMissing; i = nextIn(p, dashes, i)) {
        PrStringView n = text(p, i, "n");
        LottieDash dash{};
        dash.kind = n == "o"   ? LottieDashKind_Offset : n == "g" ? LottieDashKind_Gap : LottieDashKind_Dash;
        dash.value = readScalar(p, i, "v", 0);
        p.a->dashes.push(dash);
        ++item.count;
    }
}

void readGradient(Parser& p, uint32_t object, LottieItem& item) {
    item.mode = enumeration(p, object, "t", 1);
    item.props[LottieProp_Start] = readProperty(p, member(p, object, "s"), ValueKind_Numbers, 2);
    item.props[LottieProp_End] = readProperty(p, member(p, object, "e"), ValueKind_Numbers, 2);
    item.props[LottieProp_HighlightLength] = readScalar(p, object, "h", 0);
    item.props[LottieProp_HighlightAngle] = readScalar(p, object, "a", 0);
    uint32_t g = member(p, object, "g");
    item.stops = count(p, g, "p", 0);
    item.props[LottieProp_Stops] = readProperty(p, member(p, g, "k"));
}

void readItems(Parser& p, uint32_t array, uint32_t* first, uint32_t* count);

// The role by which expressions know each of an item's properties, by the item's kind and the property's key.
struct ItemRole {
    LottieItemKind kind;
    LottieProp slot;
    const char* key;
    LottieRole role;

    constexpr bool operator<(LottieItemKind other) const { return kind < other; }
};

constexpr ItemRole kItemRoles[] = {
    {LottieItemKind_Path, LottieProp_Shape, "ks", LottieRole_Path},
    {LottieItemKind_Rect, LottieProp_Position, "p", LottieRole_Position},
    {LottieItemKind_Rect, LottieProp_Size, "s", LottieRole_Size},
    {LottieItemKind_Rect, LottieProp_Roundness, "r", LottieRole_Roundness},
    {LottieItemKind_Ellipse, LottieProp_Position, "p", LottieRole_Position},
    {LottieItemKind_Ellipse, LottieProp_Size, "s", LottieRole_Size},
    {LottieItemKind_Star, LottieProp_Position, "p", LottieRole_Position},
    {LottieItemKind_Star, LottieProp_OuterRadius, "or", LottieRole_OuterRadius},
    {LottieItemKind_Star, LottieProp_OuterRoundness, "os", LottieRole_OuterRoundness},
    {LottieItemKind_Star, LottieProp_InnerRadius, "ir", LottieRole_InnerRadius},
    {LottieItemKind_Star, LottieProp_InnerRoundness, "is", LottieRole_InnerRoundness},
    {LottieItemKind_Star, LottieProp_Points, "pt", LottieRole_Points},
    {LottieItemKind_Star, LottieProp_Rotation, "r", LottieRole_Rotation},
    {LottieItemKind_Fill, LottieProp_Color, "c", LottieRole_Color},
    {LottieItemKind_Fill, LottieProp_Opacity, "o", LottieRole_Opacity},
    {LottieItemKind_Stroke, LottieProp_Color, "c", LottieRole_Color},
    {LottieItemKind_Stroke, LottieProp_Opacity, "o", LottieRole_Opacity},
    {LottieItemKind_Stroke, LottieProp_Width, "w", LottieRole_StrokeWidth},
    {LottieItemKind_GradientFill, LottieProp_Opacity, "o", LottieRole_Opacity},
    {LottieItemKind_GradientFill, LottieProp_Start, "s", LottieRole_Start},
    {LottieItemKind_GradientFill, LottieProp_End, "e", LottieRole_End},
    {LottieItemKind_GradientStroke, LottieProp_Opacity, "o", LottieRole_Opacity},
    {LottieItemKind_GradientStroke, LottieProp_Width, "w", LottieRole_StrokeWidth},
    {LottieItemKind_GradientStroke, LottieProp_Start, "s", LottieRole_Start},
    {LottieItemKind_GradientStroke, LottieProp_End, "e", LottieRole_End},
    {LottieItemKind_Trim, LottieProp_Start, "s", LottieRole_Start},
    {LottieItemKind_Trim, LottieProp_End, "e", LottieRole_End},
    {LottieItemKind_Trim, LottieProp_Offset, "o", LottieRole_Offset},
    {LottieItemKind_RoundCorners, LottieProp_Roundness, "r", LottieRole_Roundness},
    {LottieItemKind_Repeater, LottieProp_Copies, "c", LottieRole_Copies},
    {LottieItemKind_Repeater, LottieProp_Offset, "o", LottieRole_Offset},
    {LottieItemKind_PuckerBloat, LottieProp_Amount, "a", LottieRole_Amount},
    {LottieItemKind_OffsetPath, LottieProp_Amount, "a", LottieRole_Amount},
};
static_assert(sortedTable(kItemRoles, [](const ItemRole& a, const ItemRole& b) { return a.kind <= b.kind; }),
    "kItemRoles is in the order of its kinds");

LottieRole itemRoleOf(LottieItemKind kind) {
    switch (kind) {
        case LottieItemKind_Group:
            return LottieRole_ShapeGroup;
        case LottieItemKind_Path:
        case LottieItemKind_Rect:
        case LottieItemKind_Ellipse:
        case LottieItemKind_Star:
            return LottieRole_Shape;
        case LottieItemKind_Fill:
        case LottieItemKind_Stroke:
        case LottieItemKind_GradientFill:
        case LottieItemKind_GradientStroke:
            return LottieRole_Style;
        default:
            return LottieRole_Modifier;
    }
}

void readItem(Parser& p, uint32_t object, uint32_t index) {
    LottieItem item{};
    for (uint32_t& prop : item.props)
        prop = kLottieNone;
    item.transform = kLottieNone;
    item.kind = itemKindOf(text(p, object, "ty"));
    // A group's transform is a separate group of properties beside the group's contents, as in After Effects. Every
    // other item is a node of its own kind.
    uint32_t outer = p.parent;
    if (item.kind == LottieItemKind_Transform) {
        if (p.parent != kLottieNone && p.a->nodes.data[p.parent].role == LottieRole_Contents &&
                p.a->nodes.data[p.parent].kind == LottieNodeKind_Group)
            p.parent = p.a->nodes.data[p.parent].parent;
    } else {
        uint32_t node = addNode(p, LottieNodeKind_Group, itemRoleOf(item.kind), object);
        if (node != kLottieNone)
            p.parent = node;
    }
    item.hidden = flag(p, object, "hd");
    item.reversed = number(p, object, "d", 1) == 3;
    bool additive = false;
    item.blend = blendOf(number(p, object, "bm", 0), &additive);
    switch (item.kind) {
        case LottieItemKind_Group: {
            uint32_t group = p.parent;
            uint32_t contents = addNode(p, LottieNodeKind_Group, LottieRole_Contents, kPrJsonMissing);
            if (contents != kLottieNone)
                p.parent = contents;
            readItems(p, member(p, object, "it"), &item.first, &item.count);
            p.parent = group;
            break;
        }
        case LottieItemKind_Path:
            p.closed = p.closedOnItem ? int8_t(flag(p, object, "closed")) : int8_t(-1);
            item.props[LottieProp_Shape] = readProperty(p, member(p, object, "ks"), ValueKind_Shape);
            p.closed = -1;
            break;
        case LottieItemKind_Rect:
            item.props[LottieProp_Position] = readProperty(p, member(p, object, "p"), ValueKind_Numbers, 2);
            item.props[LottieProp_Size] = readProperty(p, member(p, object, "s"), ValueKind_Numbers, 2);
            item.props[LottieProp_Roundness] = readScalar(p, object, "r", 0);
            break;
        case LottieItemKind_Ellipse:
            item.props[LottieProp_Position] = readProperty(p, member(p, object, "p"), ValueKind_Numbers, 2);
            item.props[LottieProp_Size] = readProperty(p, member(p, object, "s"), ValueKind_Numbers, 2);
            break;
        case LottieItemKind_Star:
            item.mode = enumeration(p, object, "sy", 1);
            item.props[LottieProp_Position] = readProperty(p, member(p, object, "p"), ValueKind_Numbers, 2);
            item.props[LottieProp_OuterRadius] = readScalar(p, object, "or", 0);
            item.props[LottieProp_OuterRoundness] = readScalar(p, object, "os", 0);
            item.props[LottieProp_InnerRadius] = readScalar(p, object, "ir", 0);
            item.props[LottieProp_InnerRoundness] = readScalar(p, object, "is", 0);
            item.props[LottieProp_Points] = readScalar(p, object, "pt", 5);
            item.props[LottieProp_Rotation] = readScalar(p, object, "r", 0);
            break;
        case LottieItemKind_Fill:
            item.props[LottieProp_Color] =
                readProperty(p, member(p, object, "c"), ValueKind_Numbers, 4, p.colorsOf255 ? 1.0f / 255 : 1);
            item.props[LottieProp_Opacity] = readScalar(p, object, "o", 100);
            item.rule = number(p, object, "r", 1) == 2 ? PrPathFillRule_EvenOdd : PrPathFillRule_NonZero;
            break;
        case LottieItemKind_Stroke:
            item.props[LottieProp_Color] =
                readProperty(p, member(p, object, "c"), ValueKind_Numbers, 4, p.colorsOf255 ? 1.0f / 255 : 1);
            item.props[LottieProp_Opacity] = readScalar(p, object, "o", 100);
            readStroke(p, object, item);
            break;
        case LottieItemKind_GradientFill:
            item.props[LottieProp_Opacity] = readScalar(p, object, "o", 100);
            item.rule = number(p, object, "r", 1) == 2 ? PrPathFillRule_EvenOdd : PrPathFillRule_NonZero;
            readGradient(p, object, item);
            break;
        case LottieItemKind_GradientStroke:
            item.props[LottieProp_Opacity] = readScalar(p, object, "o", 100);
            readGradient(p, object, item);
            readStroke(p, object, item);
            break;
        case LottieItemKind_Transform:
            item.transform = readTransform(p, object);
            break;
        case LottieItemKind_Trim:
            item.mode = enumeration(p, object, "m", 1);
            item.props[LottieProp_Start] = readScalar(p, object, "s", 0);
            item.props[LottieProp_End] = readScalar(p, object, "e", 100);
            item.props[LottieProp_Offset] = readScalar(p, object, "o", 0);
            break;
        case LottieItemKind_RoundCorners:
            item.props[LottieProp_Roundness] = readScalar(p, object, "r", 0);
            break;
        case LottieItemKind_Repeater:
            item.mode = enumeration(p, object, "m", 1);
            item.props[LottieProp_Copies] = readScalar(p, object, "c", 1);
            item.props[LottieProp_Offset] = readScalar(p, object, "o", 0);
            item.transform = readTransform(p, member(p, object, "tr"));
            break;
        case LottieItemKind_PuckerBloat:
            item.props[LottieProp_Amount] = readScalar(p, object, "a", 0);
            break;
        case LottieItemKind_ZigZag:
            item.props[LottieProp_Size] = readScalar(p, object, "s", 0);
            item.props[LottieProp_Points] = readScalar(p, object, "r", 0);
            item.props[LottieProp_Roundness] = readScalar(p, object, "pt", 1);
            break;
        case LottieItemKind_Twist:
            item.props[LottieProp_Amount] = readScalar(p, object, "a", 0);
            item.props[LottieProp_Center] = readProperty(p, member(p, object, "c"), ValueKind_Numbers, 2);
            break;
        case LottieItemKind_OffsetPath: {
            item.props[LottieProp_Amount] = readScalar(p, object, "a", 0);
            item.props[LottieProp_MiterLimit] = readScalar(p, object, "ml", 4);
            double lj = number(p, object, "lj", 1);
            item.join = lj == 1 ? PrPathJoin_Miter : lj == 3 ? PrPathJoin_Bevel : PrPathJoin_Round;
            break;
        }
        case LottieItemKind_Merge:
            item.mode = enumeration(p, object, "mm", 1);
            break;
        default:
            break;
    }
    for (uint32_t r = lowerBound(kItemRoles, uint32_t(countOf(kItemRoles)), item.kind);
            r < countOf(kItemRoles) && kItemRoles[r].kind == item.kind; ++r)
        attach(p, item.props[kItemRoles[r].slot], kItemRoles[r].role, member(p, object, kItemRoles[r].key));
    p.parent = outer;
    p.a->items.data[index] = item;
}

// Reads the items of `array`, stored contiguously in the animation's items.
void readItems(Parser& p, uint32_t array, uint32_t* first, uint32_t* count) {
    *count = sizeOf(p, array);
    *first = p.a->items.count;
    for (uint32_t i = 0; i < *count; ++i)
        p.a->items.emplace();
    uint32_t i = 0;
    for (uint32_t e = firstIn(p, array); e != kPrJsonMissing; e = nextIn(p, array, e))
        readItem(p, e, *first + i++);
}

// ---------------------------------------------------------------------------------------------------------------------
// Text

// Reads a color of three or four numbers. Returns false if there is none.
bool readColor(const Parser& p, uint32_t array, PrColor* out) {
    if (sizeOf(p, array) < 3)
        return false;
    *out = PrColor{
        float(numberAt(p, elementOf(p, array, 0), 0)),
        float(numberAt(p, elementOf(p, array, 1), 0)),
        float(numberAt(p, elementOf(p, array, 2), 0)), 1,
    };
    return true;
}

void readDocument(Parser& p, uint32_t object, double time) {
    LottieTextDocument d{};
    d.time = time;
    d.text = keep(p, text(p, object, "t"));
    // The font is found by name among the fonts, which are read before the documents.
    d.font = kLottieNone;
    PrStringView name = text(p, object, "f");
    for (uint32_t i = 0; i < p.a->textFonts.count; ++i) {
        if (lottieString(*p.a, p.a->textFonts.data[i].name) == name)
            d.font = i;
    }
    if (d.font == kLottieNone && p.a->textFonts.count > 0)
        d.font = 0;
    d.size = float(number(p, object, "s", 10));
    d.tracking = float(number(p, object, "tr", 0));
    d.lineHeight = float(number(p, object, "lh", 0));
    d.baselineShift = float(number(p, object, "ls", 0));
    d.strokeWidth = float(number(p, object, "sw", 0));
    d.hasFill = readColor(p, member(p, object, "fc"), &d.fill);
    d.hasStroke = readColor(p, member(p, object, "sc"), &d.stroke);
    d.justify = enumeration(p, object, "j", 0);
    uint32_t box = member(p, object, "sz");
    d.box = sizeOf(p, box) >= 2;
    if (d.box) {
        d.boxSize = PrVec2{float(numberAt(p, elementOf(p, box, 0), 0)), float(numberAt(p, elementOf(p, box, 1), 0))};
        uint32_t position = member(p, object, "ps");
        d.boxPosition = PrVec2{
            float(numberAt(p, elementOf(p, position, 0), 0)),
            float(numberAt(p, elementOf(p, position, 1), 0)),
        };
    }
    p.a->documents.push(d);
}

void readTextAnimator(Parser& p, uint32_t object) {
    LottieTextAnimator t{};
    uint32_t selector = member(p, object, "s");
    uint32_t props = member(p, object, "a");
    t.start = readScalar(p, selector, "s", 0);
    t.end = readProperty(p, member(p, selector, "e"), ValueKind_Numbers, 1);
    t.offset = readScalar(p, selector, "o", 0);
    t.easeHigh = readScalar(p, selector, "xe", 0);
    t.easeLow = readScalar(p, selector, "ne", 0);
    t.smoothness = readScalar(p, selector, "sm", 100);
    t.amount = readScalar(p, selector, "a", 100);
    t.units = enumeration(p, selector, "r", 1);
    t.basedOn = enumeration(p, selector, "b", 1);
    t.shape = enumeration(p, selector, "sh", 1);
    t.anchor = readProperty(p, member(p, props, "a"));
    t.position = readProperty(p, member(p, props, "p"));
    t.scale = readProperty(p, member(p, props, "s"));
    t.skew = readProperty(p, member(p, props, "sk"), ValueKind_Numbers, 1);
    t.skewAxis = readProperty(p, member(p, props, "sa"), ValueKind_Numbers, 1);
    t.rotation = readProperty(p, member(p, props, "r"), ValueKind_Numbers, 1);
    t.opacity = readProperty(p, member(p, props, "o"), ValueKind_Numbers, 1);
    t.strokeWidth = readProperty(p, member(p, props, "sw"), ValueKind_Numbers, 1);
    t.strokeColor = readProperty(p, member(p, props, "sc"));
    t.fillColor = readProperty(p, member(p, props, "fc"));
    t.fillHue = readProperty(p, member(p, props, "fh"), ValueKind_Numbers, 1);
    t.fillSaturation = readProperty(p, member(p, props, "fs"), ValueKind_Numbers, 1);
    t.fillBrightness = readProperty(p, member(p, props, "fb"), ValueKind_Numbers, 1);
    t.tracking = readProperty(p, member(p, props, "t"), ValueKind_Numbers, 1);
    p.a->animators.push(t);
}

// Reads a text layer's documents, animators and options.
void readText(Parser& p, uint32_t object, LottieLayer& layer) {
    uint32_t d = member(p, object, "d");
    uint32_t k = member(p, d, "k");
    layer.first = p.a->documents.count;
    if (kindOf(p, k) == PrJsonKind_Array) {
        for (uint32_t i = firstIn(p, k); i != kPrJsonMissing; i = nextIn(p, k, i))
            readDocument(p, member(p, i, "s"), number(p, i, "t", 0));
    } else if (kindOf(p, d) == PrJsonKind_Object) {
        // Before 4.4.14, the data is the document itself.
        readDocument(p, d, 0);
    }
    layer.count = p.a->documents.count - layer.first;
    uint32_t animators = member(p, object, "a");
    layer.animators = p.a->animators.count;
    for (uint32_t i = firstIn(p, animators); i != kPrJsonMissing; i = nextIn(p, animators, i))
        readTextAnimator(p, i);
    layer.animatorCount = p.a->animators.count - layer.animators;
    uint32_t more = member(p, object, "m");
    layer.alignment = readProperty(p, member(p, more, "a"));
    layer.grouping = enumeration(p, more, "g", 1);
}

void readFonts(Parser& p, uint32_t fonts) {
    uint32_t list = member(p, fonts, "list");
    for (uint32_t i = firstIn(p, list); i != kPrJsonMissing; i = nextIn(p, list, i)) {
        LottieFont font{};
        font.name = keep(p, text(p, i, "fName"));
        font.family = keep(p, text(p, i, "fFamily"));
        font.style = keep(p, text(p, i, "fStyle"));
        font.ascent = float(number(p, i, "ascent", 75));
        p.a->textFonts.push(font);
    }
}

void readCharShapes(Parser& p, uint32_t chars) {
    // Glyphs have no node for expressions.
    uint32_t outer = p.parent;
    p.parent = kLottieNone;
    for (uint32_t i = firstIn(p, chars); i != kPrJsonMissing; i = nextIn(p, chars, i)) {
        LottieCharShape c{};
        c.character = keep(p, text(p, i, "ch"));
        c.style = keep(p, text(p, i, "style"));
        c.family = keep(p, text(p, i, "fFamily"));
        c.width = float(number(p, i, "w", 0));
        uint32_t data = member(p, i, "data");
        readItems(p, member(p, data, "shapes"), &c.first, &c.count);
        p.a->charShapes.push(c);
    }
    p.parent = outer;
}

// ---------------------------------------------------------------------------------------------------------------------
// Layers and compositions

PrColor hexColor(PrStringView hex) {
    PrColor color{0, 0, 0, 1};
    if (hex.size >= 7 && hex.data[0] == '#') {
        float* channels[3] = {&color.r, &color.g, &color.b};
        for (uint32_t i = 0; i < 3; ++i) {
            char hi = hex.data[1 + i * 2];
            char lo = hex.data[2 + i * 2];
            if (prAsciiIsHexDigit(hi) && prAsciiIsHexDigit(lo))
                *channels[i] = float(prAsciiHexValue(hi) * 16 + prAsciiHexValue(lo)) / 255;
        }
    }
    return color;
}

// Reads an effect's values as nodes under the parser's parent. A group of values (ty 5) becomes a group of its own. If
// `own` is true, the values are also recorded as the effect's own values, in order, with groups recorded as none.
void readEffectValues(Parser& p, uint32_t values, bool own) {
    uint32_t outer = p.parent;
    for (uint32_t v = firstIn(p, values); v != kPrJsonMissing; v = nextIn(p, values, v)) {
        uint32_t property = kLottieNone;
        if (integer(p, v, "ty", 0) == LottieEffectValueType_Group) {
            uint32_t group = addNode(p, LottieNodeKind_Group, LottieRole_Effect, v);
            if (group != kLottieNone) {
                p.parent = group;
                readEffectValues(p, member(p, v, "ef"), false);
                p.parent = outer;
            }
        } else {
            property = attach(p, readProperty(p, member(p, v, "v")), LottieRole_EffectValue, v);
        }
        if (own)
            p.a->effectValues.push(property);
    }
}

void readEffects(Parser& p, uint32_t array, LottieLayer& layer) {
    layer.effects = p.a->effects.count;
    uint32_t outer = p.parent;
    uint32_t group = kLottieNone;
    if (sizeOf(p, array) > 0)
        group = addNode(p, LottieNodeKind_Group, LottieRole_Effects, kPrJsonMissing);
    for (uint32_t i = firstIn(p, array); i != kPrJsonMissing; i = nextIn(p, array, i)) {
        LottieEffect effect{};
        effect.type = LottieEffectType(number(p, i, "ty", 0));
        effect.enabled = number(p, i, "en", 1) != 0;
        effect.values = p.a->effectValues.count;
        p.parent = group;
        uint32_t node = addNode(p, LottieNodeKind_Group, LottieRole_Effect, i);
        p.parent = node;
        readEffectValues(p, member(p, i, "ef"), true);
        effect.count = p.a->effectValues.count - effect.values;
        p.a->effects.push(effect);
    }
    p.parent = outer;
    layer.effectCount = p.a->effects.count - layer.effects;
}

void readMasks(Parser& p, uint32_t array, LottieLayer& layer) {
    layer.masks = p.a->masks.count;
    layer.maskCount = 0;
    uint32_t outer = p.parent;
    uint32_t group = kLottieNone;
    if (sizeOf(p, array) > 0)
        group = addNode(p, LottieNodeKind_Group, LottieRole_Masks, kPrJsonMissing);
    for (uint32_t i = firstIn(p, array); i != kPrJsonMissing; i = nextIn(p, array, i)) {
        p.parent = group;
        uint32_t node = addNode(p, LottieNodeKind_Group, LottieRole_Mask, i);
        p.parent = node;
        LottieMask mask{};
        PrStringView mode = text(p, i, "mode");
        char m = mode.size > 0 ? mode.data[0] : 'a';
        if (m == 'n')
            mask.mode = LottieMaskMode_None;
        else if (m == 's')
            mask.mode = LottieMaskMode_Subtract;
        else if (m == 'i')
            mask.mode = LottieMaskMode_Intersect;
        else if (m == 'l')
            mask.mode = LottieMaskMode_Lighten;
        else if (m == 'd')
            mask.mode = LottieMaskMode_Darken;
        else if (m == 'f')
            mask.mode = LottieMaskMode_Difference;
        else
            mask.mode = LottieMaskMode_Add;
        mask.inverted = flag(p, i, "inv");
        p.closed = p.closedOnItem ? int8_t(flag(p, i, "cl")) : int8_t(-1);
        mask.shape = attach(p, readProperty(p, member(p, i, "pt"), ValueKind_Shape), LottieRole_MaskPath, member(p, i, "pt"));
        p.closed = -1;
        mask.opacity = attach(p, readScalar(p, i, "o", 100), LottieRole_MaskOpacity, member(p, i, "o"));
        mask.expansion = attach(p, readScalar(p, i, "x", 0), LottieRole_MaskExpansion, member(p, i, "x"));
        mask.feather = readProperty(p, member(p, i, "f"), ValueKind_Numbers, 2);
        p.a->masks.push(mask);
        ++layer.maskCount;
    }
    p.parent = outer;
}

// A layer's id and its position in its composition, which sort by id and then by position.
struct LayerId {
    double id;
    uint32_t layer;

    bool operator<(double other) const { return id < other; }
};

// Reads a composition's layers. `refs` receives each layer's asset reference, which resolves precomps once every
// composition is read.
void readLayers(Parser& p, uint32_t array, LottieComp& comp, RawArray<uint32_t>& refs) {
    comp.layers = p.a->layers.count;
    comp.count = sizeOf(p, array);
    for (uint32_t i = 0; i < comp.count; ++i)
        p.a->layers.emplace();
    // Each layer's `ind`, `parent` and `tp`, which are resolved once all layers are read.
    RawArray<double> ids;
    RawArray<double> parents;
    RawArray<double> mattes;
    uint32_t n = 0;
    for (uint32_t e = firstIn(p, array); e != kPrJsonMissing; e = nextIn(p, array, e), ++n) {
        LottieLayer layer{};
        double ty = number(p, e, "ty", -1);
        if (ty == 0)
            layer.kind = LottieLayerKind_Precomp;
        else if (ty == 1)
            layer.kind = LottieLayerKind_Solid;
        else if (ty == 2)
            layer.kind = LottieLayerKind_Image;
        else if (ty == 3)
            layer.kind = LottieLayerKind_Null;
        else if (ty == 4)
            layer.kind = LottieLayerKind_Shape;
        else if (ty == 5)
            layer.kind = LottieLayerKind_Text;
        else if (ty == 13)
            layer.kind = LottieLayerKind_Camera;
        else
            layer.kind = LottieLayerKind_Other;
        layer.blend = blendOf(number(p, e, "bm", 0), &layer.additive);
        double tt = number(p, e, "tt", 0);
        layer.matte = tt >= 1 && tt <= 4 ? LottieMatte(tt) : LottieMatte_None;
        layer.matteSource = flag(p, e, "td");
        layer.hidden = flag(p, e, "hd");
        layer.threeD = flag(p, e, "ddd");
        layer.autoOrient = flag(p, e, "ao");
        layer.inPoint = number(p, e, "ip", 0);
        layer.outPoint = number(p, e, "op", 0);
        layer.startTime = number(p, e, "st", 0);
        layer.stretch = number(p, e, "sr", 1);
        if (layer.stretch == 0)
            layer.stretch = 1;
        layer.id = integer(p, e, "ind", n + 1);
        // The layer's node, which contains its groups and properties.
        uint32_t compNode = p.parent;
        layer.node = addNode(p, LottieNodeKind_Layer, LottieRole_None, e);
        if (layer.node != kLottieNone) {
            p.a->nodes.data[layer.node].layer = comp.layers + n;
            p.a->nodes.data[layer.node].comp = compNode != kLottieNone ? p.a->nodes.data[compNode].comp : kLottieNone;
            p.parent = layer.node;
        }
        layer.transform = readTransform(p, member(p, e, "ks"));
        layer.zoom = layer.kind == LottieLayerKind_Camera ? readScalar(p, e, "pe", 0) : kLottieNone;
        layer.first = 0;
        layer.count = 0;
        if (layer.kind == LottieLayerKind_Shape) {
            uint32_t contents = addNode(p, LottieNodeKind_Group, LottieRole_Contents, kPrJsonMissing);
            uint32_t outer = p.parent;
            if (contents != kLottieNone)
                p.parent = contents;
            readItems(p, member(p, e, "shapes"), &layer.first, &layer.count);
            p.parent = outer;
        }
        if (layer.kind == LottieLayerKind_Text)
            readText(p, member(p, e, "t"), layer);
        readMasks(p, member(p, e, "masksProperties"), layer);
        readEffects(p, member(p, e, "ef"), layer);
        layer.comp = kLottieNone;
        layer.timeRemap =
            attach(p, readProperty(p, member(p, e, "tm"), ValueKind_Numbers, 1), LottieRole_TimeRemap, member(p, e, "tm"));
        p.parent = compNode;
        layer.width = float(number(p, e, "w", 0));
        layer.height = float(number(p, e, "h", 0));
        if (layer.kind == LottieLayerKind_Solid) {
            layer.width = float(number(p, e, "sw", 0));
            layer.height = float(number(p, e, "sh", 0));
            layer.color = hexColor(text(p, e, "sc"));
        }
        if (layer.kind == LottieLayerKind_Precomp || layer.kind == LottieLayerKind_Image)
            refs.push(member(p, e, "refId"));
        else
            refs.push(kPrJsonMissing);
        ids.push(number(p, e, "ind", -1e300));
        parents.push(number(p, e, "parent", -1e300));
        mattes.push(number(p, e, "tp", -1e300));
        p.a->layers.data[comp.layers + n] = layer;
    }
    // The layers sorted by id, with the first layer of each id first. A parent's id finds its layer here.
    RawArray<LayerId> byId;
    byId.reserve(comp.count);
    for (uint32_t j = 0; j < comp.count; ++j)
        byId.push(LayerId{ids.data[j], j});
    sortItems(byId.data, byId.count,
        [](const LayerId& a, const LayerId& b) { return a.id < b.id || (a.id == b.id && a.layer < b.layer); });
    auto find = [&](double id) {
        uint32_t at = lowerBound(byId.data, byId.count, id);
        return id != -1e300 && at < byId.count && byId.data[at].id == id ? byId.data[at].layer : kLottieNone;
    };
    for (uint32_t j = 0; j < comp.count; ++j) {
        LottieLayer& layer = p.a->layers.data[comp.layers + j];
        layer.parent = find(parents.data[j]);
        if (layer.parent == j)
            layer.parent = kLottieNone;
        layer.matteLayer = kLottieNone;
        if (layer.matte != LottieMatte_None)
            layer.matteLayer = mattes.data[j] != -1e300 ? find(mattes.data[j]) : j > 0 ? j - 1 : kLottieNone;
    }
    byId.reset();
    ids.reset();
    parents.reset();
    mattes.reset();
}

// An asset's id and its index, which sort by id and then by index.
struct AssetId {
    PrStringView id;
    uint32_t index;

    bool operator<(PrStringView other) const { return prStringCompare(id, other) < 0; }
};

// Returns the ids from `first` on, with their indexes, sorted.
RawArray<AssetId> sortedIds(const RawArray<PrStringView>& ids, uint32_t first) {
    RawArray<AssetId> sorted;
    for (uint32_t i = first; i < ids.count; ++i)
        sorted.push(AssetId{ids.data[i], i});
    sortItems(sorted.data, sorted.count, [](const AssetId& a, const AssetId& b) {
        int order = prStringCompare(a.id, b.id);
        return order < 0 || (order == 0 && a.index < b.index);
    });
    return sorted;
}

// Returns the index of the first asset with the id, or kLottieNone if there is none.
uint32_t findId(const RawArray<AssetId>& sorted, PrStringView id) {
    uint32_t at = lowerBound(sorted.data, sorted.count, id);
    return at < sorted.count && sorted.data[at].id == id ? sorted.data[at].index : kLottieNone;
}

} // namespace

// Creates a composition's node, which is a root.
uint32_t compNode(Parser& p, uint32_t object, uint32_t comp) {
    LottieNode node{};
    node.kind = LottieNodeKind_Comp;
    node.name = keep(p, text(p, object, "nm"));
    node.parent = kLottieNone;
    node.firstChild = kLottieNone;
    node.lastChild = kLottieNone;
    node.nextSibling = kLottieNone;
    node.property = kLottieNone;
    node.layer = kLottieNone;
    node.comp = comp;
    p.a->nodes.push(node);
    return p.a->nodes.count - 1;
}

// Reads the image asset's pixels from its `p` as a data URL, or from the file that it names by `u` then `p`.
const PrDrawImage* PR_NULLABLE readImage(const Parser& p, uint32_t asset, const PrLottieOptions& options) {
    PrStringView path = text(p, asset, "p");
    if (path.data == nullptr || options.codec == nullptr)
        return nullptr;
    const PrDrawImage* image = nullptr;
    if (prStringStartsWith(path, "data:")) {
        size_t comma = prStringFindByte(path, ',');
        if (comma == path.size || !prStringContains(prStringSlice(path, 0, comma), ";base64"))
            return nullptr;
        PrStringView encoded = prStringSlice(path, comma + 1, path.size - comma - 1);
        size_t size = prBase64DecodedCapacity(encoded.size);
        char* bytes = static_cast<char*>(prMemoryAlloc(size != 0 ? size : 1));
        if (prBase64Decode(encoded, bytes, size, &size))
            prDrawImageCreateFromBytes(options.codec, PrByteView{reinterpret_cast<const uint8_t*>(bytes), size}, &image, nullptr);
        prMemoryFree(bytes);
        return image;
    }
    if (options.load == nullptr)
        return nullptr;
    PrStringView directory = text(p, asset, "u");
    RawArray<char> joined;
    if (directory.data != nullptr && !flag(p, asset, "e"))
        joined.append(directory.data, uint32_t(directory.size));
    joined.append(path.data, uint32_t(path.size));
    PrByteView bytes{};
    if (options.load(options.loadContext, PrStringView{joined.data, joined.count}, &bytes))
        prDrawImageCreateFromBytes(options.codec, bytes, &image, nullptr);
    joined.reset();
    return image;
}

PrStatus lottieParse(PrStringView json, const PrLottieOptions& options, LottieAnimation& a) {
    Parser p{};
    p.a = &a;
    p.closed = -1;
    p.parent = kLottieNone;
    p.doc = prJsonCreate();
    if (!prJsonParse(p.doc, json, nullptr)) {
        prJsonDestroy(p.doc);
        return PrStatus_Syntax;
    }
    uint32_t root = 0;
    uint32_t layers = member(p, root, "layers");
    if (kindOf(p, root) != PrJsonKind_Object || kindOf(p, layers) != PrJsonKind_Array) {
        prJsonDestroy(p.doc);
        return PrStatus_InvalidData;
    }
    PrStringView version = text(p, root, "v");
    p.colorsOf255 = before(version, 4, 1, 9);
    p.closedOnItem = before(version, 4, 4, 18);
    a.info.extent.x = float(number(p, root, "w", 0));
    a.info.extent.y = float(number(p, root, "h", 0));
    a.info.frameRate = number(p, root, "fr", 30);
    a.info.inPoint = number(p, root, "ip", 0);
    a.info.outPoint = number(p, root, "op", 0);

    // The fonts and glyphs that text is set in, which documents refer to.
    readFonts(p, member(p, root, "fonts"));
    readCharShapes(p, member(p, root, "chars"));

    // The root composition, then each asset that is a composition. Layers refer to these by id.
    RawArray<uint32_t> refs;
    RawArray<PrStringView> ids;
    uint32_t rootComp = a.comps.count;
    a.comps.emplace();
    LottieComp comp{};
    comp.width = a.info.extent.x;
    comp.height = a.info.extent.y;
    comp.node = compNode(p, root, rootComp);
    p.parent = comp.node;
    readLayers(p, layers, comp, refs);
    p.parent = kLottieNone;
    a.comps.data[rootComp] = comp;
    ids.push(PrStringView{});
    uint32_t assets = member(p, root, "assets");
    RawArray<PrStringView> imageIds;
    for (uint32_t e = firstIn(p, assets); e != kPrJsonMissing; e = nextIn(p, assets, e)) {
        uint32_t assetLayers = member(p, e, "layers");
        if (kindOf(p, assetLayers) != PrJsonKind_Array) {
            if (member(p, e, "p") == kPrJsonMissing)
                continue;
            LottieImage image{float(number(p, e, "w", 0)), float(number(p, e, "h", 0)), readImage(p, e, options)};
            a.images.push(image);
            imageIds.push(text(p, e, "id"));
            continue;
        }
        uint32_t index = a.comps.count;
        a.comps.emplace();
        LottieComp asset{};
        asset.width = float(number(p, e, "w", 0));
        asset.height = float(number(p, e, "h", 0));
        asset.node = compNode(p, e, index);
        p.parent = asset.node;
        readLayers(p, assetLayers, asset, refs);
        p.parent = kLottieNone;
        a.comps.data[index] = asset;
        ids.push(text(p, e, "id"));
    }
    // Every layer was read in order, so `refs` is parallel to the layers.
    RawArray<AssetId> images = sortedIds(imageIds, 0);
    RawArray<AssetId> comps = sortedIds(ids, 1);
    for (uint32_t i = 0; i < a.layers.count; ++i) {
        PrStringView ref = prJsonString(p.doc, refs.data[i]);
        if (refs.data[i] == kPrJsonMissing || ref.data == nullptr)
            continue;
        LottieLayer& layer = a.layers.data[i];
        uint32_t found = findId(layer.kind == LottieLayerKind_Image ? images : comps, ref);
        if (found != kLottieNone)
            layer.comp = found;
    }
    images.reset();
    comps.reset();
    refs.reset();
    ids.reset();
    imageIds.reset();
    lottieLayoutText(a, options);
    p.scratch.reset();
    prJsonDestroy(p.doc);
    return PrStatus_Ok;
}

void lottieReset(LottieAnimation& a) {
    lottieExpressionsDestroy(a.expressions);
    a.expressions = nullptr;
    for (uint32_t i = 0; i < a.images.count; ++i)
        prDrawImageRelease(a.images.data[i].image);
    a.images.reset();
    a.comps.reset();
    a.layers.reset();
    a.items.reset();
    a.transforms.reset();
    a.dashes.reset();
    a.masks.reset();
    for (uint32_t i = 0; i < a.fonts.count; ++i)
        prFontRelease(a.fonts.data[i]);
    a.fonts.reset();
    a.strings.reset();
    a.textFonts.reset();
    a.charShapes.reset();
    a.documents.reset();
    a.animators.reset();
    a.letters.reset();
    a.letterIndexes.reset();
    a.textGlyphs.reset();
    a.textTransforms.reset();
    a.nodes.reset();
    a.effects.reset();
    a.effectValues.reset();
    a.properties.reset();
    a.keyframes.reset();
    a.numbers.reset();
}

} // namespace pr_impl
