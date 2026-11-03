// SVG fonts: reading a font element, and matching and kerning its glyphs.

#include "svg/src/svg_font.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/float.h"
#include "pr/number_list.h"
#include "pr/path_data.h"
#include "pr/string_ops.h"
#include "pr/utf8.h"

#include "dom/src/dom_node.h"
#include "svg/src/svg_parse.h"
#include "svg/src/svg_tree.h"

namespace pr_impl {

namespace {

// The number of font units per em for a font that does not specify it.
constexpr float kDefaultUnitsPerEm = 1000;

bool isSvgElement(const PrDomElement* element, PrDomTag tag) {
    return element->space == PrDomNamespace_Svg && element->tag == tag;
}

float numberAttribute(const PrDomElement* element, PrStringView name, float fallback) {
    PrStringView text = svgAttribute(element, name);
    size_t pos = 0;
    float value = 0;
    return text.data != nullptr && numberListRead(text, &pos, &value) ? value : fallback;
}

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

PrStringView trimmed(PrStringView text) {
    while (text.size != 0 && isSpace(text.data[0])) {
        ++text.data;
        --text.size;
    }
    while (text.size != 0 && isSpace(text.data[text.size - 1]))
        --text.size;
    return text;
}

// Calls `each` with each item of the comma-separated list, trimmed, until it returns true.
template <typename Each>
bool anyItem(PrStringView list, Each each) {
    size_t start = 0;
    for (size_t i = 0; i <= list.size; ++i) {
        if (i < list.size && list.data[i] != ',')
            continue;
        PrStringView item = trimmed(PrStringView{list.data + start, i - start});
        if (item.size != 0 && each(item))
            return true;
        start = i + 1;
    }
    return false;
}

// Parses one range in the unicode-range syntax: U+hex, U+hex-hex, or U+hex with trailing `?`s.
bool parseRange(PrStringView item, PrFontCodePointRange* out) {
    if (item.size < 3 || (item.data[0] != 'U' && item.data[0] != 'u') || item.data[1] != '+')
        return false;
    size_t i = 2;
    uint32_t first = 0;
    uint32_t last = 0;
    uint32_t digits = 0;
    for (; i < item.size && prAsciiIsHexDigit(item.data[i]) && digits < 6; ++i, ++digits) {
        first = first * 16 + prAsciiHexValue(item.data[i]);
        last = last * 16 + prAsciiHexValue(item.data[i]);
    }
    uint32_t wild = 0;
    for (; i < item.size && item.data[i] == '?' && digits < 6; ++i, ++digits, ++wild) {
        first = first * 16;
        last = last * 16 + 15;
    }
    if (digits == 0)
        return false;
    if (i < item.size && item.data[i] == '-' && wild == 0) {
        ++i;
        // The end may repeat the U+. The SVG 1.1 test suite writes ranges this way.
        if (i + 1 < item.size && (item.data[i] == 'U' || item.data[i] == 'u') && item.data[i + 1] == '+')
            i += 2;
        last = 0;
        digits = 0;
        for (; i < item.size && prAsciiIsHexDigit(item.data[i]) && digits < 6; ++i, ++digits)
            last = last * 16 + prAsciiHexValue(item.data[i]);
        if (digits == 0)
            return false;
    }
    if (i != item.size || last < first)
        return false;
    *out = PrFontCodePointRange{first, last};
    return true;
}

// Returns true if a kerning list of characters and ranges includes the glyph's characters.
bool unicodeNames(PrStringView list, const SvgGlyph& glyph) {
    if (list.data == nullptr || glyph.unicode.size == 0)
        return false;
    uint32_t codePoint = 0;
    size_t length = prUtf8Decode(glyph.unicode, 0, &codePoint);
    bool single = length == glyph.unicode.size;
    return anyItem(list, [&](PrStringView item) {
        PrFontCodePointRange range{};
        if (single && parseRange(item, &range))
            return codePoint >= range.first && codePoint <= range.last;
        return item == glyph.unicode;
    });
}

bool glyphNames(PrStringView list, const SvgGlyph& glyph) {
    if (list.data == nullptr || glyph.names.data == nullptr)
        return false;
    return anyItem(list, [&](PrStringView item) {
        return anyItem(glyph.names, [&](PrStringView name) { return item == name; });
    });
}

bool languageMatches(PrStringView languages, PrStringView language) {
    if (languages.data == nullptr)
        return true;
    return anyItem(languages, [&](PrStringView tag) {
        return language.size >= tag.size && prAsciiEqualsFold(PrStringView{language.data, tag.size}, tag) &&
            (language.size == tag.size || language.data[tag.size] == '-');
    });
}

void readGlyph(const PrDomElement* element, float fallbackAdvance, SvgGlyph* out) {
    SvgGlyph glyph{};
    // The characters are the attribute's value exactly, including white space.
    PrStringView unicode = domAttributeValue(element, "unicode");
    glyph.unicode = unicode.data != nullptr ? unicode : PrStringView{""};
    glyph.names = svgAttribute(element, "glyph-name");
    glyph.languages = svgAttribute(element, "lang");
    PrStringView form = svgAttribute(element, "arabic-form");
    if (form == "isolated")
        glyph.arabicForm = SvgArabicForm_Isolated;
    else if (form == "initial")
        glyph.arabicForm = SvgArabicForm_Initial;
    else if (form == "medial")
        glyph.arabicForm = SvgArabicForm_Medial;
    else if (form == "terminal")
        glyph.arabicForm = SvgArabicForm_Terminal;
    else
        glyph.arabicForm = SvgArabicForm_Any;
    glyph.advance = numberAttribute(element, "horiz-adv-x", fallbackAdvance);
    for (const PrDomNode* child = element->first; child != nullptr; child = child->next)
        if (domElementFromNode(child) != nullptr)
            glyph.content = element;
    PrStringView d = svgAttribute(element, "d");
    if (d.data != nullptr) {
        glyph.path = prPathCreate();
        prPathClear(glyph.path);
        prPathAppendData(glyph.path, d);
        if (prPathVerbCount(glyph.path) == 0) {
            prPathDestroy(glyph.path);
            glyph.path = nullptr;
        }
    }
    *out = glyph;
}

// Reads the family of a font-face's font-family attribute, the first of its list, without quotes.
bool firstFamily(PrStringView list, PrStringView* family) {
    size_t i = 0;
    while (i < list.size && (prAsciiIsSpace(list.data[i]) || list.data[i] == ','))
        ++i;
    if (i >= list.size)
        return false;
    char quote = list.data[i];
    if (quote == '"' || quote == '\'') {
        size_t start = ++i;
        while (i < list.size && list.data[i] != quote)
            ++i;
        *family = PrStringView{list.data + start, i - start};
        return true;
    }
    size_t start = i;
    while (i < list.size && list.data[i] != ',')
        ++i;
    *family = prStringTrimEnd(prStringSlice(list, start, i - start));
    return true;
}

void noteFamily(const SvgDocumentFonts* fonts, uint32_t hash) {
    if (fonts->changed != nullptr)
        fonts->changed(fonts->changedContext, hash);
}

void resetFace(SvgFontFace* face) {
    prRawStringReset(&face->family);
    face->ranges.reset();
}

// Adds a font-face element of the document's tree, if it names a family, where tree order puts it.
void addFace(SvgDocumentFonts* fonts, const PrDomElement* element) {
    PrStringView family{};
    if (domNodeRoot(element) != fonts->document || !firstFamily(svgAttribute(element, "font-family"), &family))
        return;
    SvgFontFace face{};
    face.family = prRawStringCopy(family);
    face.familyHash = svgFontFamilyHash(family);
    face.element = element;
    PrStringView ranges = svgAttribute(element, "unicode-range");
    if (ranges.data != nullptr && !svgParseUnicodeRange(ranges, &face.ranges))
        face.ranges.clear();
    uint32_t at = fonts->faces.count;
    while (at > 0 && domNodePrecedes(element, fonts->faces.data[at - 1].element))
        --at;
    fonts->faces.insert(at, face);
    noteFamily(fonts, face.familyHash);
}

void removeFace(SvgDocumentFonts* fonts, const PrDomElement* element) {
    for (uint32_t i = 0; i < fonts->faces.count; ++i) {
        SvgFontFace& face = fonts->faces.data[i];
        if (face.element != element)
            continue;
        uint32_t hash = face.familyHash;
        resetFace(&face);
        fonts->faces.erase(i);
        noteFamily(fonts, hash);
        return;
    }
}

// Forgets what was read of the font element `element`, and what the faces found of it.
void forgetFont(SvgDocumentFonts* fonts, const PrDomElement* element) {
    for (uint32_t i = 0; i < fonts->fonts.count; ++i) {
        SvgFont* font = fonts->fonts.data[i];
        if (font->element != element)
            continue;
        for (uint32_t k = 0; k < fonts->faces.count; ++k) {
            SvgFontFace& face = fonts->faces.data[k];
            if (face.font == font) {
                face.font = nullptr;
                face.resolved = false;
                noteFamily(fonts, face.familyHash);
            }
        }
        svgFontReset(font);
        prMemoryFree(font);
        fonts->fonts.erase(i);
        break;
    }
    // A face inside the font names it whether or not text has read it yet.
    for (uint32_t k = 0; k < fonts->faces.count; ++k) {
        const SvgFontFace& face = fonts->faces.data[k];
        if (face.element->parent == element)
            noteFamily(fonts, face.familyHash);
    }
}

// Looks again for the fonts of the faces that name theirs by a font-face-uri, which may name another element now.
void retargetUris(SvgDocumentFonts* fonts) {
    for (uint32_t i = 0; i < fonts->faces.count; ++i) {
        SvgFontFace& face = fonts->faces.data[i];
        const PrDomElement* parent = domElementFromNode(face.element->parent);
        if (parent != nullptr && svgIs(parent, PrDomTag_Font))
            continue;
        face.font = nullptr;
        face.resolved = false;
        noteFamily(fonts, face.familyHash);
    }
}

// Notes a change at `element` or under it: of the font or the face around it, whose glyphs or sources it may be.
void changedAt(SvgDocumentFonts* fonts, const PrDomElement* PR_NULLABLE element) {
    for (const PrDomElement* at = element; at != nullptr && at->space == PrDomNamespace_Svg;
            at = domElementFromNode(at->parent)) {
        if (at->tag == PrDomTag_Font) {
            forgetFont(fonts, at);
            return;
        }
        if (at->tag == PrDomTag_FontFace) {
            for (uint32_t i = 0; i < fonts->faces.count; ++i) {
                SvgFontFace& face = fonts->faces.data[i];
                if (face.element == at) {
                    face.font = nullptr;
                    face.resolved = false;
                    noteFamily(fonts, face.familyHash);
                }
            }
        }
    }
}

} // namespace

void svgFontLoad(const PrDomElement* font, SvgFont* out) {
    out->element = font;
    out->unitsPerEm = kDefaultUnitsPerEm;
    float advance = numberAttribute(font, "horiz-adv-x", 0);
    const PrDomElement* face = nullptr;
    const PrDomElement* missing = nullptr;
    for (const PrDomNode* child = font->first; child != nullptr; child = child->next) {
        const PrDomElement* e = domElementFromNode(child);
        if (e == nullptr)
            continue;
        if (face == nullptr && isSvgElement(e, PrDomTag_FontFace))
            face = e;
        if (missing == nullptr && isSvgElement(e, PrDomTag_MissingGlyph))
            missing = e;
    }
    if (face != nullptr) {
        float units = numberAttribute(face, "units-per-em", kDefaultUnitsPerEm);
        out->unitsPerEm = units > 0 ? units : kDefaultUnitsPerEm;
    }
    // Without a font-face, the ascent and descent use SVG 1.1's defaults: the font's height above the baseline, and
    // nothing below.
    out->ascent = face != nullptr ? numberAttribute(face, "ascent", out->unitsPerEm) : out->unitsPerEm;
    out->descent = face != nullptr ? prF32Abs(numberAttribute(face, "descent", 0)) : 0;
    // If the font-face does not place the decoration lines, the underline is a tenth of an em below the baseline, the
    // overline is at the ascent, and the line-through is a third of an em up. Each is a twentieth of an em thick.
    auto metric = [&](PrStringView name, float fallback) {
        return face != nullptr ? numberAttribute(face, name, fallback) : fallback;
    };
    float em = out->unitsPerEm;
    // A font-face's baselines are relative to the glyphs' origin, which is the alphabetic baseline by default. The
    // defaults are css-inline-3's synthesized baselines.
    float alphabetic = metric("alphabetic", 0);
    out->ideographic = metric("ideographic", -out->descent) - alphabetic;
    out->hanging = metric("hanging", out->ascent * 0.8f) - alphabetic;
    out->mathematical = metric("mathematical", out->ascent / 2) - alphabetic;
    out->xHeight = metric("x-height", em / 2);
    out->underlineThickness = metric("underline-thickness", em / 20);
    out->underlinePosition = metric("underline-position", -em / 10);
    out->overlineThickness = metric("overline-thickness", em / 20);
    out->overlinePosition = metric("overline-position", out->ascent);
    out->strikethroughThickness = metric("strikethrough-thickness", em / 20);
    out->strikethroughPosition = metric("strikethrough-position", em / 3);
    SvgGlyph missingGlyph{};
    missingGlyph.unicode = "";
    missingGlyph.advance = advance;
    if (missing != nullptr)
        readGlyph(missing, advance, &missingGlyph);
    missingGlyph.unicode = "";
    out->glyphs.push(missingGlyph);
    for (const PrDomNode* child = font->first; child != nullptr; child = child->next) {
        const PrDomElement* e = domElementFromNode(child);
        if (e == nullptr)
            continue;
        if (isSvgElement(e, PrDomTag_Glyph)) {
            SvgGlyph glyph{};
            readGlyph(e, advance, &glyph);
            out->glyphs.push(glyph);
        } else if (isSvgElement(e, PrDomTag_Hkern)) {
            SvgKern kern{};
            kern.u1 = svgAttribute(e, "u1");
            kern.g1 = svgAttribute(e, "g1");
            kern.u2 = svgAttribute(e, "u2");
            kern.g2 = svgAttribute(e, "g2");
            kern.k = numberAttribute(e, "k", 0);
            out->kerns.push(kern);
        }
    }
}

void svgFontReset(SvgFont* font) {
    for (uint32_t i = 0; i < font->glyphs.count; ++i)
        prPathDestroy(font->glyphs.data[i].path);
    font->glyphs.reset();
    font->kerns.reset();
}

uint32_t svgFontMatch(const SvgFont& font, PrStringView text, size_t pos, PrStringView language,
        SvgArabicForm form, size_t* length) {
    for (uint32_t i = 1; i < font.glyphs.count; ++i) {
        const SvgGlyph& glyph = font.glyphs.data[i];
        PrStringView unicode = glyph.unicode;
        if (unicode.size == 0 || unicode.size > text.size - pos ||
                PrStringView{text.data + pos, unicode.size} != unicode || !languageMatches(glyph.languages, language) ||
                (form != SvgArabicForm_Any && glyph.arabicForm != SvgArabicForm_Any && glyph.arabicForm != form))
            continue;
        *length = unicode.size;
        return i;
    }
    uint32_t codePoint = 0;
    *length = prUtf8Decode(text, pos, &codePoint);
    return kSvgMissingGlyph;
}

float svgFontKerning(const SvgFont& font, uint32_t first, uint32_t second) {
    const SvgGlyph& a = font.glyphs.data[first];
    const SvgGlyph& b = font.glyphs.data[second];
    for (uint32_t i = 0; i < font.kerns.count; ++i) {
        const SvgKern& kern = font.kerns.data[i];
        if ((unicodeNames(kern.u1, a) || glyphNames(kern.g1, a)) && (unicodeNames(kern.u2, b) || glyphNames(kern.g2, b)))
            return kern.k;
    }
    return 0;
}

SvgDocumentFonts* svgDocumentFontsCreate(PrDomDocument* document) {
    SvgDocumentFonts* fonts = allocZeroed<SvgDocumentFonts>();
    fonts->document = document;
    svgDocumentFontsInserted(fonts, document);
    return fonts;
}

void svgDocumentFontsDestroy(SvgDocumentFonts* fonts) {
    for (uint32_t i = 0; i < fonts->fonts.count; ++i) {
        svgFontReset(fonts->fonts.data[i]);
        prMemoryFree(fonts->fonts.data[i]);
    }
    fonts->fonts.reset();
    for (uint32_t i = 0; i < fonts->faces.count; ++i)
        resetFace(&fonts->faces.data[i]);
    fonts->faces.reset();
    prMemoryFree(fonts);
}

void svgDocumentFontsInserted(SvgDocumentFonts* fonts, PrDomNode* node) {
    bool named = false;
    for (PrDomNode* at = node; at != nullptr; at = domNodeNext(node, at)) {
        const PrDomElement* element = domElementFromNode(at);
        if (element == nullptr || element->space != PrDomNamespace_Svg)
            continue;
        if (element->tag == PrDomTag_FontFace)
            addFace(fonts, element);
        named = named || element->tag == PrDomTag_Font;
    }
    if (fonts->faces.count == 0)
        return;
    if (named)
        retargetUris(fonts);
    if (node->parent != nullptr)
        changedAt(fonts, domElementFromNode(node->parent));
}

void svgDocumentFontsRemoving(SvgDocumentFonts* fonts, PrDomNode* node) {
    if (fonts->faces.count == 0 && fonts->fonts.count == 0)
        return;
    for (PrDomNode* at = node; at != nullptr; at = domNodeNext(node, at)) {
        const PrDomElement* element = domElementFromNode(at);
        if (element == nullptr || element->space != PrDomNamespace_Svg)
            continue;
        if (element->tag == PrDomTag_FontFace)
            removeFace(fonts, element);
        else if (element->tag == PrDomTag_Font)
            forgetFont(fonts, element);
    }
    if (node->parent != nullptr)
        changedAt(fonts, domElementFromNode(node->parent));
}

void svgDocumentFontsAttributeChanged(SvgDocumentFonts* fonts, const PrDomElement* element, PrStringView name) {
    if (element->space != PrDomNamespace_Svg || !domIsConnected(element))
        return;
    if (element->tag == PrDomTag_FontFace && (name == "font-family" || name == "unicode-range")) {
        removeFace(fonts, element);
        addFace(fonts, element);
    }
    if (fonts->faces.count == 0)
        return;
    if (element->tag == PrDomTag_Font && name == "id")
        retargetUris(fonts);
    changedAt(fonts, element);
}

void svgDocumentFontsResourceLoaded(SvgDocumentFonts* fonts) {
    retargetUris(fonts);
}

void svgDocumentFontsSetChanged(SvgDocumentFonts* fonts, SvgFontFamilyChangedFn PR_NULLABLE changed, void* PR_NULLABLE context) {
    fonts->changed = changed;
    fonts->changedContext = context;
}

const SvgFont* svgDocumentFontsLoad(SvgDocumentFonts* fonts, const PrDomElement* element) {
    for (uint32_t i = 0; i < fonts->fonts.count; ++i) {
        if (fonts->fonts.data[i]->element == element)
            return fonts->fonts.data[i];
    }
    SvgFont* font = allocZeroed<SvgFont>();
    svgFontLoad(element, font);
    fonts->fonts.push(font);
    return font;
}

uint32_t svgFontFamilyHash(PrStringView family) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < family.size; ++i) {
        char c = family.data[i];
        hash = (hash ^ uint8_t(c >= 'A' && c <= 'Z' ? c + 32 : c)) * 16777619u;
    }
    return hash;
}

bool svgParseUnicodeRange(PrStringView text, RawArray<PrFontCodePointRange>* out) {
    out->clear();
    bool valid = true;
    anyItem(text, [&](PrStringView item) {
        PrFontCodePointRange range{};
        valid = parseRange(item, &range);
        if (valid)
            out->push(range);
        return !valid;
    });
    return valid && out->count != 0;
}

} // namespace pr_impl
