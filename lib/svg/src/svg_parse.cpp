// SVG's own grammars.

#include "svg/src/svg_parse.h"

#include "pr/ascii.h"
#include "pr/float.h"
#include "pr/number_list.h"
#include "pr/string_ops.h"

#include "svg/src/svg_attribute.h"
#include "svg/src/svg_types.h"

namespace pr_impl {

namespace {

void skipSpace(PrStringView text, size_t* pos) {
    while (*pos < text.size && prAsciiIsSpace(text.data[*pos]))
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

bool svgParseAspect(PrStringView text, PrSvgPreserveAspectRatio* out) {
    *out = kSvgAspectDefault;
    PrStringView words[4] = {};
    uint32_t count = 0;
    size_t at = 0;
    while (text.data != nullptr && at < text.size) {
        skipSpace(text, &at);
        size_t start = at;
        while (at < text.size && !prAsciiIsSpace(text.data[at]))
            ++at;
        if (at == start)
            break;
        if (count == 4)
            return false;
        words[count++] = PrStringView{text.data + start, at - start};
    }
    uint32_t first = count != 0 && words[0] == "defer" ? 1 : 0;
    if (first >= count || count - first > 2)
        return false;
    PrSvgPreserveAspectRatio read = {svgKeywordValue(SvgKeywords_Align, words[first]), PrSvgMeetOrSlice_Meet};
    if (first + 1 < count)
        read.meetOrSlice = svgKeywordValue(SvgKeywords_MeetOrSlice, words[first + 1]);
    if (read.align == 0 || read.meetOrSlice == 0)
        return false;
    *out = read;
    return true;
}

PrAffine2 svgViewBoxTransform(PrRect viewBox, PrSvgPreserveAspectRatio aspect, float width, float height) {
    float vw = prRectWidth(viewBox);
    float vh = prRectHeight(viewBox);
    if (vw <= 0 || vh <= 0)
        return prAffine2Identity();
    float sx = width / vw;
    float sy = height / vh;
    if (aspect.align != PrSvgAlign_None) {
        float s = aspect.meetOrSlice == PrSvgMeetOrSlice_Slice ? prF32Max(sx, sy) : prF32Min(sx, sy);
        sx = sy = s;
    }
    // The alignments from xMinYMin are x's min, mid and max for each of y's; none places at the minimum.
    uint32_t index = aspect.align > PrSvgAlign_None ? uint32_t(aspect.align - PrSvgAlign_XMinYMin) : 0;
    float tx = -viewBox.left * sx + float(index % 3) * (width - vw * sx) / 2;
    float ty = -viewBox.top * sy + float(index / 3) * (height - vh * sy) / 2;
    return PrAffine2{sx, 0, 0, sy, tx, ty};
}

bool svgParseTransform(PrStringView text, PrAffine2* out) {
    RawArray<PrSvgTransform> list;
    bool ok = svgParseTransformList(text, &list) && list.count != 0;
    PrAffine2 transform = prAffine2Identity();
    for (uint32_t i = 0; ok && i < list.count; ++i) {
        PrDomMatrix m = prSvgTransformMatrix(&list.data[i]);
        PrAffine2 f{float(m.m[0]), float(m.m[1]), float(m.m[4]), float(m.m[5]), float(m.m[12]), float(m.m[13])};
        // The function applies inside the ones before it.
        transform = prAffine2Concat(f, transform);
    }
    list.reset();
    if (ok)
        *out = transform;
    return ok;
}

bool svgParseNumbers(PrStringView text, RawArray<float>* out) {
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
