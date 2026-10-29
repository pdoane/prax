// SVG's own grammars.

#include "svg/src/svg_parse.h"

#include "pr/ascii.h"
#include "pr/float.h"
#include "pr/number_list.h"
#include "pr/string_ops.h"

namespace pr_impl {

namespace {

void skipSpace(PrStringView text, size_t* pos) {
    while (*pos < text.size && asciiIsSpace(text.data[*pos]))
        ++*pos;
}

} // namespace

bool svgParseViewBox(PrStringView text, PrRect* out) {
    size_t pos = 0;
    float n[4];
    if (!numberListReadArray(text, &pos, n, 4))
        return false;
    skipSpace(text, &pos);
    if (pos != text.size || n[2] < 0 || n[3] < 0)
        return false;
    *out = prRectAt(PrVec2{n[0], n[1]}, PrVec2{n[2], n[3]});
    return true;
}

bool svgParseAspect(PrStringView text, SvgAspect* out) {
    *out = SvgAspect{};
    size_t pos = 0;
    skipSpace(text, &pos);
    auto word = [&](PrStringView expected) {
        if (pos + expected.size > text.size || PrStringView{text.data + pos, expected.size} != expected)
            return false;
        pos += expected.size;
        return true;
    };
    SvgAspect aspect;
    if (word("none")) {
        aspect.x = aspect.y = SvgAlign_None;
    } else {
        SvgAlign x = SvgAlign_None;
        if (word("xMin"))
            x = SvgAlign_Min;
        else if (word("xMid"))
            x = SvgAlign_Mid;
        else if (word("xMax"))
            x = SvgAlign_Max;
        else
            return false;
        SvgAlign y = SvgAlign_None;
        if (word("YMin"))
            y = SvgAlign_Min;
        else if (word("YMid"))
            y = SvgAlign_Mid;
        else if (word("YMax"))
            y = SvgAlign_Max;
        else
            return false;
        aspect.x = x;
        aspect.y = y;
    }
    skipSpace(text, &pos);
    if (word("slice"))
        aspect.slice = true;
    else
        word("meet");
    skipSpace(text, &pos);
    if (pos != text.size)
        return false;
    *out = aspect;
    return true;
}

PrAffine2 svgViewBoxTransform(PrRect viewBox, SvgAspect aspect, float width, float height) {
    float vw = prRectWidth(viewBox);
    float vh = prRectHeight(viewBox);
    if (vw <= 0 || vh <= 0)
        return prAffine2Identity();
    float sx = width / vw;
    float sy = height / vh;
    if (aspect.x != SvgAlign_None) {
        float s = aspect.slice ? maxF32(sx, sy) : minF32(sx, sy);
        sx = sy = s;
    }
    float tx = -viewBox.left * sx;
    float ty = -viewBox.top * sy;
    auto shift = [](SvgAlign align, float room) {
        return align == SvgAlign_Mid ? room / 2 : align == SvgAlign_Max ? room : 0;
    };
    tx += shift(aspect.x, width - vw * sx);
    ty += shift(aspect.y, height - vh * sy);
    return PrAffine2{sx, 0, 0, sy, tx, ty};
}

bool svgParseTransform(PrStringView text, PrAffine2* out) {
    PrAffine2 transform = prAffine2Identity();
    size_t pos = 0;
    bool any = false;
    for (;;) {
        while (pos < text.size && (asciiIsSpace(text.data[pos]) || text.data[pos] == ','))
            ++pos;
        if (pos == text.size)
            break;
        size_t name = pos;
        while (pos < text.size && ((text.data[pos] | 0x20) >= 'a' && (text.data[pos] | 0x20) <= 'z'))
            ++pos;
        PrStringView function{text.data + name, pos - name};
        while (pos < text.size && asciiIsSpace(text.data[pos]))
            ++pos;
        if (pos == text.size || text.data[pos] != '(')
            return false;
        ++pos;
        float n[6] = {};
        uint32_t count = 0;
        for (;;) {
            while (pos < text.size && (asciiIsSpace(text.data[pos]) || (count != 0 && text.data[pos] == ',')))
                ++pos;
            if (pos < text.size && text.data[pos] == ')')
                break;
            if (count == 6 || !numberListRead(text, &pos, &n[count]))
                return false;
            ++count;
        }
        ++pos;
        constexpr float kRadians = kPiF32 / 180;
        PrAffine2 f{};
        if (function == "matrix" && count == 6)
            f = PrAffine2{n[0], n[1], n[2], n[3], n[4], n[5]};
        else if (function == "translate" && (count == 1 || count == 2))
            f = prAffine2Translate(PrVec2{n[0], count == 2 ? n[1] : 0});
        else if (function == "scale" && (count == 1 || count == 2))
            f = prAffine2Scale(PrVec2{n[0], count == 2 ? n[1] : n[0]});
        else if (function == "rotate" && (count == 1 || count == 3))
            f = prAffine2Concat(prAffine2Concat(prAffine2Translate(PrVec2{-n[1], -n[2]}),
                                                prAffine2Rotate(n[0] * kRadians)),
                                prAffine2Translate(PrVec2{n[1], n[2]}));
        else if (function == "skewX" && count == 1)
            f = PrAffine2{1, 0, tanF32(n[0] * kRadians), 1, 0, 0};
        else if (function == "skewY" && count == 1)
            f = PrAffine2{1, tanF32(n[0] * kRadians), 0, 1, 0, 0};
        else
            return false;
        // The function applies inside the ones before it.
        transform = prAffine2Concat(f, transform);
        any = true;
    }
    if (any)
        *out = transform;
    return any;
}

bool svgParseNumbers(PrStringView text, Array<float>* out) {
    out->clear();
    size_t pos = 0;
    for (;;) {
        if (out->count != 0)
            numberListSkipSeparator(text, &pos);
        skipSpace(text, &pos);
        if (pos == text.size)
            return true;
        float value = 0;
        if (!numberListRead(text, &pos, &value))
            return false;
        out->push(value);
    }
}

} // namespace pr_impl
