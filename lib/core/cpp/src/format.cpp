// Formatting: text made from a format string and arguments, written to a Writer.

#include "pr/format.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/bit_cast.h"
#include "pr/count_of.h"
#include "pr/float_convert.h"
#include "pr/integer.h"
#include "pr/string_ops.h"

namespace pr {
namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Specs

bool isAlign(char c) {
    return c == '<' || c == '>' || c == '^';
}

bool isSign(char c) {
    return c == '+' || c == '-' || c == ' ';
}

bool isType(char c) {
    switch (c) {
        case 'd':
        case 'x':
        case 'X':
        case 'o':
        case 'b':
        case 'B':
        case 'c':
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 's':
        case 'p':
            return true;
        default:
            return false;
    }
}

bool isIntegerType(char c) {
    return c == 'd' || c == 'x' || c == 'X' || c == 'o' || c == 'b' || c == 'B';
}

// The arguments of a format string, and the next one that `{}` takes.
struct ArgumentCursor {
    Span<const FormatArgument> list;
    uint32_t next;
};

// Parses the decimal digits at `*at` in `text`, and advances `*at` past them.
uint32_t digitsAt(PrStringView text, size_t* at) {
    uint64_t value = 0;
    while (*at < text.size && prAsciiIsDigit(text.data[*at])) {
        value = value * 10 + uint64_t(text.data[*at] - '0');
        if (value > 0xffffffffu)
            value = 0xffffffffu;
        ++*at;
    }
    return uint32_t(value);
}

// Parses a width or precision at `*at`: digits, or a `{}` or `{N}` that names an integer argument. Returns false if
// there is neither.
bool countAt(PrStringView text, size_t* at, ArgumentCursor* cursor, uint32_t* out) {
    if (*at < text.size && prAsciiIsDigit(text.data[*at])) {
        *out = digitsAt(text, at);
        return true;
    }
    if (*at >= text.size || text.data[*at] != '{')
        return false;
    ++*at;
    uint32_t which = 0;
    if (*at < text.size && prAsciiIsDigit(text.data[*at]))
        which = digitsAt(text, at);
    else
        which = cursor->next++;
    bool closed = *at < text.size && text.data[*at] == '}';
    PR_ASSERT(closed && "a nested field closes with }");
    if (closed)
        ++*at;
    int64_t value = 0;
    bool found = which < cursor->list.count() && cursor->list[which].integer(cursor->list[which].value, &value);
    PR_ASSERT(found && "a nested field names an integer argument");
    if (!found || value < 0)
        value = 0;
    *out = value > 0xffffffff ? 0xffffffffu : uint32_t(value);
    return true;
}

// Parses the spec in `text`, which is what follows a field's colon.
FormatSpec parseSpec(PrStringView text, ArgumentCursor* cursor) {
    FormatSpec spec;
    size_t at = 0;
    if (text.size >= 2 && isAlign(text.data[1])) {
        spec.fill = text.data[0];
        spec.align = text.data[1];
        at = 2;
    } else if (text.size >= 1 && isAlign(text.data[0])) {
        spec.align = text.data[0];
        at = 1;
    }
    if (at < text.size && isSign(text.data[at]))
        spec.sign = text.data[at++];
    if (at < text.size && text.data[at] == '#') {
        spec.alternate = true;
        ++at;
    }
    // A zero before the width pads with zeros after the sign, unless an align places the padding.
    if (at < text.size && text.data[at] == '0') {
        if (spec.align == '\0') {
            spec.fill = '0';
            spec.align = '=';
        }
        ++at;
    }
    uint32_t count = 0;
    if (countAt(text, &at, cursor, &count))
        spec.width = count;
    if (at < text.size && text.data[at] == '.') {
        ++at;
        spec.precision = countAt(text, &at, cursor, &count) ? int32_t(count > 0x7fffffffu ? 0x7fffffffu : count) : 0;
    }
    if (at < text.size && isType(text.data[at]))
        spec.type = text.data[at++];
    PR_ASSERT(at == text.size && "a spec is [[fill]align][sign][#][0][width][.precision][type]");
    return spec;
}

// The alignment of a field: its spec's, or `fallback`. Zero padding is right alignment for what has no sign.
char alignOf(const FormatSpec& spec, char fallback) {
    if (spec.align == '\0')
        return fallback;
    return spec.align == '=' ? '>' : spec.align;
}

// Returns the sign that `spec` asks for, or '\0' for none.
char signOf(bool negative, const FormatSpec& spec) {
    if (negative)
        return '-';
    return spec.sign == '+' || spec.sign == ' ' ? spec.sign : '\0';
}

// Writes `sign` and `prefix`, then the body of `bodySize` bytes that `body` writes, padded as `spec` says. Zero padding
// goes between the prefix and the body, and any other padding outside them.
template <typename Body>
void writeNumber(Writer& writer, char sign, PrStringView prefix, size_t bodySize, const FormatSpec& spec, Body body) {
    size_t size = (sign != '\0' ? 1 : 0) + prefix.size + bodySize;
    uint32_t padding = spec.width > size ? spec.width - uint32_t(size) : 0;
    if (spec.align == '=') {
        if (sign != '\0')
            writer.writeChar(sign);
        writer.write(prefix);
        writer.writeRepeated('0', padding);
        body();
        return;
    }
    char align = alignOf(spec, '>');
    uint32_t before = padding;
    if (align == '<')
        before = 0;
    else if (align == '^')
        before = padding / 2;
    writer.writeRepeated(spec.fill, before);
    if (sign != '\0')
        writer.writeChar(sign);
    writer.write(prefix);
    body();
    writer.writeRepeated(spec.fill, padding - before);
}

// ---------------------------------------------------------------------------------------------------------------------
// Floats
//
// The digits come from core's exact conversions (pr/float_convert.h). This layer places the point and the exponent, and
// pads the field. A run of digits stands for |value| = 0.<digits> * 10^exponent, and the places that the run does not
// reach are zeros.

// The most digits that a rounded conversion writes, and a byte that the conversion needs past them.
constexpr size_t kMaxRoundedDigits = kPrF64MaxExactDigits + 1;

size_t fixedSize(int32_t exponent, uint32_t decimals, bool alternate) {
    size_t whole = exponent > 0 ? size_t(exponent) : 1;
    return whole + (decimals > 0 || alternate ? 1 : 0) + decimals;
}

void writeFixed(Writer& writer, const char* digits, size_t count, int32_t exponent, uint32_t decimals, bool alternate) {
    if (exponent > 0) {
        size_t whole = size_t(exponent);
        size_t present = count < whole ? count : whole;
        writer.write(PrStringView{digits, present});
        writer.writeRepeated('0', uint32_t(whole - present));
    } else {
        writer.writeChar('0');
    }
    if (decimals == 0 && !alternate)
        return;
    writer.writeChar('.');

    // The index in the digits of the first place after the point.
    int64_t index = exponent;
    uint32_t remaining = decimals;
    if (index < 0) {
        uint64_t zeros = uint64_t(-index);
        uint32_t run = zeros < remaining ? uint32_t(zeros) : remaining;
        writer.writeRepeated('0', run);
        remaining -= run;
        index += run;
    }
    if (remaining != 0 && index >= 0 && uint64_t(index) < count) {
        size_t available = count - size_t(index);
        size_t run = available < remaining ? available : remaining;
        writer.write(PrStringView{digits + index, run});
        remaining -= uint32_t(run);
    }
    writer.writeRepeated('0', remaining);
}

size_t exponentSize(int32_t exponent) {
    uint32_t magnitude = exponent < 0 ? uint32_t(-exponent) : uint32_t(exponent);
    return magnitude >= 100 ? 3 : 2;
}

size_t scientificSize(uint32_t decimals, int32_t exponent, bool alternate) {
    return 1 + (decimals > 0 || alternate ? 1 : 0) + decimals + 2 + exponentSize(exponent);
}

void writeScientific(Writer& writer, const char* digits, size_t count, uint32_t decimals, int32_t exponent, char mark,
        bool alternate) {
    writer.writeChar(count != 0 ? digits[0] : '0');
    if (decimals != 0 || alternate) {
        writer.writeChar('.');
        size_t available = count > 1 ? count - 1 : 0;
        size_t run = available < decimals ? available : decimals;
        writer.write(PrStringView{digits + 1, run});
        writer.writeRepeated('0', decimals - uint32_t(run));
    }
    writer.writeChar(mark);
    writer.writeChar(exponent < 0 ? '-' : '+');
    uint32_t magnitude = exponent < 0 ? uint32_t(-exponent) : uint32_t(exponent);
    if (magnitude >= 100) {
        writer.writeChar(char('0' + magnitude / 100));
        magnitude %= 100;
    }
    writer.writeChar(char('0' + magnitude / 10));
    writer.writeChar(char('0' + magnitude % 10));
}

// The shortest form keeps a notation of its own, with a bare exponent, so that text written to be read back stays as
// short as the value allows. Exponents below `plainLimit` are written in full.

size_t shortestSize(size_t count, int32_t exponent, int32_t plainLimit) {
    int32_t lead = exponent + int32_t(count) - 1;
    if (exponent >= 0 && lead < plainLimit)
        return count + size_t(exponent);
    if (exponent < 0 && lead >= -4)
        return lead >= 0 ? count + 1 : 2 + size_t(-lead - 1) + count;
    size_t size = count + (count > 1 ? 1 : 0) + 1 + (lead < 0 ? 1 : 0);
    uint32_t magnitude = lead < 0 ? uint32_t(-lead) : uint32_t(lead);
    do {
        ++size;
        magnitude /= 10;
    } while (magnitude != 0);
    return size;
}

void writeShortestBody(Writer& writer, const char* digits, size_t count, int32_t exponent, int32_t plainLimit) {
    int32_t lead = exponent + int32_t(count) - 1;
    if (exponent >= 0 && lead < plainLimit) {
        writer.write(PrStringView{digits, count});
        writer.writeRepeated('0', uint32_t(exponent));
    } else if (exponent < 0 && lead >= 0) {
        size_t whole = size_t(lead) + 1;
        writer.write(PrStringView{digits, whole});
        writer.writeChar('.');
        writer.write(PrStringView{digits + whole, count - whole});
    } else if (exponent < 0 && lead >= -4) {
        writer.write("0.");
        writer.writeRepeated('0', uint32_t(-lead - 1));
        writer.write(PrStringView{digits, count});
    } else {
        writer.writeChar(digits[0]);
        if (count > 1) {
            writer.writeChar('.');
            writer.write(PrStringView{digits + 1, count - 1});
        }
        writer.writeChar('e');
        formatInteger(writer, lead < 0, lead < 0 ? uint64_t(-int64_t(lead)) : uint64_t(lead), FormatSpec{});
    }
}

void writeShortest(Writer& writer, bool negative, PrF64Decimal decimal, int32_t plainLimit, const FormatSpec& spec) {
    char digits[20];
    size_t count = 1;
    if (decimal.digits == 0) {
        digits[0] = '0';
        decimal.exponent = 0;
    } else {
        count = prU64Format(decimal.digits, digits, 10);
    }
    size_t size = shortestSize(count, decimal.exponent, plainLimit);
    writeNumber(writer, signOf(negative, spec), "", size, spec, [&] {
        writeShortestBody(writer, digits, count, decimal.exponent, plainLimit);
    });
}

// Writes an infinity, with its sign, or a NaN, which has no meaningful sign.
void writeNonFinite(Writer& writer, bool negative, bool nan, const FormatSpec& spec) {
    if (nan) {
        writer.writePadded("nan", spec.width, spec.fill == '0' ? ' ' : spec.fill, alignOf(spec, '>'));
        return;
    }
    FormatSpec spaced = spec;
    if (spaced.align == '=') {
        spaced.align = '>';
        spaced.fill = ' ';
    }
    writeNumber(writer, signOf(negative, spec), "", 3, spaced, [&] { writer.write("inf"); });
}

// ---------------------------------------------------------------------------------------------------------------------
// Format strings

// Writes the argument `which` under `spec`, or nothing if there is no such argument.
void writeArgument(Writer& writer, const ArgumentCursor& cursor, uint32_t which, const FormatSpec& spec) {
    bool present = which < cursor.list.count();
    PR_ASSERT(present && "a field names an argument that is there");
    if (present)
        cursor.list[which].format(writer, cursor.list[which].value, spec);
}

// Returns the offset of the brace that closes the field whose spec begins at `at`, past the braces of nested fields, or
// the size of `format` if it does not close.
size_t fieldEnd(PrStringView format, size_t at) {
    uint32_t depth = 0;
    for (; at < format.size; ++at) {
        if (format.data[at] == '{') {
            ++depth;
        } else if (format.data[at] == '}') {
            if (depth == 0)
                return at;
            --depth;
        }
    }
    return format.size;
}

} // namespace

