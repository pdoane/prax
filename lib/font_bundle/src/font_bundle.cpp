// The font bundles.

#include "pr/font_bundle.h"

#include "pr/count_of.h"
#include "pr/locale_common.h"
#include "pr/raw_array.h"
#include "pr/string_ops.h"

namespace pr_impl {
namespace {

// A font file of a bundle, with the family that it holds, the generic family that it provides if any, and an alias if
// any. For Standard14, the alias is the name of the PDF standard font that the file stands in for.
struct NamedFont {
    const char* file;
    const char* family;
    PrFontGeneric generic = PrFontGeneric_None;
    const char* PR_NULLABLE alias = nullptr;
};

constexpr NamedFont kNoto[] = {
    {"NotoSerif[wdth,wght].ttf", "Noto Serif", PrFontGeneric_Serif},
    {"NotoSerif-Italic[wdth,wght].ttf", "Noto Serif"},
    {"NotoSans[wdth,wght].ttf", "Noto Sans", PrFontGeneric_SansSerif},
    {"NotoSans[wdth,wght].ttf", "Noto Sans", PrFontGeneric_SystemUi},
    {"NotoSans-Italic[wdth,wght].ttf", "Noto Sans"},
    {"NotoSansMono[wdth,wght].ttf", "Noto Sans Mono", PrFontGeneric_Monospace},
    {"NewCMMath-Regular.otf", "NewComputerModernMath", PrFontGeneric_Math},
};

constexpr NamedFont kNotoMono[] = {{"NotoSansMono[wdth,wght].ttf", "Noto Sans Mono", PrFontGeneric_Monospace}};

constexpr NamedFont kStandard14[] = {
    {"Tinos-Regular.ttf", "Tinos", PrFontGeneric_Serif, "Times"},
    {"Tinos-Bold.ttf", "Tinos"},
    {"Tinos-Italic.ttf", "Tinos"},
    {"Tinos-BoldItalic.ttf", "Tinos"},
    {"Arimo-Regular.ttf", "Arimo", PrFontGeneric_SansSerif, "Helvetica"},
    {"Arimo-Bold.ttf", "Arimo"},
    {"Arimo-Italic.ttf", "Arimo"},
    {"Arimo-BoldItalic.ttf", "Arimo"},
    {"Cousine-Regular.ttf", "Cousine", PrFontGeneric_Monospace, "Courier"},
    {"Cousine-Bold.ttf", "Cousine"},
    {"Cousine-Italic.ttf", "Cousine"},
    {"Cousine-BoldItalic.ttf", "Cousine"},
    {"LeviSymbol-Regular.ttf", "LeviSymbol", PrFontGeneric_None, "Symbol"},
    {"Levibats-Regular.ttf", "Levibats-Regular", PrFontGeneric_None, "ZapfDingbats"},
    {"NewCMMath-Regular.otf", "NewComputerModernMath", PrFontGeneric_Math},
};

// Plex's sans, serif and monospace faces are its variable fonts. Their families have names of their own, so the family
// names of Plex's static fonts are aliases of them.
constexpr NamedFont kPlex[] = {
    {"IBMPlexSerifVar-Roman.ttf", "IBM Plex Serif Var", PrFontGeneric_Serif, "IBM Plex Serif"},
    {"IBMPlexSerifVar-Italic.ttf", "IBM Plex Serif Var"},
    {"IBMPlexSansVar-Roman.ttf", "IBM Plex Sans Var", PrFontGeneric_SansSerif, "IBM Plex Sans"},
    {"IBMPlexSansVar-Roman.ttf", "IBM Plex Sans Var", PrFontGeneric_SystemUi},
    {"IBMPlexSansVar-Italic.ttf", "IBM Plex Sans Var"},
    {"IBMPlexMonoVar-Roman.ttf", "IBM Plex Mono Var", PrFontGeneric_Monospace, "IBM Plex Mono"},
    {"IBMPlexMonoVar-Italic.ttf", "IBM Plex Mono Var"},
    {"IBMPlexMath-Regular.ttf", "IBM Plex Math", PrFontGeneric_Math},
};

// A generic family that a fallback family of a bundle stands for.
struct GenericFamily {
    PrFontGeneric generic;
    const char* family;
};

constexpr GenericFamily kNotoGenerics[] = {
    {PrFontGeneric_Emoji, "Noto Color Emoji"},
    {PrFontGeneric_Nastaliq, "Noto Nastaliq Urdu"},
};

// The generic families of the fallback families, and the locales that they are made for: Simplified and Traditional
// Chinese, Hong Kong's forms of Traditional, Japanese and Korean, regardless of the language each writes, and Urdu.
constexpr PrFontGeneric kSans = PrFontGeneric_SansSerif;
constexpr PrFontGeneric kSerif = PrFontGeneric_Serif;
constexpr PrLocale kHans = prLocaleMake(PrLanguage_None, PrScript_Hans, PrRegion_None);
constexpr PrLocale kHant = prLocaleMake(PrLanguage_None, PrScript_Hant, PrRegion_None);
constexpr PrLocale kHantHongKong = prLocaleMake(PrLanguage_None, PrScript_Hant, PrRegion_HK);
constexpr PrLocale kJapanese = prLocaleMake(PrLanguage_None, PrScript_Jpan, PrRegion_None);
constexpr PrLocale kKorean = prLocaleMake(PrLanguage_None, PrScript_Kore, PrRegion_None);
constexpr PrLocale kUrdu = prLocaleMake(PrLanguage_Ur, PrScript_Zzzz, PrRegion_None);

// A font file that a bundle falls back to: the family that it holds, the generic family whose style it is drawn in, the
// locales that it is made for, and whether it serves private-use code points (PrFontFallbackFamily).
struct FallbackFont {
    const char* file;
    const char* family;
    PrFontGeneric generic = PrFontGeneric_None;
    PrLocale locale = 0;
    bool privateUse = false;
};

// In Noto's fallback, Latin, Greek and Cyrillic come first, because the CJK families cover them too. The math and
// symbol families come before the CJK families of their style for the same reason. The CJK families cover arrows,
// mathematical operators and other symbols of no script, and draw them as wide and as high as ideographs. Text in other
// scripts takes these symbols from the symbol families instead: a sans family for sans text, and New Computer Modern
// Math for serif text. Text in a CJK locale still takes them from its own CJK family, because matching prefers the
// family made for the locale.
//
// Each script's sans family comes before its serif one. Simplified Chinese comes first among the CJK families, and Han
// text of no language uses it. Last Resort comes last, and serves every code point with the glyph of its block. The CJK
// families are made for the writing systems that they draw, regardless of the language.

constexpr FallbackFont kNotoFallback[] = {
    {"NotoSans[wdth,wght].ttf", "Noto Sans", kSans},
    {"NotoSans-Italic[wdth,wght].ttf", "Noto Sans", kSans},
    {"NotoSansArabic[wdth,wght].ttf", "Noto Sans Arabic", kSans},
    {"NotoNaskhArabic[wght].ttf", "Noto Naskh Arabic", kSerif},
    {"NotoNastaliqUrdu[wght].ttf", "Noto Nastaliq Urdu", PrFontGeneric_Nastaliq, kUrdu},
    {"NotoSansHebrew[wdth,wght].ttf", "Noto Sans Hebrew", kSans},
    {"NotoSansDevanagari[wdth,wght].ttf", "Noto Sans Devanagari", kSans},
    {"NotoSansBengali[wdth,wght].ttf", "Noto Sans Bengali", kSans},
    {"NotoSansTamil[wdth,wght].ttf", "Noto Sans Tamil", kSans},
    {"NotoSansTelugu[wdth,wght].ttf", "Noto Sans Telugu", kSans},
    {"NotoSansKannada[wdth,wght].ttf", "Noto Sans Kannada", kSans},
    {"NotoSansMalayalam[wdth,wght].ttf", "Noto Sans Malayalam", kSans},
    {"NotoSansGujarati[wdth,wght].ttf", "Noto Sans Gujarati", kSans},
    {"NotoSansGurmukhi[wdth,wght].ttf", "Noto Sans Gurmukhi", kSans},
    {"NotoSansOriya[wdth,wght].ttf", "Noto Sans Oriya", kSans},
    {"NotoSansSinhala[wdth,wght].ttf", "Noto Sans Sinhala", kSans},
    {"NotoSansThaana[wght].ttf", "Noto Sans Thaana", kSans},
    {"NotoSansThai[wdth,wght].ttf", "Noto Sans Thai", kSans},
    {"NotoSansLao[wdth,wght].ttf", "Noto Sans Lao", kSans},
    {"NotoSansMyanmar[wdth,wght].ttf", "Noto Sans Myanmar", kSans},
    {"NotoSansKhmer[wdth,wght].ttf", "Noto Sans Khmer", kSans},
    {"NotoSerifTibetan[wght].ttf", "Noto Serif Tibetan", kSerif},
    {"NotoSansMongolian-Regular.ttf", "Noto Sans Mongolian", kSans},
    {"NotoSansArmenian[wdth,wght].ttf", "Noto Sans Armenian", kSans},
    {"NotoSansGeorgian[wdth,wght].ttf", "Noto Sans Georgian", kSans},
    {"NotoSansEthiopic[wdth,wght].ttf", "Noto Sans Ethiopic", kSans},
    {"NotoSansMath-Regular.ttf", "Noto Sans Math", kSans},
    {"NotoSansSymbols[wght].ttf", "Noto Sans Symbols", kSans},
    {"NotoSansSymbols2-Regular.ttf", "Noto Sans Symbols 2", kSans},
    {"NotoSansSC-VF.otf", "Noto Sans SC", kSans, kHans},
    {"NotoSansTC-VF.otf", "Noto Sans TC", kSans, kHant},
    {"NotoSansHK-VF.otf", "Noto Sans HK", kSans, kHantHongKong},
    {"NotoSansJP-VF.otf", "Noto Sans JP", kSans, kJapanese},
    {"NotoSansKR-VF.otf", "Noto Sans KR", kSans, kKorean},
    {"NotoColorEmoji.ttf", "Noto Color Emoji", PrFontGeneric_Emoji},
    {"NotoSansMono[wdth,wght].ttf", "Noto Sans Mono", PrFontGeneric_Monospace},
    {"NotoSerif[wdth,wght].ttf", "Noto Serif", kSerif},
    {"NotoSerif-Italic[wdth,wght].ttf", "Noto Serif", kSerif},
    {"NotoSerifHebrew[wdth,wght].ttf", "Noto Serif Hebrew", kSerif},
    {"NotoSerifDevanagari[wdth,wght].ttf", "Noto Serif Devanagari", kSerif},
    {"NotoSerifBengali[wdth,wght].ttf", "Noto Serif Bengali", kSerif},
    {"NotoSerifTamil[wdth,wght].ttf", "Noto Serif Tamil", kSerif},
    {"NotoSerifTamil-Italic[wdth,wght].ttf", "Noto Serif Tamil", kSerif},
    {"NotoSerifTelugu[wght].ttf", "Noto Serif Telugu", kSerif},
    {"NotoSerifKannada[wght].ttf", "Noto Serif Kannada", kSerif},
    {"NotoSerifMalayalam[wght].ttf", "Noto Serif Malayalam", kSerif},
    {"NotoSerifGujarati[wght].ttf", "Noto Serif Gujarati", kSerif},
    {"NotoSerifGurmukhi[wght].ttf", "Noto Serif Gurmukhi", kSerif},
    {"NotoSerifOriya[wght].ttf", "Noto Serif Oriya", kSerif},
    {"NotoSerifSinhala[wdth,wght].ttf", "Noto Serif Sinhala", kSerif},
    {"NotoSerifThai[wdth,wght].ttf", "Noto Serif Thai", kSerif},
    {"NotoSerifLao[wdth,wght].ttf", "Noto Serif Lao", kSerif},
    {"NotoSerifMyanmar[wdth,wght].ttf", "Noto Serif Myanmar", kSerif},
    {"NotoSerifKhmer[wdth,wght].ttf", "Noto Serif Khmer", kSerif},
    {"NotoSerifArmenian[wdth,wght].ttf", "Noto Serif Armenian", kSerif},
    {"NotoSerifGeorgian[wdth,wght].ttf", "Noto Serif Georgian", kSerif},
    {"NotoSerifEthiopic[wdth,wght].ttf", "Noto Serif Ethiopic", kSerif},
    {"NewCMMath-Regular.otf", "NewComputerModernMath", kSerif},
    {"NotoSerifSC-VF.otf", "Noto Serif SC", kSerif, kHans},
    {"NotoSerifTC-VF.otf", "Noto Serif TC", kSerif, kHant},
    {"NotoSerifHK-VF.otf", "Noto Serif HK", kSerif, kHantHongKong},
    {"NotoSerifJP-VF.otf", "Noto Serif JP", kSerif, kJapanese},
    {"NotoSerifKR-VF.otf", "Noto Serif KR", kSerif, kKorean},
    {"LastResort-Regular.ttf", "Last Resort", PrFontGeneric_None},
};

// Standard14 falls back to its own text faces, so that text in one of them takes what that face lacks from the others.
// It falls back to New Computer Modern Math for math and symbols.
constexpr FallbackFont kStandard14Fallback[] = {
    {"Arimo-Regular.ttf", "Arimo", kSans},
    {"Arimo-Bold.ttf", "Arimo", kSans},
    {"Arimo-Italic.ttf", "Arimo", kSans},
    {"Arimo-BoldItalic.ttf", "Arimo", kSans},
    {"Tinos-Regular.ttf", "Tinos", kSerif},
    {"Tinos-Bold.ttf", "Tinos", kSerif},
    {"Tinos-Italic.ttf", "Tinos", kSerif},
    {"Tinos-BoldItalic.ttf", "Tinos", kSerif},
    {"Cousine-Regular.ttf", "Cousine", PrFontGeneric_Monospace},
    {"Cousine-Bold.ttf", "Cousine", PrFontGeneric_Monospace},
    {"Cousine-Italic.ttf", "Cousine", PrFontGeneric_Monospace},
    {"Cousine-BoldItalic.ttf", "Cousine", PrFontGeneric_Monospace},
    {"NewCMMath-Regular.otf", "NewComputerModernMath", kSerif},
};

// Plex falls back to IBM's own families. Plex Sans comes first, for Latin, Greek and Cyrillic, and Plex Math follows
// it. Both come before the script families, because those cover Latin and symbols too. The CJK families come after the
// other scripts, because they also cover arrows and other symbols of no script, drawn as wide and as high as
// ideographs. Plex Mono and Plex Serif come last, for text in their styles.
constexpr FallbackFont kPlexFallback[] = {
    {"IBMPlexSansVar-Roman.ttf", "IBM Plex Sans Var", kSans},
    {"IBMPlexSansVar-Italic.ttf", "IBM Plex Sans Var", kSans},
    {"IBMPlexMath-Regular.ttf", "IBM Plex Math", kSans},
    {"IBMPlexSansArabic-Regular.ttf", "IBM Plex Sans Arabic", kSans},
    {"IBMPlexSansArabic-Medium.ttf", "IBM Plex Sans Arabic", kSans},
    {"IBMPlexSansArabic-SemiBold.ttf", "IBM Plex Sans Arabic", kSans},
    {"IBMPlexSansArabic-Bold.ttf", "IBM Plex Sans Arabic", kSans},
    {"IBMPlexSansHebrew-Regular.ttf", "IBM Plex Sans Hebrew", kSans},
    {"IBMPlexSansHebrew-Medium.ttf", "IBM Plex Sans Hebrew", kSans},
    {"IBMPlexSansHebrew-SemiBold.ttf", "IBM Plex Sans Hebrew", kSans},
    {"IBMPlexSansHebrew-Bold.ttf", "IBM Plex Sans Hebrew", kSans},
    {"IBMPlexSansDevanagari-Regular.ttf", "IBM Plex Sans Devanagari", kSans},
    {"IBMPlexSansDevanagari-Medium.ttf", "IBM Plex Sans Devanagari", kSans},
    {"IBMPlexSansDevanagari-SemiBold.ttf", "IBM Plex Sans Devanagari", kSans},
    {"IBMPlexSansDevanagari-Bold.ttf", "IBM Plex Sans Devanagari", kSans},
    {"IBMPlexSansThai-Regular.ttf", "IBM Plex Sans Thai", kSans},
    {"IBMPlexSansThai-Medium.ttf", "IBM Plex Sans Thai", kSans},
    {"IBMPlexSansThai-SemiBold.ttf", "IBM Plex Sans Thai", kSans},
    {"IBMPlexSansThai-Bold.ttf", "IBM Plex Sans Thai", kSans},
    {"IBMPlexSansSC-Regular.otf", "IBM Plex Sans SC", kSans, kHans},
    {"IBMPlexSansSC-Medium.otf", "IBM Plex Sans SC", kSans, kHans},
    {"IBMPlexSansSC-SemiBold.otf", "IBM Plex Sans SC", kSans, kHans},
    {"IBMPlexSansSC-Bold.otf", "IBM Plex Sans SC", kSans, kHans},
    {"IBMPlexSansTC-Regular.otf", "IBM Plex Sans TC", kSans, kHant},
    {"IBMPlexSansTC-Medium.otf", "IBM Plex Sans TC", kSans, kHant},
    {"IBMPlexSansTC-SemiBold.otf", "IBM Plex Sans TC", kSans, kHant},
    {"IBMPlexSansTC-Bold.otf", "IBM Plex Sans TC", kSans, kHant},
    {"IBMPlexSansJP-Regular.otf", "IBM Plex Sans JP", kSans, kJapanese},
    {"IBMPlexSansJP-Medium.otf", "IBM Plex Sans JP", kSans, kJapanese},
    {"IBMPlexSansJP-SemiBold.otf", "IBM Plex Sans JP", kSans, kJapanese},
    {"IBMPlexSansJP-Bold.otf", "IBM Plex Sans JP", kSans, kJapanese},
    {"IBMPlexSansKR-Regular.otf", "IBM Plex Sans KR", kSans, kKorean},
    {"IBMPlexSansKR-Medium.otf", "IBM Plex Sans KR", kSans, kKorean},
    {"IBMPlexSansKR-SemiBold.otf", "IBM Plex Sans KR", kSans, kKorean},
    {"IBMPlexSansKR-Bold.otf", "IBM Plex Sans KR", kSans, kKorean},
    {"IBMPlexMonoVar-Roman.ttf", "IBM Plex Mono Var", PrFontGeneric_Monospace},
    {"IBMPlexMonoVar-Italic.ttf", "IBM Plex Mono Var", PrFontGeneric_Monospace},
    {"IBMPlexSerifVar-Roman.ttf", "IBM Plex Serif Var", kSerif},
    {"IBMPlexSerifVar-Italic.ttf", "IBM Plex Serif Var", kSerif},
};

// Nerd Fonts' symbols serve the private-use code points. They also map a few other code points, which they serve only
// if the bundles before them have no face. They are drawn in no generic family, so that no generic ranks them above a
// face of text.
constexpr FallbackFont kNerdFontSymbolsFallback[] = {
    {"SymbolsNerdFontMono-Regular.ttf", "Symbols Nerd Font Mono", PrFontGeneric_None, 0, true},
};

// The fonts of a bundle: those that it names, those that it falls back to, and the generic families that its fallback
// families stand for. A file may be both named and a fallback.
struct Stack {
    const NamedFont* PR_NULLABLE named;
    uint32_t namedCount;
    const FallbackFont* fallback;
    uint32_t fallbackCount;
    const GenericFamily* PR_NULLABLE generics;
    uint32_t genericCount;
};

Stack bundleStack(PrFontBundle bundle) {
    switch (bundle) {
        case PrFontBundle_Complete:
            break;
        case PrFontBundle_NotoMono:
            return Stack{.named = kNotoMono, .namedCount = uint32_t(countOf(kNotoMono))};
        case PrFontBundle_Standard14:
            return Stack{
                .named = kStandard14,
                .namedCount = uint32_t(countOf(kStandard14)),
                .fallback = kStandard14Fallback,
                .fallbackCount = uint32_t(countOf(kStandard14Fallback)),
            };
        case PrFontBundle_Plex:
            return Stack{
                .named = kPlex,
                .namedCount = uint32_t(countOf(kPlex)),
                .fallback = kPlexFallback,
                .fallbackCount = uint32_t(countOf(kPlexFallback)),
            };
        case PrFontBundle_NerdFontSymbols:
            return Stack{
                .fallback = kNerdFontSymbolsFallback,
                .fallbackCount = uint32_t(countOf(kNerdFontSymbolsFallback)),
            };
    }
    return Stack{
        .named = kNoto,
        .namedCount = uint32_t(countOf(kNoto)),
        .fallback = kNotoFallback,
        .fallbackCount = uint32_t(countOf(kNotoFallback)),
        .generics = kNotoGenerics,
        .genericCount = uint32_t(countOf(kNotoGenerics)),
    };
}

// A file of the bundles being installed, and whether it was added to the set.
struct BundleFile {
    const char* file;
    bool added;
};

// Returns the index of `file` in `files`, or kArrayMissing.
uint32_t bundleFileIndex(const RawArray<BundleFile>& files, const char* file) {
    for (uint32_t i = 0; i < files.count; ++i) {
        if (PrStringView(files.data[i].file) == file)
            return i;
    }
    return kArrayMissing;
}

// Records `file` in `files` once, the first time any of the bundles names it.
void bundleFileRecord(RawArray<BundleFile>& files, const char* file) {
    if (bundleFileIndex(files, file) == kArrayMissing)
        files.push(BundleFile{file, false});
}

// Records every file of the stacks of `bundles`, each once.
void bundleFilesRecord(RawArray<BundleFile>& files, const PrFontBundle* bundles, uint32_t count) {
    for (uint32_t b = 0; b < count; ++b) {
        Stack stack = bundleStack(bundles[b]);
        for (uint32_t i = 0; i < stack.namedCount; ++i)
            bundleFileRecord(files, stack.named[i].file);
        for (uint32_t i = 0; i < stack.fallbackCount; ++i)
            bundleFileRecord(files, stack.fallback[i].file);
    }
}

// Returns true if `file` was added.
bool bundleFileIsAdded(const RawArray<BundleFile>& files, const char* file) {
    return files.data[bundleFileIndex(files, file)].added;
}

// Adds the font file `file` of `directory` to `set`. Returns false if it does not load.
bool add(PrFontSet* set, PrStringView directory, const char* file) {
    PrRawString path = prRawStringCopy(directory);
    prRawStringAppend(&path, "/");
    prRawStringAppend(&path, file);
    const PrFont* PR_NULLABLE font = nullptr;
    PrStatus status = prFontCreateFromFile(prRawStringView(&path), 0, nullptr, &font);
    prRawStringReset(&path);
    if (status != PrStatus_Ok)
        return false;
    PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
    prFontSetAdd(set, &descriptor, font);
    prFontRelease(font);
    return true;
}

} // namespace
} // namespace pr_impl

