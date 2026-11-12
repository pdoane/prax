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

// Writes `decimal`, which must be positive, in the notation that prF64Format describes. The digits are written in
// place, and a point that falls among them is made room for by moving the digits after it.
size_t formatDecimal(PrF64Decimal decimal, char* out) {
    int32_t count = int32_t(prU64DecimalDigits(decimal.digits));
    int32_t point = decimal.exponent + count;
    if (point >= count && point <= kPrF64PositionalMaxExponent) {
        prU64Format(decimal.digits, out, 10);
        prMemFill(out + count, '0', size_t(point - count));
        return size_t(point);
    }
    if (point > 0 && point <= kPrF64PositionalMaxExponent) {
        prU64Format(decimal.digits, out, 10);
        prMemMove(out + point + 1, out + point, size_t(count - point));
        out[point] = '.';
        return size_t(count) + 1;
    }
    if (point > kPrF64PositionalMinExponent && point <= 0) {
        out[0] = '0';
        out[1] = '.';
        prMemFill(out + 2, '0', size_t(-point));
        prU64Format(decimal.digits, out + 2 - point, 10);
        return size_t(2 - point + count);
    }
    // The digits after the first go one place to the right, past the point.
    prU64Format(decimal.digits, out + 1, 10);
    size_t length = 1;
    out[0] = out[1];
    if (count > 1) {
        out[1] = '.';
        length = size_t(count) + 1;
    }
    out[length++] = 'e';
    int32_t exponent = point - 1;
    out[length++] = exponent < 0 ? '-' : '+';
    length += prU64Format(uint64_t(exponent < 0 ? -exponent : exponent), out + length, 10);
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

size_t prF32Format(float value, char* out) {
    double wide = double(value);
    size_t length = 0;
    if (!formatSpecial(&wide, out, &length))
        return length;
    // Widening is exact, so the magnitude narrows back to the same float. Of every float, one magnitude has a shortest
    // decimal that a double holds past the midpoint to its neighbor, so that rounding the double to a float gives the
    // neighbor: 7.038531e-26. It takes a digit more.
    float magnitude = float(wide);
    constexpr uint32_t kDoubleRounded = 0x15ae43fdu;
    PrF64Decimal decimal = PrF64Decimal{70385307, -33};
    if (bitCast<uint32_t>(magnitude) != kDoubleRounded)
        decimal = prF32ShortestDecimal(magnitude);
    length += formatDecimal(decimal, out + length);
    PR_ASSERT(length <= kPrF64TextMaxBytes);
    return length;
}

size_t prF64FormatSignificant(double value, uint32_t digits, char* out) {
    PR_ASSERT(digits >= 1 && digits <= kPrF64MaxDigits);
    size_t length = 0;
    if (!formatSpecial(&value, out, &length))
        return length;

    PrF64Decimal decimal = prF64ShortestDecimal(value);
    uint32_t count = prU64DecimalDigits(decimal.digits);
    if (count > digits) {
        uint32_t dropped = count - digits;
        uint64_t scale = prU64Pow10(dropped);
        uint64_t rest = decimal.digits % scale;
        decimal.digits = decimal.digits / scale + (rest * 2 >= scale ? 1 : 0);
        decimal.exponent += int32_t(dropped);
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
