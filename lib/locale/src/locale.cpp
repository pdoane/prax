// Languages, scripts and regions, and the locales that BCP 47 language tags resolve to. A tag must be well-formed by
// RFC 5646. Its locale is made canonical by the IANA Language Subtag Registry and CLDR's aliases, and maximized by the
// likely subtags of UTS #35.
//
// rfc5646 - https://www.rfc-editor.org/rfc/rfc5646

#include "pr/locale.h"

#include "pr/ascii.h"
#include "pr/assert.h"

#include "locale/src/locale_data.h"

namespace pr_impl {
namespace {

// The subtags of a tag, in order, split at its hyphens.
struct Subtags {
    PrStringView tag;
    size_t at;

    // Writes the next subtag to `*out` and returns true, or returns false if there are no more subtags. A tag that ends
    // in a hyphen, or holds two hyphens together, has an empty subtag there.
    bool next(PrStringView* out) {
        if (at > tag.size)
            return false;
        size_t end = at;
        while (end < tag.size && tag.data[end] != '-')
            ++end;
        *out = PrStringView{tag.data + at, end - at};
        at = end + 1;
        return true;
    }
};

bool allLetters(PrStringView text) {
    for (size_t i = 0; i < text.size; ++i) {
        if (!asciiIsLetter(text.data[i]))
            return false;
    }
    return true;
}

bool allDigits(PrStringView text) {
    for (size_t i = 0; i < text.size; ++i) {
        if (!asciiIsDigit(text.data[i]))
            return false;
    }
    return true;
}

// Returns true if a script code is one of ISO 15924's private-use codes, Qaaa through Qabx.
bool privateScript(PrStringView code) {
    char third = asciiLower(code.data[2]);
    char fourth = asciiLower(code.data[3]);
    return asciiLower(code.data[0]) == 'q' && asciiLower(code.data[1]) == 'a'
           && (third == 'a' || (third == 'b' && fourth <= 'x'));
}

// Returns true if a region subtag is one of the registry's private-use ranges, QM through QZ and XA through XZ.
bool privateRegion(PrStringView subtag) {
    if (subtag.size != 2)
        return false;
    char first = asciiLower(subtag.data[0]);
    char second = asciiLower(subtag.data[1]);
    return (first == 'q' && second >= 'm') || first == 'x';
}

// Returns the locale with its likely script and region filled in if it lacks them (UTS #35, "Likely Subtags"). The keys
// tried, in order, are: language, script and region; language and region; language and script; language; and und with
// the script, if the locale has a script. The first key that the likely subtags give a match for supplies the missing
// subtags. The locale's own subtags take precedence over those of the match.
PrLocale maximize(PrLanguage language, PrScript script, PrRegion region) {
    static const PrLanguage undetermined = languageFromSubtag("und");
    const PrLocale keys[] = {prLocaleMake(language, script, region), prLocaleMake(language, PrScript_Zzzz, region),
                             prLocaleMake(language, script, PrRegion_None),
                             prLocaleMake(language, PrScript_Zzzz, PrRegion_None),
                             script != PrScript_Zzzz ? prLocaleMake(undetermined, script, PrRegion_None) : 0};
    for (PrLocale key : keys) {
        if (key == 0)
            continue;
        PrLocale match = localeLikelySubtags(key);
        if (match == 0)
            continue;
        return prLocaleMake(language != undetermined ? language : prLocaleLanguage(match),
                            script != PrScript_Zzzz ? script : prLocaleScript(match),
                            region != PrRegion_None ? region : prLocaleRegion(match));
    }
    return prLocaleMake(language, script, region);
}

// Appends `text` to `out` at `*at`.
void append(char* out, uint32_t* at, PrStringView text) {
    for (size_t i = 0; i < text.size; ++i)
        out[(*at)++] = text.data[i];
}

} // namespace
} // namespace pr_impl

using namespace pr_impl;

PrLanguage prLanguageFromSubtag(PrStringView subtag) {
    return languageFromSubtag(subtag);
}

PrScript prScriptFromCode(PrStringView code) {
    PrScript script = PrScript_Zzzz;
    return scriptFromCode(code, &script) ? script : PrScript_Zzzz;
}

PrRegion prRegionFromSubtag(PrStringView subtag) {
    return regionFromSubtag(subtag);
}

PrStringView prLanguageSubtag(PrLanguage language) {
    return languageSubtag(language);
}

PrStringView prScriptCode(PrScript script) {
    PR_ASSERT(script < kScriptCount);
    return scriptCode(script);
}

PrStringView prRegionSubtag(PrRegion region) {
    return regionSubtag(region);
}