void formatInteger(Writer& writer, bool negative, uint64_t magnitude, const FormatSpec& spec) {
    uint32_t radix = 10;
    PrStringView prefix = "";
    switch (spec.type) {
        case 'c': {
            FormatSpec character{.width = spec.width, .fill = spec.fill, .align = spec.align};
            formatChar(writer, char(magnitude), character);
            return;
        }
        case 'x':
            radix = 16;
            prefix = "0x";
            break;
        case 'X':
            radix = 16;
            prefix = "0X";
            break;
        case 'o':
            radix = 8;
            prefix = "0";
            break;
        case 'b':
            radix = 2;
            prefix = "0b";
            break;
        case 'B':
            radix = 2;
            prefix = "0B";
            break;
        default:
            PR_ASSERT((spec.type == '\0' || spec.type == 'd') && "an integer's type is d, x, X, o, b, B or c");
            break;
    }
    if (!spec.alternate)
        prefix = "";
    char digits[kPrU64TextMaxBytes];
    size_t count = prU64Format(magnitude, digits, radix);
    if (spec.type == 'X')
        prAsciiUpperText(digits, count);
    writeNumber(writer, signOf(negative, spec), prefix, count, spec, [&] { writer.write(PrStringView{digits, count}); });
}

void formatFloat(Writer& writer, float value, const FormatSpec& spec) {
    uint32_t bits = bitCast<uint32_t>(value);
    bool negative = (bits >> 31) != 0;
    if ((bits & 0x7f800000u) == 0x7f800000u) {
        writeNonFinite(writer, negative, (bits & 0x7fffffu) != 0, spec);
        return;
    }
    if (spec.type == '\0' && spec.precision < 0) {
        PrF64Decimal decimal = (bits & 0x7fffffffu) == 0 ? PrF64Decimal{0, 0} : prF32ShortestDecimal(value);
        writeShortest(writer, negative, decimal, 8, spec);
        return;
    }
    // Widening is exact, so each place rounds to the same digit either way.
    formatFloat(writer, double(value), spec);
}

