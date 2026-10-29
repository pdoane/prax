// The font bundles.

#include "pr/font_bundle.h"

#include "pr/count_of.h"
#include "pr/locale_common.h"
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
    {"NotoSans-Italic[wdth,wght].ttf", "Noto Sans"},
    {"NotoSansMono[wdth,wght].ttf", "Noto Sans Mono", PrFontGeneric_Monospace},
    {"NewCMMath-Regular.otf", "NewComputerModernMath", PrFontGeneric_Math},
};

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
};

// The generic families that fallback families stand for in every bundle.
struct GenericFamily {
    PrFontGeneric generic;
    const char* family;
};

constexpr GenericFamily kFallbackGenerics[] = {
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

// A font file that the bundles fall back to: the family that it holds, the generic family whose style it is drawn in,
// and the locales that it is made for (PrFontFallbackFamily).
//
// In kFallback, Latin, Greek and Cyrillic come first, because the CJK families cover them too. Each script's sans
// family comes before its serif one. Simplified Chinese comes first among the CJK families, and Han text of no language
// uses it. Last Resort comes last, and serves every code point with the glyph of its block. The CJK families are made
// for the writing systems that they draw, regardless of the language.
struct FallbackFont {
    const char* file;
    const char* family;
    PrFontGeneric generic = PrFontGeneric_None;
    PrLocale locale = 0;
};

constexpr FallbackFont kFallback[] = {
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
    {"NotoSansSC-VF.otf", "Noto Sans SC", kSans, kHans},
    {"NotoSansTC-VF.otf", "Noto Sans TC", kSans, kHant},
    {"NotoSansHK-VF.otf", "Noto Sans HK", kSans, kHantHongKong},
    {"NotoSansJP-VF.otf", "Noto Sans JP", kSans, kJapanese},
    {"NotoSansKR-VF.otf", "Noto Sans KR", kSans, kKorean},
    {"NotoSansMath-Regular.ttf", "Noto Sans Math", kSans},
    {"NotoSansSymbols[wght].ttf", "Noto Sans Symbols", kSans},
    {"NotoSansSymbols2-Regular.ttf", "Noto Sans Symbols 2", kSans},
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
    {"NotoSerifSC-VF.otf", "Noto Serif SC", kSerif, kHans},
    {"NotoSerifTC-VF.otf", "Noto Serif TC", kSerif, kHant},
    {"NotoSerifHK-VF.otf", "Noto Serif HK", kSerif, kHantHongKong},
    {"NotoSerifJP-VF.otf", "Noto Serif JP", kSerif, kJapanese},
    {"NotoSerifKR-VF.otf", "Noto Serif KR", kSerif, kKorean},
    {"LastResort-Regular.ttf", "Last Resort", PrFontGeneric_None},
};

// The fonts that a bundle names, which it installs before the fallback fonts.
struct Bundle {
    const NamedFont* named;
    uint32_t count;
};

Bundle bundleOf(PrFontBundle bundle) {
    if (bundle == PrFontBundle_Standard14)
        return Bundle{kStandard14, uint32_t(countOf(kStandard14))};
    return Bundle{kNoto, uint32_t(countOf(kNoto))};
}

// Returns the index of the bundle's named font of the file `file`, or the bundle's count if it names none.
uint32_t namedIndex(const Bundle& bundle, const char* file) {
    for (uint32_t i = 0; i < bundle.count; ++i) {
        if (PrStringView(bundle.named[i].file) == file)
            return i;
    }
    return bundle.count;
}

// Adds the font file `file` of `directory` to `set`. Returns false if it does not load.
bool add(PrFontSet* set, PrStringView directory, const char* file) {
    PrString path = prStringCopy(directory);
    prStringAppend(&path, "/");
    prStringAppend(&path, file);
    const PrFont* PR_NULLABLE font = nullptr;
    PrStatus status = prFontCreateFromFile(prStringView(&path), 0, nullptr, &font);
    prStringReset(&path);
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

uint32_t prFontBundleInstall(PrFontSet* set, PrFontBundle bundle, PrStringView directory) {
    Bundle s = bundleOf(bundle);
    uint32_t added = 0;
    // For each named font, true if its file was added. The fallback may name the same files.
    bool namedAdded[countOf(kStandard14) > countOf(kNoto) ? countOf(kStandard14) : countOf(kNoto)] = {};
    for (uint32_t i = 0; i < s.count; ++i) {
        const NamedFont& entry = s.named[i];
        if (!add(set, directory, entry.file))
            continue;
        ++added;
        namedAdded[i] = true;
        if (entry.generic != PrFontGeneric_None)
            prFontSetSetGeneric(set, entry.generic, entry.family);
        if (entry.alias != nullptr)
            prFontSetAlias(set, entry.alias, entry.family);
    }
    prFontSetSetDefaultFamily(set, prFontFamilyGeneric(PrFontGeneric_Serif));
    PrFontFallbackFamily families[countOf(kFallback)];
    uint32_t count = 0;
    for (const FallbackFont& entry : kFallback) {
        uint32_t named = namedIndex(s, entry.file);
        if (named != s.count ? !namedAdded[named] : !add(set, directory, entry.file))
            continue;
        added += named != s.count ? 0 : 1;
        if (count == 0 || families[count - 1].family.data != entry.family)
            families[count++] = PrFontFallbackFamily{entry.family, entry.locale, entry.generic};
    }
    prFontSetSetFallbackFamilies(set, families, count);
    for (const GenericFamily& binding : kFallbackGenerics)
        prFontSetSetGeneric(set, binding.generic, binding.family);
    return added;
}

uint32_t prFontBundleFileCount(PrFontBundle bundle) {
    Bundle s = bundleOf(bundle);
    uint32_t count = s.count;
    for (const FallbackFont& entry : kFallback)
        count += namedIndex(s, entry.file) != s.count ? 0 : 1;
    return count;
}
