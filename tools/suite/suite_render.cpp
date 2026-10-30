// The render settings of a suite (suite_render.h).

#include "tools/suite/suite_render.h"

#include "pr/file.h"
#include "pr/font_set.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"

using namespace pr;

namespace suite {

namespace {

bool isBlank(char c) {
    return c == ' ' || c == '\t' || c == '\r';
}

PrStringView trimmed(PrStringView text) {
    while (text.size != 0 && isBlank(text.data[0])) {
        ++text.data;
        --text.size;
    }
    while (text.size != 0 && isBlank(text.data[text.size - 1]))
        --text.size;
    return text;
}

// Splits the text at the first `separator`, trimming both sides. Returns false if there is none.
bool split(PrStringView text, char separator, PrStringView* first, PrStringView* rest) {
    for (size_t i = 0; i < text.size; ++i) {
        if (text.data[i] == separator) {
            *first = trimmed(PrStringView{text.data, i});
            *rest = trimmed(PrStringView{text.data + i + 1, text.size - i - 1});
            return true;
        }
    }
    return false;
}

bool readLine(SuiteRender* out, PrStringView line, PrStringView root) {
    for (size_t i = 0; i < line.size; ++i)
        if (line.data[i] == '#')
            line.size = i;
    line = trimmed(line);
    if (line.size == 0)
        return true;
    if (line == "system") {
        prFontSystemInstall(PrFontSystemBackend_Default, out->fonts);
        return true;
    }
    PrStringView setting;
    PrStringView value;
    if (!split(line, ' ', &setting, &value))
        return false;
    if (setting == "font") {
        TextBuffer file;
        textAppend(&file, root);
        textAppend(&file, "/");
        textAppend(&file, value);
        PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
        const PrFont* font = nullptr;
        PrStatus status = prFontCreateFromFile(textView(&file), 0, nullptr, &font);
        textReset(&file);
        if (status != PrStatus_Ok)
            return false;
        prFontSetAdd(out->fonts, &descriptor, font);
        prFontRelease(font);
        return true;
    }
    if (setting == "alias") {
        PrStringView name;
        PrStringView family;
        if (!split(value, '=', &name, &family))
            return false;
        PrFontGeneric generic = prFontGenericFromName(name);
        if (generic != PrFontGeneric_None)
            prFontSetSetGeneric(out->fonts, generic, family);
        else
            prFontSetAlias(out->fonts, name, family);
        return true;
    }
    if (setting == "default") {
        PrFontGeneric generic = prFontGenericFromName(value);
        if (generic != PrFontGeneric_None)
            prFontSetSetDefaultFamily(out->fonts, prFontFamilyGeneric(generic));
        else
            prFontSetSetDefaultFamily(out->fonts, prFontFamilyFromName(value));
        return true;
    }
    if (setting == "background" && value == "white") {
        out->background = PrColor{1, 1, 1, 1};
        return true;
    }
    return false;
}

} // namespace

bool suiteRenderRead(SuiteRender* out, PrStringView path, PrStringView root) {
    suiteRenderReset(out);
    out->fonts = prFontSetCreate(nullptr);
    PrFileMapping mapping{};
    if (!prFileMap(path, &mapping))
        return false;
    const char* data = reinterpret_cast<const char*>(mapping.data);
    bool ok = true;
    size_t start = 0;
    for (size_t i = 0; i <= mapping.size; ++i) {
        if (i < mapping.size && data[i] != '\n')
            continue;
        ok = readLine(out, PrStringView{data + start, i - start}, root) && ok;
        start = i + 1;
    }
    prFileUnmap(&mapping);
    return ok;
}

void suiteRenderReset(SuiteRender* render) {
    prFontSetDestroy(render->fonts);
    *render = SuiteRender{};
}

} // namespace suite
