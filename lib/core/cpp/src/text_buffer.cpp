#include "pr/text_buffer.h"

#include "pr/assert.h"
#include "pr/string_ops.h"

namespace pr {

namespace {

} // namespace

void textAppendUtf8(TextBuffer* out, PrStringView text) {
    size_t at = 0;
    for (;;) {
        PrStringView rest = {text.data + at, text.size - at};
        size_t valid = prUtf8Validate(rest);
        textAppend(out, rest.data, uint32_t(valid));
        at += valid;
        if (at == text.size)
            return;

        uint32_t codePoint;
        at += prUtf8Decode(text, at, &codePoint);
        textAppendCodePoint(out, kPrUtf8Replacement);
    }
}

PrString textTake(TextBuffer* out) {
    if (out->text.count <= kPrStringInlineCapacity) {
        PrString string = prStringCopy(textView(out));
        textReset(out);
        return string;
    }

    PrString string = stringAdopt(out->text.data, out->text.count, out->text.capacity);
    out->text = Array<char>{};
    return string;
}

} // namespace pr