void formatFloat(Writer& writer, double value, const FormatSpec& spec) {
    uint64_t bits = bitCast<uint64_t>(value);
    bool negative = (bits >> 63) != 0;
    if ((bits & 0x7ff0000000000000ull) == 0x7ff0000000000000ull) {
        writeNonFinite(writer, negative, (bits & 0xfffffffffffffull) != 0, spec);
        return;
    }
    bool zero = (bits & ~(1ull << 63)) == 0;
    if (spec.type == '\0' && spec.precision < 0) {
        writeShortest(writer, negative, zero ? PrF64Decimal{0, 0} : prF64ShortestDecimal(value), 17, spec);
        return;
    }

    char sign = signOf(negative, spec);
    char mark = spec.type == 'E' || spec.type == 'G' ? 'E' : 'e';
    char digits[kMaxRoundedDigits + 1];

    if (spec.type == 'f' || spec.type == 'F') {
        uint32_t decimals = spec.precision < 0 ? 6 : uint32_t(spec.precision);
        PrF64Digits rounded = {};
        if (!zero)
            rounded = prF64FixedDigits(value, decimals, digits, sizeof(digits));
        writeNumber(writer, sign, "", fixedSize(rounded.exponent, decimals, spec.alternate), spec, [&] {
            writeFixed(writer, digits, rounded.count, rounded.exponent, decimals, spec.alternate);
        });
        return;
    }

    if (spec.type == 'e' || spec.type == 'E') {
        uint32_t decimals = spec.precision < 0 ? 6 : uint32_t(spec.precision);
        PrF64Digits rounded = {1, 1};
        if (zero)
            digits[0] = '0';
        else
            rounded = prF64SignificantDigits(value, decimals + 1, digits, sizeof(digits));
        int32_t exponent = rounded.exponent - 1;
        writeNumber(writer, sign, "", scientificSize(decimals, exponent, spec.alternate), spec, [&] {
            writeScientific(writer, digits, rounded.count, decimals, exponent, mark, spec.alternate);
        });
        return;
    }

    // The general form, which a precision with no type takes too: the magnitude picks the notation, and trailing zeros
    // are dropped unless the alternate form keeps them.
    PR_ASSERT((spec.type == 'g' || spec.type == 'G' || spec.type == '\0') && "a float's type is f, F, e, E, g or G");
    uint32_t significant = spec.precision < 0 ? 6 : uint32_t(spec.precision);
    if (significant == 0)
        significant = 1;
    PrF64Digits rounded = {1, 1};
    if (zero)
        digits[0] = '0';
    else
        rounded = prF64SignificantDigits(value, significant, digits, sizeof(digits));
    size_t count = rounded.count;
    if (!spec.alternate) {
        while (count > 1 && digits[count - 1] == '0')
            --count;
    }
    int32_t exponent = rounded.exponent - 1;
    if (int32_t(significant) > exponent && exponent >= -4) {
        int32_t places = int32_t(count) - rounded.exponent;
        if (spec.alternate)
            places = int32_t(significant) - 1 - exponent;
        uint32_t decimals = places > 0 ? uint32_t(places) : 0;
        writeNumber(writer, sign, "", fixedSize(rounded.exponent, decimals, spec.alternate), spec, [&] {
            writeFixed(writer, digits, count, rounded.exponent, decimals, spec.alternate);
        });
        return;
    }
    uint32_t decimals = spec.alternate ? significant - 1 : uint32_t(count - 1);
    writeNumber(writer, sign, "", scientificSize(decimals, exponent, spec.alternate), spec, [&] {
        writeScientific(writer, digits, count, decimals, exponent, mark, spec.alternate);
    });
}

