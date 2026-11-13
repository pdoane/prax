// The CoreText backend of lib/font (font_coretext.h).

#include "pr/font_coretext.h"

#include "pr/apple_string.h"
#include "pr/ascii.h"
#include "pr/font_system_backend.h"
#include "pr/locale.h"
#include "pr/memory.h"

#include <CoreText/CoreText.h>

namespace pr_impl {

namespace {

// Adds the file of the font that `descriptor` describes to `set` as a file of `family`. Returns false if the font has
// no file, which is the case for a font that the system has not downloaded.
bool addFileOf(PrFontSet* set, CTFontDescriptorRef descriptor, PrStringView family) {
    CFURLRef url = static_cast<CFURLRef>(CTFontDescriptorCopyAttribute(descriptor, kCTFontURLAttribute));
    if (url == nullptr)
        return false;
    PrRawString path = prAppleFileUrlToPath(url);
    bool added = !prRawStringIsNull(&path);
    if (added)
        prFontSetAddFileFamily(set, family, prRawStringView(&path));
    prRawStringReset(&path);
    CFRelease(url);
    return added;
}

// Returns true if the family of `font` is `family`, ignoring ASCII case.
bool isFamily(CTFontRef font, PrStringView family) {
    CFStringRef name = CTFontCopyFamilyName(font);
    PrRawString text = prAppleStringToRawString(name);
    bool same = prAsciiEqualsFold(prRawStringView(&text), family);
    prRawStringReset(&text);
    CFRelease(name);
    return same;
}

bool addFamily(PrFontSystem*, PrFontSet* set, PrStringView family) {
    CFStringRef name = prAppleStringCreate(family);
    if (name == nullptr)
        return false;
    // A family that the system reserves for its own text, such as the family of the UI font, matches no descriptor.
    // Creating a font by the family name finds it.
    const void* keys[] = {kCTFontFamilyNameAttribute};
    const void* values[] = {name};
    CFDictionaryRef attributes =
        CFDictionaryCreate(nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CTFontDescriptorRef wanted = CTFontDescriptorCreateWithAttributes(attributes);
    CFSetRef mandatory = CFSetCreate(nullptr, keys, 1, &kCFTypeSetCallBacks);
    CFArrayRef matches = CTFontDescriptorCreateMatchingFontDescriptors(wanted, mandatory);
    bool added = false;
    CFIndex count = matches != nullptr ? CFArrayGetCount(matches) : 0;
    for (CFIndex i = 0; i < count; ++i) {
        CTFontDescriptorRef match = static_cast<CTFontDescriptorRef>(CFArrayGetValueAtIndex(matches, i));
        added = addFileOf(set, match, family) || added;
    }
    if (!added) {
        CTFontRef font = CTFontCreateWithName(name, 12, nullptr);
        if (isFamily(font, family)) {
            CTFontDescriptorRef descriptor = CTFontCopyFontDescriptor(font);
            added = addFileOf(set, descriptor, family);
            CFRelease(descriptor);
        }
        CFRelease(font);
    }
    if (matches != nullptr)
        CFRelease(matches);
    CFRelease(mandatory);
    CFRelease(wanted);
    CFRelease(attributes);
    CFRelease(name);
    return added;
}

// Returns the font that the system sets its own text in for `generic`: the user's fixed-pitch font for the monospace
// generic families, and the UI font for the others. The caller releases it.
CTFontRef systemFontOf(PrFontGeneric generic) {
    bool fixed = generic == PrFontGeneric_Monospace || generic == PrFontGeneric_UiMonospace;
    return CTFontCreateUIFontForLanguage(fixed ? kCTFontUIFontUserFixedPitch : kCTFontUIFontSystem, 12, nullptr);
}

// The system's family for each generic family, by PrFontGeneric, or null if it has none. The generic families that
// setsOwnText accepts take their family from systemFontOf instead.
constexpr const char* PR_NULLABLE kGenericFamilies[PrFontGeneric_Count] = {
    nullptr,                // None
    "Times",                // Serif
    "Helvetica",            // SansSerif
    nullptr,                // Monospace: the fixed-pitch font
    "Apple Chancery",       // Cursive
    "Papyrus",              // Fantasy
    nullptr,                // SystemUi: the UI font
    "STIX Two Math",        // Math
    "Apple Color Emoji",    // Emoji
    "Times",                // UiSerif
    nullptr,                // UiSansSerif: the UI font
    nullptr,                // UiMonospace: the fixed-pitch font
    nullptr,                // UiRounded: the UI font
    nullptr,                // Fangsong
    nullptr,                // Kai
    nullptr,                // KhmerMul
    "Noto Nastaliq Urdu",   // Nastaliq
};

// Returns true if the system's font for `generic` is a font that it sets its own text in (systemFontOf).
bool setsOwnText(PrFontGeneric generic) {
    switch (generic) {
        case PrFontGeneric_Monospace:
        case PrFontGeneric_SystemUi:
        case PrFontGeneric_UiSansSerif:
        case PrFontGeneric_UiMonospace:
        case PrFontGeneric_UiRounded:
            return true;
        default:
            return false;
    }
}

PrRawString genericFamily(PrFontSystem*, PrFontGeneric generic) {
    if (!setsOwnText(generic)) {
        const char* PR_NULLABLE family = kGenericFamilies[generic];
        return family != nullptr ? prRawStringCopy(family) : prRawStringNull();
    }
    CTFontRef font = systemFontOf(generic);
    CFStringRef name = CTFontCopyFamilyName(font);
    PrRawString family = prAppleStringToRawString(name);
    CFRelease(name);
    CFRelease(font);
    return family;
}

PrRawString addFallback(PrFontSystem* system, PrFontSet* set, PrStringView text, PrLocale locale, PrFontGeneric generic) {
    CFStringRef string = prAppleStringCreate(text);
    if (string == nullptr)
        return prRawStringNull();
    // CoreText's fallback cascade starts from the font of the generic family, or else from systemFontOf.
    CTFontRef base = nullptr;
    PrRawString named = setsOwnText(generic) ? prRawStringNull() : genericFamily(system, generic);
    if (prRawStringView(&named).size != 0) {
        CFStringRef name = prAppleStringCreate(prRawStringView(&named));
        if (name != nullptr) {
            base = CTFontCreateWithName(name, 12, nullptr);
            CFRelease(name);
        }
    }
    prRawStringReset(&named);
    if (base == nullptr)
        base = systemFontOf(generic);
    CFStringRef language = nullptr;
    if (locale != 0) {
        char tag[kPrLocaleMaxTag];
        uint32_t size = prLocaleTag(locale, tag);
        language = prAppleStringCreate(PrStringView{tag, size});
    }
    CTFontRef font = CTFontCreateForStringWithLanguage(base, string, CFRangeMake(0, CFStringGetLength(string)), language);
    PrRawString family = prRawStringNull();
    if (font != nullptr) {
        CFStringRef name = CTFontCopyFamilyName(font);
        family = prAppleStringToRawString(name);
        CFRelease(name);
        CTFontDescriptorRef descriptor = CTFontCopyFontDescriptor(font);
        if (prRawStringView(&family).size == 0 || !addFileOf(set, descriptor, prRawStringView(&family)))
            prRawStringReset(&family);
        CFRelease(descriptor);
        CFRelease(font);
    }
    if (language != nullptr)
        CFRelease(language);
    CFRelease(base);
    CFRelease(string);
    return family;
}

// The system, which has no state.
PrFontSystem gSystem = {.addFamilyFn = addFamily, .genericFamilyFn = genericFamily, .addFallbackFn = addFallback};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrFontSystem* prFontCoreTextCreate(void) {
    return &gSystem;
}
