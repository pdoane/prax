#include "pr/number_list.h"

#include "pr/ascii.h"
#include "pr/float_convert.h"

namespace pr {

namespace {

bool at(PrStringView text, size_t pos, char c) {
    return pos < text.size && text.data[pos] == c;
}

} // namespace

bool numberListRead(PrStringView text, size_t* pos, float* out) {
    size_t p = *pos;
    numberListSkipSpace(text, &p);
    size_t start = p;
    if (at(text, p, '+') || at(text, p, '-'))
        ++p;
    size_t digits = p;
    while (p < text.size && prAsciiIsDigit(text.data[p]))
        ++p;
    bool whole = p > digits;
    bool fraction = false;
    if (at(text, p, '.') && p + 1 < text.size && prAsciiIsDigit(text.data[p + 1])) {
        ++p;
        while (p < text.size && prAsciiIsDigit(text.data[p]))
            ++p;
        fraction = true;
    }
    if (!whole && !fraction)
        return false;
    if (at(text, p, 'e') || at(text, p, 'E')) {
        size_t e = p + 1;
        if (at(text, e, '+') || at(text, e, '-'))
            ++e;
        if (e < text.size && prAsciiIsDigit(text.data[e])) {
            p = e;
            while (p < text.size && prAsciiIsDigit(text.data[p]))
                ++p;
        }
    }
    double value = 0;
    if (!prF64ParsePrefix(PrStringView{text.data + start, p - start}, &value))
        return false;
    *out = float(value);
    *pos = p;
    return true;
}

bool numberListReadArray(PrStringView text, size_t* pos, float* out, uint32_t count) {
    size_t start = *pos;
    for (uint32_t i = 0; i < count; ++i) {
        if (i != 0)
            numberListSkipSeparator(text, pos);
        if (!numberListRead(text, pos, &out[i])) {
            *pos = start;
            return false;
        }
    }
    return true;
}

void numberListSkipSeparator(PrStringView text, size_t* pos) {
    numberListSkipSpace(text, pos);
    if (at(text, *pos, ',')) {
        ++*pos;
        numberListSkipSpace(text, pos);
    }
}

void numberListSkipSpace(PrStringView text, size_t* pos) {
    while (*pos < text.size && prAsciiIsSpace(text.data[*pos]))
        ++*pos;
}

} // namespace pr
