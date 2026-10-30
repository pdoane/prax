// The DirectWrite backend of lib/font (font_directwrite.h).

#include "pr/font_directwrite.h"

#include "pr/memory.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwrite_3.h>

namespace pr_impl {

namespace {

// UTF-8 text converted from UTF-16, which the caller frees.
struct Utf8 {
    char* PR_NULLABLE data = nullptr;
    size_t size = 0;
};

Utf8 utf8Of(const wchar_t* wide, UINT32 length) {
    Utf8 text;
    int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, int(length), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        return text;
    text.data = static_cast<char*>(prMemoryAlloc(size_t(bytes)));
    text.size = size_t(WideCharToMultiByte(CP_UTF8, 0, wide, int(length), text.data, bytes, nullptr, nullptr));
    return text;
}

// Returns the family of the font `index` of the set, as the face's typographic family names it, or the family of its
// weight, width and slope if it has none: in English if the font names it in English, and otherwise in its first
// language.
Utf8 familyOf(IDWriteFontSet* fonts, UINT32 index) {
    Utf8 family;
    const DWRITE_FONT_PROPERTY_ID properties[] = {
        DWRITE_FONT_PROPERTY_ID_TYPOGRAPHIC_FAMILY_NAME,
        DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME
    };
    for (DWRITE_FONT_PROPERTY_ID property : properties) {
        IDWriteLocalizedStrings* names = nullptr;
        BOOL exists = FALSE;
        if (FAILED(fonts->GetPropertyValues(index, property, &exists, &names)) || !exists || names == nullptr) {
            if (names != nullptr)
                names->Release();
            continue;
        }
        UINT32 at = 0;
        BOOL english = FALSE;
        if (FAILED(names->FindLocaleName(L"en-us", &at, &english)) || !english)
            at = 0;
        UINT32 length = 0;
        if (names->GetCount() > at && SUCCEEDED(names->GetStringLength(at, &length)) && length != 0) {
            wchar_t* wide = static_cast<wchar_t*>(prMemoryAlloc((size_t(length) + 1) * sizeof(wchar_t)));
            if (SUCCEEDED(names->GetString(at, wide, length + 1)))
                family = utf8Of(wide, length);
            prMemoryFree(wide);
        }
        names->Release();
        if (family.data != nullptr)
            return family;
    }
    return family;
}

// Returns the path of the local file of the font `index` of the set, or none if the font is not in a local file.
Utf8 pathOf(IDWriteFontSet* fonts, UINT32 index) {
    Utf8 path;
    IDWriteFontFaceReference* reference = nullptr;
    IDWriteFontFile* file = nullptr;
    IDWriteFontFileLoader* loader = nullptr;
    IDWriteLocalFontFileLoader* local = nullptr;
    const void* key = nullptr;
    UINT32 keySize = 0;
    UINT32 length = 0;
    if (SUCCEEDED(fonts->GetFontFaceReference(index, &reference)) && SUCCEEDED(reference->GetFontFile(&file)) &&
            SUCCEEDED(file->GetLoader(&loader)) && SUCCEEDED(loader->QueryInterface(IID_PPV_ARGS(&local))) &&
            SUCCEEDED(file->GetReferenceKey(&key, &keySize)) &&
            SUCCEEDED(local->GetFilePathLengthFromKey(key, keySize, &length))) {
        wchar_t* wide = static_cast<wchar_t*>(prMemoryAlloc((size_t(length) + 1) * sizeof(wchar_t)));
        if (SUCCEEDED(local->GetFilePathFromKey(key, keySize, wide, length + 1)))
            path = utf8Of(wide, length);
        prMemoryFree(wide);
    }
    if (local != nullptr)
        local->Release();
    if (loader != nullptr)
        loader->Release();
    if (file != nullptr)
        file->Release();
    if (reference != nullptr)
        reference->Release();
    return path;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

uint32_t prFontDirectWriteInstall(PrFontSet* set) {
    // DirectWrite's set of the system's fonts holds the fonts installed for every user and for this one. It describes
    // them from the font cache, so the files are not read.
    IDWriteFactory3* factory = nullptr;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3),
            reinterpret_cast<IUnknown**>(&factory))))
        return 0;
    uint32_t before = prFontSetFaceCount(set);
    IDWriteFontSet* fonts = nullptr;
    if (SUCCEEDED(factory->GetSystemFontSet(&fonts))) {
        UINT32 count = fonts->GetFontCount();
        for (UINT32 i = 0; i < count; ++i) {
            Utf8 path = pathOf(fonts, i);
            Utf8 family = path.data != nullptr ? familyOf(fonts, i) : Utf8{};
            if (family.data != nullptr)
                prFontSetAddFileFamily(set, PrStringView{family.data, family.size}, PrStringView{path.data, path.size});
            prMemoryFree(family.data);
            prMemoryFree(path.data);
        }
        fonts->Release();
    }
    factory->Release();
    return prFontSetFaceCount(set) - before;
}