void formatText(Writer& writer, PrStringView text, const FormatSpec& spec) {
    PR_ASSERT((spec.type == '\0' || spec.type == 's') && "text's type is s");
    if (spec.precision >= 0 && size_t(spec.precision) < text.size)
        text = prStringSlice(text, 0, size_t(spec.precision));
    writer.writePadded(text, spec.width, spec.fill, alignOf(spec, '<'));
}

void formatChar(Writer& writer, char value, const FormatSpec& spec) {
    if (isIntegerType(spec.type)) {
        formatInteger(writer, false, uint64_t(uint8_t(value)), spec);
        return;
    }
    PR_ASSERT((spec.type == '\0' || spec.type == 'c') && "a char's type is c or an integer's");
    writer.writePadded(PrStringView{&value, 1}, spec.width, spec.fill, alignOf(spec, '<'));
}

void formatBool(Writer& writer, bool value, const FormatSpec& spec) {
    if (isIntegerType(spec.type)) {
        formatInteger(writer, false, value ? 1 : 0, spec);
        return;
    }
    FormatSpec text = spec;
    text.type = '\0';
    formatText(writer, value ? "true" : "false", text);
}

void formatPointer(Writer& writer, const void* PR_NULLABLE value, const FormatSpec& spec) {
    PR_ASSERT((spec.type == '\0' || spec.type == 'p') && "a pointer's type is p");
    if (value == nullptr) {
        writer.writePadded("nullptr", spec.width, spec.fill, alignOf(spec, '<'));
        return;
    }
    char text[2 + kPrU64TextMaxBytes] = {'0', 'x'};
    size_t count = 2 + prU64Format(uint64_t(uintptr_t(value)), text + 2, 16);
    writer.writePadded(PrStringView{text, count}, spec.width, spec.fill, alignOf(spec, '<'));
}