PrLocale prLocaleFromTag(PrStringView tag) {
    // If the registry lists the whole tag, the tag is replaced by the registry's replacement. If the registry gives no
    // replacement, the tag names no locale.
    PrStringView replacement{};
    if (localeWholeTagReplacement(tag, &replacement)) {
        if (replacement.size == 0)
            return 0;
        tag = replacement;
    }
    Subtags subtags{tag, 0};
    PrStringView part{};
    // The registry lists only languages of two and three letters. It lists none of four letters, which are reserved for
    // future use, and none of the five to eight letters that it may register.
    if (!subtags.next(&part) || part.size < 2 || part.size > 3 || !allLetters(part))
        return 0;
    PrLanguage language = languageFromSubtag(part);
    if (language == PrLanguage_None)
        return 0;
    PrScript script = PrScript_Zzzz;
    PrRegion region = PrRegion_None;
    // The subtags resolve in order. The first that is ill-formed or that the registry does not list ends the tag, which
    // keeps what resolved before it, as a lookup that cuts the tag there finds (rfc4647 section 3.4). Variants,
    // extensions and private use are dropped, so the region is the last subtag that resolves.
    bool more = subtags.next(&part);
    // An extlang whose prefix is the language is a language of its own, and it replaces the pair (rfc5646 section 4.5).
    if (more && part.size == 3 && allLetters(part)) {
        PrLanguage prefix = PrLanguage_None;
        PrLanguage extlang =
            languageFromExtlang(part, &prefix) && prefix == language ? languageFromSubtag(part) : PrLanguage_None;
        more = extlang != PrLanguage_None && subtags.next(&part);
        language = extlang != PrLanguage_None ? extlang : language;
    }
    if (more && part.size == 4 && allLetters(part))
        more = (privateScript(part) || scriptFromCode(part, &script)) && subtags.next(&part);
    if (more && ((part.size == 2 && allLetters(part)) || (part.size == 3 && allDigits(part))) && !privateRegion(part))
        region = regionFromSubtag(part);
    // CLDR's aliases replace a language with the locale that CLDR's data uses for it: `cmn` becomes `zh`, and `sh`
    // becomes `sr-Latn`.
    if (PrLocale alias = languageAlias(language)) {
        language = prLocaleLanguage(alias);
        script = script != PrScript_Zzzz ? script : prLocaleScript(alias);
        region = region != PrRegion_None ? region : prLocaleRegion(alias);
    }
    return maximize(language, script, region);
}

uint32_t prLocaleTag(PrLocale locale, char out[PR_NONNULL kPrLocaleMaxTag]) {
    uint32_t at = 0;
    if (prLocaleLanguage(locale) == PrLanguage_None)
        return 0;
    append(out, &at, languageSubtag(prLocaleLanguage(locale)));
    if (prLocaleScript(locale) != PrScript_Zzzz) {
        out[at++] = '-';
        append(out, &at, scriptCode(prLocaleScript(locale)));
    }
    if (prLocaleRegion(locale) != PrRegion_None) {
        out[at++] = '-';
        append(out, &at, regionSubtag(prLocaleRegion(locale)));
    }
    return at;
}

uint32_t prScriptOpenTypeTags(PrScript script, uint32_t out[PR_NONNULL kPrScriptMaxOpenTypeTags]) {
    PR_ASSERT(script < kScriptCount);
    uint32_t count = 0;
    for (uint32_t tag : kScriptOpenType[script]) {
        if (tag != 0)
            out[count++] = tag;
    }
    return count;
}

uint32_t prLocaleOpenTypeLanguages(PrLocale locale, uint32_t* PR_NULLABLE out, uint32_t capacity) {
    uint32_t first = 0;
    uint32_t end = 0;
    languageSystems(prLocaleLanguage(locale), &first, &end);
    PrScript script = prLocaleScript(locale);
    PrRegion region = prLocaleRegion(locale);
    // List the systems for the locale's script and region first, then those for its script, then those for any script.
    uint32_t total = 0;
    for (uint32_t pass = 0; pass < 3; ++pass) {
        for (uint32_t i = first; i < end; ++i) {
            PrScript forScript = prLocaleScript(kSystemQualifier[i]);
            PrRegion forRegion = prLocaleRegion(kSystemQualifier[i]);
            bool applies = pass == 0   ? forScript != PrScript_Zzzz && forRegion != PrRegion_None
                                             && forScript == script && forRegion == region
                           : pass == 1 ? forScript != PrScript_Zzzz && forRegion == PrRegion_None && forScript == script
                                       : forScript == PrScript_Zzzz;
            if (!applies)
                continue;
            if (out != nullptr && total < capacity)
                out[total] = kSystemTag[i];
            ++total;
        }
    }
    return total;
}

PrStringView prLocaleQuote(PrLocale locale, uint32_t depth, bool open) {
    const PrStringView* marks = localeQuoteMarks(locale);
    if (marks == nullptr)
        marks = localeQuoteMarks(prLocaleMake(prLocaleLanguage(locale), prLocaleScript(locale), PrRegion_None));
    if (marks == nullptr)
        marks = localeRootQuoteMarks();
    return marks[(depth == 0 ? 0 : 2) + (open ? 0 : 1)];
}
