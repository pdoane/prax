// The NUL-terminated strings of the C functions of POSIX.

#include "pr/posix_string.h"

#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/string_ops.h"

char* PR_NULLABLE prPosixStringCopy(PrStringView text) {
    if (prStringFindByte(text, '\0') != text.size)
        return nullptr;
    char* copy = static_cast<char*>(prMemoryAlloc(text.size + 1));
    if (text.size != 0)
        prMemCopy(copy, text.data, text.size);
    copy[text.size] = 0;
    return copy;
}
