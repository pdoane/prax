#include "pr/float.h"

#include "pr/assert.h"
#include "pr/bit_cast.h"
#include "pr/float_convert.h"
#include "pr/integer.h"
#include "pr/mem.h"

namespace pr_impl {
namespace {

size_t copyLiteral(const char* text, char* out) {
    size_t size = 0;
    while (text[size] != '\0') {
        out[size] = text[size];
        ++size;
    }
    return size;
}

// Writes `decimal`, which must be positive, in the notation that prF64Format describes.
size_t formatDecimal(PrF64Decimal decimal, char* out) {
    char digits[kPrU64TextMaxBytes];
    int32_t count = int32_t(prU64Format(decimal.digits, digits, 10));

    // Writes `count` digits with the decimal point after position `point`.
    size_t length = 0;
    int32_t point = decimal.exponent + count;
    if (point >= count && point <= kPrF64PositionalMaxExponent) {
        prMemCopy(out, digits, size_t(count));
        length += size_t(count);
        for (int32_t i = 0; i < point - count; ++i)
            out[length++] = '0';
    } else if (point > 0 && point <= kPrF64PositionalMaxExponent) {
        prMemCopy(out, digits, size_t(point));
        length += size_t(point);
        out[length++] = '.';
        prMemCopy(out + length, digits + point, size_t(count - point));
        length += size_t(count - point);
    } else if (point > kPrF64PositionalMinExponent && point <= 0) {
        out[length++] = '0';
        out[length++] = '.';
        for (int32_t i = 0; i < -point; ++i)
            out[length++] = '0';
        prMemCopy(out + length, digits, size_t(count));
        length += size_t(count);
    } else {
        out[length++] = digits[0];
        if (count > 1) {
            out[length++] = '.';
            prMemCopy(out + length, digits + 1, size_t(count - 1));
            length += size_t(count - 1);
        }
        out[length++] = 'e';
        int32_t exponent = point - 1;
        out[length++] = exponent < 0 ? '-' : '+';
        length += prU64Format(uint64_t(exponent < 0 ? -exponent : exponent), out + length, 10);
    }
    return length;
}

// Writes the sign of `*value`, and writes the values that need no digits: NaN, infinities and zero. Makes `*value`
// positive. Returns true if digits remain to be written.
bool formatSpecial(double* value, char* out, size_t* length) {
    *length = 0;
    if (prF64IsNan(*value)) {
        *length = copyLiteral("NaN", out);
        return false;
    }
    if (*value < 0) {
        out[(*length)++] = '-';
        *value = -*value;
    }
    if (!prF64IsFinite(*value)) {
        *length += copyLiteral("Infinity", out + *length);
        return false;
    }
    if (*value == 0) {
        *length = 0;
        out[(*length)++] = '0';
        return false;
    }
    return true;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

double prF64Parse(const char* data, size_t size) {
    double value = 0;
    bool parsed = prF64ParsePrefix({data, size}, &value);
    PR_ASSERT(parsed);
    return value;
}

size_t prF64Format(double value, char* out) {
    size_t length = 0;
    if (!formatSpecial(&value, out, &length))
        return length;

    // An integer below 10^15 is already its shortest text, written with no point and no exponent.
    if (value < 1e15) {
        uint64_t units = uint64_t(value);
        if (double(units) == value)
            return length + prU64Format(units, out + length, 10);
    }

    length += formatDecimal(prF64ShortestDecimal(value), out + length);
    PR_ASSERT(length <= kPrF64TextMaxBytes);
    return length;
}

size_t prF64FormatSignificant(double value, uint32_t digits, char* out) {
    PR_ASSERT(digits >= 1 && digits <= kPrF64MaxDigits);
    size_t length = 0;
    if (!formatSpecial(&value, out, &length))
        return length;

    PrF64Decimal decimal = prF64ShortestDecimal(value);
    uint64_t limit = 1;
    for (uint32_t i = 0; i < digits; ++i)
        limit *= 10;
    if (decimal.digits >= limit) {
        uint64_t scale = 1;
        while (decimal.digits / scale >= limit) {
            scale *= 10;
            ++decimal.exponent;
        }
        uint64_t rest = decimal.digits % scale;
        decimal.digits = decimal.digits / scale + (rest * 2 >= scale ? 1 : 0);
        while (decimal.digits % 10 == 0) {
            decimal.digits /= 10;
            ++decimal.exponent;
        }
    }

    length += formatDecimal(decimal, out + length);
    PR_ASSERT(length <= kPrF64TextMaxBytes);
    return length;
}

size_t prF64FormatFixed(double value, uint32_t decimals, char* out) {
    PR_ASSERT(prF64IsFinite(value) && prF64Abs(value) < 1e21 && decimals <= kPrF64MaxDigits);
    if (value == 0) {
        out[0] = '0';
        return 1;
    }
    // The shortest decimal, digits * 10^exponent, rounded at the 10^-decimals place.
    PrF64Decimal decimal = prF64ShortestDecimal(value);
    int32_t place = -int32_t(decimals);
    if (decimal.exponent < place) {
        uint32_t dropped = uint32_t(place - decimal.exponent);
        // The digits are below 10^kPrF64MaxDigits, so past that many places they round to zero.
        if (dropped > kPrF64MaxDigits) {
            decimal.digits = 0;
        } else {
            uint64_t scale = 1;
            for (uint32_t i = 0; i < dropped; ++i)
                scale *= 10;
            uint64_t rest = decimal.digits % scale;
            decimal.digits = decimal.digits / scale + (rest * 2 >= scale ? 1 : 0);
        }
        decimal.exponent = place;
        while (decimal.digits != 0 && decimal.digits % 10 == 0) {
            decimal.digits /= 10;
            ++decimal.exponent;
        }
    }
    if (decimal.digits == 0) {
        out[0] = '0';
        return 1;
    }
    size_t length = 0;
    if (value < 0)
        out[length++] = '-';
    char digits[kPrU64TextMaxBytes];
    int32_t count = int32_t(prU64Format(decimal.digits, digits, 10));
    if (decimal.exponent >= 0) {
        prMemCopy(out + length, digits, size_t(count));
        length += size_t(count);
        for (int32_t i = 0; i < decimal.exponent; ++i)
            out[length++] = '0';
        return length;
    }
    // The digits before the point, then the zeros and digits after it.
    int32_t point = count + decimal.exponent;
    if (point > 0) {
        prMemCopy(out + length, digits, size_t(point));
        length += size_t(point);
    } else {
        out[length++] = '0';
    }
    out[length++] = '.';
    for (int32_t i = point; i < 0; ++i)
        out[length++] = '0';
    int32_t from = point > 0 ? point : 0;
    prMemCopy(out + length, digits + from, size_t(count - from));
    length += size_t(count - from);
    PR_ASSERT(length <= kPrF64FixedMaxBytes);
    return length;
}