void formatArguments(Writer& writer, PrStringView format, Span<const FormatArgument> arguments) {
    ArgumentCursor cursor = {arguments, 0};
    size_t at = 0;
    while (at < format.size) {
        char c = format.data[at];
        if (c != '{' && c != '}') {
            size_t end = at;
            while (end < format.size && format.data[end] != '{' && format.data[end] != '}')
                ++end;
            writer.write(prStringSlice(format, at, end - at));
            at = end;
            continue;
        }
        if (at + 1 < format.size && format.data[at + 1] == c) {
            writer.writeChar(c);
            at += 2;
            continue;
        }
        if (c == '}') {
            PR_ASSERT(false && "a } outside a field is written }}");
            writer.writeChar(c);
            ++at;
            continue;
        }

        ++at;
        uint32_t which = 0;
        if (at < format.size && prAsciiIsDigit(format.data[at]))
            which = digitsAt(format, &at);
        else
            which = cursor.next++;
        FormatSpec spec;
        if (at < format.size && format.data[at] == ':') {
            size_t end = fieldEnd(format, at + 1);
            spec = parseSpec(prStringSlice(format, at + 1, end - at - 1), &cursor);
            at = end;
        }
        bool closed = at < format.size && format.data[at] == '}';
        PR_ASSERT(closed && "a field closes with }");
        if (closed)
            ++at;
        writeArgument(writer, cursor, which, spec);
    }
}

} // namespace pr
