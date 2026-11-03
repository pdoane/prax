#include "pr/integer.h"

#include "pr/assert.h"

size_t prU64Format(uint64_t value, char* out, uint32_t radix) {
    PR_ASSERT(radix >= 2 && radix <= kPrU64MaxRadix);
    const char* digits = "0123456789abcdefghijklmnopqrstuvwxyz";

    char reversed[kPrU64TextMaxBytes];
    size_t size = 0;
    do {
        reversed[size++] = digits[value % radix];
        value /= radix;
    } while (value != 0);

    for (size_t i = 0; i < size; ++i)
        out[i] = reversed[size - i - 1];
    return size;
}

bool prU64Parse(const char* data, size_t size, uint64_t* out, uint32_t radix) {
    uint64_t value = 0;
    if (size == 0 || prU64ParsePrefix(data, size, &value, radix) != size)
        return false;
    *out = value;
    return true;
}

size_t prU64ParsePrefix(const char* data, size_t size, uint64_t* out, uint32_t radix) {
    PR_ASSERT(radix >= 2 && radix <= kPrU64MaxRadix);
    uint64_t value = 0;
    size_t i = 0;
    for (; i < size; ++i) {
        char c = data[i];
        uint32_t digit = radix;
        if (c >= '0' && c <= '9')
            digit = uint32_t(c - '0');
        else if (c >= 'a' && c <= 'z')
            digit = uint32_t(c - 'a' + 10);
        else if (c >= 'A' && c <= 'Z')
            digit = uint32_t(c - 'A' + 10);
        if (digit >= radix)
            break;
        value = value > (UINT64_MAX - digit) / radix ? UINT64_MAX : value * radix + digit;
    }
    if (i != 0)
        *out = value;
    return i;
}
