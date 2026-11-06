// The DirectWrite backend of lib/font (font_directwrite.h).

#include "pr/font_directwrite.h"

#include "pr/alloc.h"
#include "pr/locale.h"
#include "pr/memory.h"

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dwrite_3.h>

namespace pr_impl {

namespace {

// The system's fonts, from DirectWrite's shared factory. `fonts` holds the fonts installed for all users and for the
// current user, and `fallback` is the system's font fallback. Objects of the shared factory are thread-safe.
struct DirectWriteSystem : PrFontSystem {
    IDWriteFactory3* factory;
    IDWriteFontSet* fonts;
    IDWriteFontFallback* fallback;
};

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

// Returns `text` as NUL-terminated UTF-16, which the caller frees, or null if it is not UTF-8.
wchar_t* PR_NULLABLE wideOf(PrStringView text, UINT32* PR_NULLABLE outLength) {
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data, int(text.size), nullptr, 0);
    if (length <= 0 && text.size != 0)
        return nullptr;
    wchar_t* wide = static_cast<wchar_t*>(prMemoryAlloc((size_t(length) + 1) * sizeof(wchar_t)));
    MultiByteToWideChar(CP_UTF8, 0, text.data, int(text.size), wide, length);
    wide[length] = 0;
    if (outLength != nullptr)
        *outLength = UINT32(length);
    return wide;
}

// Copies `text` to a raw string, and frees `text`.
PrRawString rawOf(Utf8 text) {
    PrRawString raw = text.data != nullptr ? prRawStringCopy(PrStringView{text.data, text.size}) : prRawStringNull();
    prMemoryFree(text.data);
    return raw;
}

// Returns the English name in `names`, or else its first name, or none if it has no name.
Utf8 nameOf(IDWriteLocalizedStrings* names) {
    Utf8 name;
    UINT32 at = 0;
    BOOL english = FALSE;
    if (FAILED(names->FindLocaleName(L"en-us", &at, &english)) || !english)
        at = 0;
    UINT32 length = 0;
    if (names->GetCount() > at && SUCCEEDED(names->GetStringLength(at, &length)) && length != 0) {
        wchar_t* wide = static_cast<wchar_t*>(prMemoryAlloc((size_t(length) + 1) * sizeof(wchar_t)));
        if (SUCCEEDED(names->GetString(at, wide, length + 1)))
            name = utf8Of(wide, length);
        prMemoryFree(wide);
    }
    return name;
}

// Returns the path of the local file of `reference`, or none if the font is not in a local file.
Utf8 pathOf(IDWriteFontFaceReference* reference) {
    Utf8 path;
    IDWriteFontFile* file = nullptr;
    IDWriteFontFileLoader* loader = nullptr;
    IDWriteLocalFontFileLoader* local = nullptr;
    const void* key = nullptr;
    UINT32 keySize = 0;
    UINT32 length = 0;
    if (SUCCEEDED(reference->GetFontFile(&file)) && SUCCEEDED(file->GetLoader(&loader)) &&
            SUCCEEDED(loader->QueryInterface(IID_PPV_ARGS(&local))) && SUCCEEDED(file->GetReferenceKey(&key, &keySize)) &&
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
    return path;
}

// Adds the local files of the fonts of `fonts` to `set` as files of `family`, and returns true if it added any.
bool addFiles(PrFontSet* set, IDWriteFontSet* fonts, PrStringView family) {
    bool added = false;
    UINT32 count = fonts->GetFontCount();
    for (UINT32 i = 0; i < count; ++i) {
        IDWriteFontFaceReference* reference = nullptr;
        if (FAILED(fonts->GetFontFaceReference(i, &reference)))
            continue;
        Utf8 path = pathOf(reference);
        reference->Release();
        if (path.data != nullptr) {
            prFontSetAddFileFamily(set, family, PrStringView{path.data, path.size});
            added = true;
        }
        prMemoryFree(path.data);
    }
    return added;
}

void destroy(PrFontSystem* system) {
    DirectWriteSystem* d = static_cast<DirectWriteSystem*>(system);
    d->fallback->Release();
    d->fonts->Release();
    d->factory->Release();
    prMemoryFree(d);
}

bool addFamily(PrFontSystem* system, PrFontSet* set, PrStringView family) {
    DirectWriteSystem* d = static_cast<DirectWriteSystem*>(system);
    wchar_t* name = wideOf(family, nullptr);
    if (name == nullptr)
        return false;
    // Match the family by its typographic name first, and then by its Win32 name, which names a family of one weight,
    // width and slope.
    const DWRITE_FONT_PROPERTY_ID properties[] = {
        DWRITE_FONT_PROPERTY_ID_TYPOGRAPHIC_FAMILY_NAME,
        DWRITE_FONT_PROPERTY_ID_WIN32_FAMILY_NAME,
    };
    bool added = false;
    for (DWRITE_FONT_PROPERTY_ID id : properties) {
        DWRITE_FONT_PROPERTY property{id, name, nullptr};
        IDWriteFontSet* matching = nullptr;
        if (SUCCEEDED(d->fonts->GetMatchingFonts(&property, 1, &matching))) {
            added = addFiles(set, matching, family);
            matching->Release();
        }
        if (added)
            break;
    }
    prMemoryFree(name);
    return added;
}

// The system's family for each generic family, by PrFontGeneric, or null if it has none.
constexpr const char* PR_NULLABLE kGenericFamilies[PrFontGeneric_Count] = {
    nullptr,                // None
    "Times New Roman",      // Serif
    "Arial",                // SansSerif
    "Consolas",             // Monospace
    "Comic Sans MS",        // Cursive
    "Impact",               // Fantasy
    "Segoe UI",             // SystemUi
    "Cambria Math",         // Math
    "Segoe UI Emoji",       // Emoji
    "Times New Roman",      // UiSerif
    "Segoe UI",             // UiSansSerif
    "Consolas",             // UiMonospace
    "Segoe UI",             // UiRounded
    "FangSong",             // Fangsong
    "KaiTi",                // Kai
    nullptr,                // KhmerMul
    "Urdu Typesetting",     // Nastaliq
};

PrRawString genericFamily(PrFontSystem*, PrFontGeneric generic) {
    const char* PR_NULLABLE family = kGenericFamilies[generic];
    return family != nullptr ? prRawStringCopy(family) : prRawStringNull();
}

// One cluster of text in one locale, as DirectWrite's fallback reads it. It is not reference counted, because it lives
// on the stack of the call that maps it.
struct ClusterSource final : IDWriteTextAnalysisSource {
    const wchar_t* text;
    UINT32 length;
    const wchar_t* locale;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDWriteTextAnalysisSource)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    HRESULT STDMETHODCALLTYPE GetTextAtPosition(UINT32 position, const WCHAR** out, UINT32* outLength) override {
        *out = position < length ? text + position : nullptr;
        *outLength = position < length ? length - position : 0;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetTextBeforePosition(UINT32 position, const WCHAR** out, UINT32* outLength) override {
        *out = position != 0 && position <= length ? text : nullptr;
        *outLength = position <= length ? position : 0;
        return S_OK;
    }
    DWRITE_READING_DIRECTION STDMETHODCALLTYPE GetParagraphReadingDirection() override {
        return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }
    HRESULT STDMETHODCALLTYPE GetLocaleName(UINT32 position, UINT32* outLength, const WCHAR** out) override {
        *outLength = position < length ? length - position : 0;
        *out = locale;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetNumberSubstitution(UINT32 position, UINT32* outLength,
            IDWriteNumberSubstitution** out) override {
        *outLength = position < length ? length - position : 0;
        *out = nullptr;
        return S_OK;
    }
};

PrRawString addFallback(PrFontSystem* system, PrFontSet* set, PrStringView text, PrLocale locale, PrFontGeneric generic) {
    DirectWriteSystem* d = static_cast<DirectWriteSystem*>(system);
    UINT32 length = 0;
    wchar_t* wide = wideOf(text, &length);
    if (wide == nullptr || length == 0) {
        prMemoryFree(wide);
        return prRawStringNull();
    }
    char tag[kPrLocaleMaxTag];
    uint32_t tagSize = locale != 0 ? prLocaleTag(locale, tag) : 0;
    wchar_t* localeName = wideOf(PrStringView{tag, tagSize}, nullptr);
    // DirectWrite's fallback starts from the family of the generic family, or else from Segoe UI.
    const char* PR_NULLABLE base = kGenericFamilies[generic];
    wchar_t* baseName = wideOf(base != nullptr ? PrStringView{base} : PrStringView{"Segoe UI"}, nullptr);
    ClusterSource source{};
    source.text = wide;
    source.length = length;
    source.locale = localeName != nullptr ? localeName : L"";
    UINT32 mapped = 0;
    IDWriteFont* font = nullptr;
    FLOAT scale = 1;
    PrRawString family = prRawStringNull();
    if (SUCCEEDED(d->fallback->MapCharacters(&source, 0, length, nullptr, baseName, DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, &mapped, &font, &scale)) && font != nullptr) {
        IDWriteFontFamily* fontFamily = nullptr;
        IDWriteLocalizedStrings* names = nullptr;
        if (SUCCEEDED(font->GetFontFamily(&fontFamily)) && SUCCEEDED(fontFamily->GetFamilyNames(&names)))
            family = rawOf(nameOf(names));
        IDWriteFont3* font3 = nullptr;
        IDWriteFontFaceReference* reference = nullptr;
        bool added = false;
        if (prRawStringView(&family).size != 0 && SUCCEEDED(font->QueryInterface(IID_PPV_ARGS(&font3))) &&
                SUCCEEDED(font3->GetFontFaceReference(&reference))) {
            Utf8 path = pathOf(reference);
            if (path.data != nullptr) {
                prFontSetAddFileFamily(set, prRawStringView(&family), PrStringView{path.data, path.size});
                added = true;
            }
            prMemoryFree(path.data);
        }
        if (!added)
            prRawStringReset(&family);
        if (reference != nullptr)
            reference->Release();
        if (font3 != nullptr)
            font3->Release();
        if (names != nullptr)
            names->Release();
        if (fontFamily != nullptr)
            fontFamily->Release();
        font->Release();
    }
    prMemoryFree(baseName);
    prMemoryFree(localeName);
    prMemoryFree(wide);
    return family;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrFontSystem* PR_NULLABLE prFontDirectWriteCreate(void) {
    IDWriteFactory3* factory = nullptr;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3),
            reinterpret_cast<IUnknown**>(&factory))))
        return nullptr;
    IDWriteFontSet* fonts = nullptr;
    IDWriteFontFallback* fallback = nullptr;
    if (FAILED(factory->GetSystemFontSet(&fonts)) || FAILED(factory->GetSystemFontFallback(&fallback))) {
        if (fonts != nullptr)
            fonts->Release();
        factory->Release();
        return nullptr;
    }
    DirectWriteSystem* system = allocZeroed<DirectWriteSystem>();
    system->destroy = destroy;
    system->addFamily = addFamily;
    system->genericFamily = genericFamily;
    system->addFallback = addFallback;
    system->factory = factory;
    system->fonts = fonts;
    system->fallback = fallback;
    return system;
}