using namespace pr_impl;

uint32_t prFontBundleInstall(PrFontSet* set, const PrFontBundle* bundles, uint32_t count, PrStringView directory) {
    RawArray<BundleFile> files{};
    bundleFilesRecord(files, bundles, count);
    uint32_t added = 0;
    for (uint32_t i = 0; i < files.count; ++i) {
        files.data[i].added = add(set, directory, files.data[i].file);
        added += files.data[i].added ? 1 : 0;
    }
    // The bundles bind their names last to first, so that the first bundle to give a name binds it.
    for (uint32_t b = count; b-- > 0;) {
        Stack stack = bundleStack(bundles[b]);
        for (uint32_t i = 0; i < stack.namedCount; ++i) {
            const NamedFont& entry = stack.named[i];
            if (!bundleFileIsAdded(files, entry.file))
                continue;
            if (entry.generic != PrFontGeneric_None)
                prFontSetSetGeneric(set, entry.generic, entry.family);
            if (entry.alias != nullptr)
                prFontSetAlias(set, entry.alias, entry.family);
        }
        for (uint32_t i = 0; i < stack.genericCount; ++i) {
            for (uint32_t k = 0; k < stack.fallbackCount; ++k) {
                if (PrStringView(stack.fallback[k].family) == stack.generics[i].family &&
                        bundleFileIsAdded(files, stack.fallback[k].file)) {
                    prFontSetSetGeneric(set, stack.generics[i].generic, stack.generics[i].family);
                    break;
                }
            }
        }
    }
    prFontSetSetDefaultFamily(set, prFontFamilyGeneric(PrFontGeneric_Serif));
    // The fallback families of each bundle follow in turn. A family is listed once, at its place in the first bundle
    // that lists it.
    RawArray<PrFontFallbackFamily> families{};
    for (uint32_t b = 0; b < count; ++b) {
        Stack stack = bundleStack(bundles[b]);
        for (uint32_t i = 0; i < stack.fallbackCount; ++i) {
            const FallbackFont& entry = stack.fallback[i];
            if (!bundleFileIsAdded(files, entry.file))
                continue;
            bool listed = false;
            for (uint32_t k = 0; k < families.count && !listed; ++k)
                listed = families.data[k].family == entry.family;
            if (!listed)
                families.push(PrFontFallbackFamily{entry.family, entry.locale, entry.generic, entry.privateUse});
        }
    }
    prFontSetSetFallbackFamilies(set, families.data, families.count);
    families.reset();
    files.reset();
    return added;
}

uint32_t prFontBundleFileCount(const PrFontBundle* bundles, uint32_t count) {
    RawArray<BundleFile> files{};
    bundleFilesRecord(files, bundles, count);
    uint32_t total = files.count;
    files.reset();
    return total;
}
