// The render settings of a suite (suite_render.h).

#include "tools/suite/suite_render.h"

#include "pr/array.h"
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

// Returns the font system that the font sets of every suite query, or null if there is none. It is created once and
// lives for the process.
PrFontSystem* PR_NULLABLE systemFonts() {
    static PrFontSystem* PR_NULLABLE system = prFontSystemCreate(PrFontSystemBackend_Default);
    return system;
}

// Reads a line of settings into `out`.
bool readLine(SuiteRender* out, PrStringView line, PrStringView root) {
    PrFontSet* fonts = out->fonts;
    for (size_t i = 0; i < line.size; ++i)
        if (line.data[i] == '#')
            line.size = i;
    line = trimmed(line);
    if (line.size == 0)
        return true;
    if (line == "system") {
        prFontSetSetSystem(fonts, systemFonts());
        return true;
    }
    if (line == "fallback") {
        Array<PrFontFallbackFamily> families;
        for (uint32_t i = 0; i < prFontSetFamilyCount(fonts); ++i)
            families.push(PrFontFallbackFamily{.family = prFontSetFamily(fonts, i)});
        prFontSetSetFallbackFamilies(fonts, families.data(), families.count());
        return true;
    }
    PrStringView setting;
    PrStringView value;
    if (!split(line, ' ', &setting, &value))
        return false;
    if (setting == "font") {
        PrTextBuffer file = {};
        prTextBufferAppend(&file, root);
        prTextBufferAppend(&file, "/");
        prTextBufferAppend(&file, value);
        PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
        const PrFont* font = nullptr;
        PrBlob* opened = prFileOpen(prTextBufferView(&file));
        prTextBufferReset(&file);
        PrStatus status = opened != nullptr ? prFontCreate(opened, 0, nullptr, &font) : PrStatus_NotFound;
        prBlobRelease(opened);
        if (status != PrStatus_Ok)
            return false;
        prFontSetAdd(fonts, &descriptor, font);
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
            prFontSetSetGeneric(fonts, generic, family);
        else
            prFontSetAlias(fonts, name, family);
        return true;
    }
    if (setting == "default") {
        PrFontGeneric generic = prFontGenericFromName(value);
        if (generic != PrFontGeneric_None)
            prFontSetSetDefaultFamily(fonts, prFontFamilyGeneric(generic));
        else
            prFontSetSetDefaultFamily(fonts, prFontFamilyFromName(value));
        return true;
    }
    if (setting == "background" && value == "white") {
        out->background = PrColor{1, 1, 1, 1};
        return true;
    }
    if (setting == "svg" && value == "image") {
        out->svgImage = true;
        return true;
    }
    return false;
}

// Reads the settings file at `path` into `out`.
bool readFile(SuiteRender* out, PrStringView path, PrStringView root) {
    PrBlob* blob = prFileOpen(path);
    if (blob == nullptr)
        return false;
    PrByteView mapping = prBlobBytes(blob);
    const char* data = reinterpret_cast<const char*>(mapping.data);
    bool ok = true;
    size_t start = 0;
    for (size_t i = 0; i <= mapping.size; ++i) {
        if (i < mapping.size && data[i] != '\n')
            continue;
        ok = readLine(out, PrStringView{data + start, i - start}, root) && ok;
        start = i + 1;
    }
    prBlobRelease(blob);
    return ok;
}

} // namespace

bool suiteRenderRead(SuiteRender* out, PrStringView path, PrStringView root) {
    suiteRenderReset(out);
    out->fonts = prFontSetCreate(nullptr);
    return readFile(out, path, root);
}

bool suiteRenderReadFonts(PrFontSet* fonts, PrStringView path, PrStringView root) {
    SuiteRender render{};
    render.fonts = fonts;
    return readFile(&render, path, root);
}

void suiteRenderReset(SuiteRender* render) {
    prFontSetDestroy(render->fonts);
    *render = SuiteRender{};
}

} // namespace suite
