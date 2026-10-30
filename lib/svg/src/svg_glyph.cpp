// The glyph decoder of SVG, which draws the SVG documents of glyphs, and their images in bitmap strikes.
//
// opentype - https://learn.microsoft.com/en-us/typography/opentype/spec/svg

#include "pr/svg_glyph.h"

#include "pr/alloc.h"
#include "pr/array.h"
#include "pr/css.h"
#include "pr/css_presentation.h"
#include "pr/dom.h"
#include "pr/dom_xml.h"
#include "pr/draw_commands.h"
#include "pr/svg.h"
#include "pr/svg_render.h"

namespace pr_impl {

namespace {

// The number of parsed documents that the decoder keeps. The least recently used document is dropped first.
constexpr uint32_t kDocumentCacheSize = 8;

// The largest document that the decoder inflates, as a multiple of its compressed size.
constexpr size_t kMaxInflation = 64;

// A document parsed from a font, keyed by the font and the document's index in it.
struct ParsedDocument {
    uint64_t font;
    uint32_t index;
    PrDomDocument* PR_NULLABLE document;    // null for a document that did not parse
    uint64_t used;
};

struct SvgDecoder : PrDrawGlyphDecoder {
    PrImageCodec* PR_NULLABLE imageCodec;
    PrCompressCodec* PR_NULLABLE compressCodec;
    Array<ParsedDocument> documents;
    uint64_t clock;
    Array<uint8_t> inflated;
    Array<char> text;                       // the palette's custom properties, with names and values end to end
    Array<PrSvgCustomProperty> properties;
};

// Returns the document's text: its bytes, inflated if they are gzip-compressed. Returns the null view if they cannot be
// inflated.
PrStringView documentText(SvgDecoder& d, PrByteView bytes) {
    bool gzip = bytes.size >= 2 && bytes.data[0] == 0x1f && bytes.data[1] == 0x8b;
    if (!gzip)
        return PrStringView{reinterpret_cast<const char*>(bytes.data), bytes.size};
    if (d.compressCodec == nullptr)
        return PrStringView{};
    for (size_t capacity = bytes.size * 4; capacity <= bytes.size * kMaxInflation; capacity *= 2) {
        d.inflated.clear();
        d.inflated.reserve(uint32_t(capacity));
        PrCompressRead read{};
        PrStatus status = prCompressCodecDecode(d.compressCodec, PrCompressFormat_Gzip, bytes, d.inflated.data,
            capacity, &read);
        if (status != PrStatus_Ok)
            return PrStringView{};
        if (read.progress == PrCompressProgress_Done)
            return PrStringView{reinterpret_cast<const char*>(d.inflated.data), read.written};
    }
    return PrStringView{};
}

// Returns the parsed document of the glyph, which is parsed on first use. Returns null if it does not parse.
PrDomDocument* PR_NULLABLE documentOf(SvgDecoder& d, const PrFont* font, const PrFontGlyphSvg& svg) {
    uint64_t id = prFontId(font);
    ++d.clock;
    for (uint32_t i = 0; i < d.documents.count; ++i) {
        ParsedDocument& parsed = d.documents.data[i];
        if (parsed.font == id && parsed.index == svg.index) {
            parsed.used = d.clock;
            return parsed.document;
        }
    }
    PrStringView text = documentText(d, svg.document);
    PrDomDocument* document = nullptr;
    if (text.data != nullptr) {
        PrXmlError error{};
        document = prDomParseXml(text, "image/svg+xml", &error);
        if (error.message.data != nullptr) {
            prDomDocumentRelease(document);
            document = nullptr;
        } else {
            prCssEnable(document);
            prSvgEnable(document);
        }
    }
    ParsedDocument entry{id, svg.index, document, d.clock};
    if (d.documents.count == kDocumentCacheSize) {
        uint32_t oldest = 0;
        for (uint32_t i = 1; i < d.documents.count; ++i) {
            if (d.documents.data[i].used < d.documents.data[oldest].used)
                oldest = i;
        }
        prDomDocumentRelease(d.documents.data[oldest].document);
        d.documents.data[oldest] = entry;
    } else {
        d.documents.push(entry);
    }
    return document;
}

void appendText(Array<char>& out, const char* text) {
    while (*text != 0)
        out.push(*text++);
}

void appendNumber(Array<char>& out, uint32_t value) {
    char digits[10];
    uint32_t count = 0;
    do {
        digits[count++] = char('0' + value % 10);
        value /= 10;
    } while (value != 0);
    while (count > 0)
        out.push(digits[--count]);
}

void appendHex(Array<char>& out, float channel) {
    const char* digits = "0123456789abcdef";
    float clamped = channel < 0 ? 0 : channel > 1 ? 1 : channel;
    uint32_t byte = uint32_t(clamped * 255 + 0.5f);
    out.push(digits[byte >> 4]);
    out.push(digits[byte & 15]);
}

// Sets the properties to the palette's entries, named --color0, --color1 and so on, each with a #rrggbbaa color.
void paletteProperties(SvgDecoder& d, const PrFont* font, const PrFontPalette* PR_NULLABLE palette) {
    uint32_t count = prFontPaletteEntryCount(font);
    d.text.clear();
    d.properties.clear();
    // The names and values are written first, and views of them are taken once the text no longer grows.
    Array<uint32_t> starts;
    for (uint32_t i = 0; i < count; ++i) {
        starts.push(d.text.count);
        appendText(d.text, "--color");
        appendNumber(d.text, i);
        starts.push(d.text.count);
        PrColor color = prFontPaletteEntry(font, palette, i);
        d.text.push('#');
        appendHex(d.text, color.r);
        appendHex(d.text, color.g);
        appendHex(d.text, color.b);
        appendHex(d.text, color.a);
    }
    starts.push(d.text.count);
    for (uint32_t i = 0; i < count; ++i) {
        const char* base = d.text.data;
        uint32_t name = starts.data[i * 2];
        uint32_t value = starts.data[i * 2 + 1];
        uint32_t end = starts.data[i * 2 + 2];
        d.properties.push(PrSvgCustomProperty{
            PrStringView{base + name, value - name},
            PrStringView{base + value, end - value},
        });
    }
    starts.reset();
}

void destroyDecoder(PrDrawGlyphDecoder* decoder) {
    SvgDecoder* d = static_cast<SvgDecoder*>(decoder);
    for (uint32_t i = 0; i < d->documents.count; ++i)
        prDomDocumentRelease(d->documents.data[i].document);
    d->documents.reset();
    d->inflated.reset();
    d->text.reset();
    d->properties.reset();
    prMemoryFree(d);
}

bool decode(PrDrawGlyphDecoder* decoder, const PrFont* font, uint16_t glyph, float pixelsPerEm,
        const PrFontPalette* PR_NULLABLE palette, PrColor foreground, PrDrawList* list) {
    SvgDecoder& d = *static_cast<SvgDecoder*>(decoder);
    PrFontGlyphSvg svg{};
    if (!prFontGlyphSvg(font, glyph, &svg))
        return d.imageCodec != nullptr && prDrawGlyphImage(d.imageCodec, font, glyph, pixelsPerEm, list);
    PrDomDocument* document = documentOf(d, font, svg);
    if (document == nullptr)
        return false;
    // The glyph is the element whose id is "glyph" followed by the glyph's id.
    Array<char> name;
    appendText(name, "glyph");
    appendNumber(name, glyph);
    PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(document), PrStringView{name.data, name.count});
    name.reset();
    PrSvgElement* drawn = element != nullptr ? prSvgElementQuery(prDomElementAsEventTarget(element)) : nullptr;
    if (drawn == nullptr)
        return false;
    paletteProperties(d, font, palette);
    PrSvgDrawOptions options{};
    options.properties = d.properties.data;
    options.propertyCount = d.properties.count;
    options.color = foreground;
    options.hasColor = true;
    uint32_t before = prDrawListCommandCount(list);
    float em = 1 / svg.scale;
    prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{svg.scale, svg.scale}));
    prSvgElementDrawInPlace(drawn, PrVec2{em, em}, &options, list);
    prDrawListPopTransform(list);
    return prDrawListCommandCount(list) > before + 2;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrDrawGlyphDecoder* prSvgGlyphDecoderCreate(PrImageCodec* PR_NULLABLE imageCodec,
        PrCompressCodec* PR_NULLABLE compressCodec) {
    SvgDecoder* decoder = allocZeroed<SvgDecoder>();
    decoder->destroy = destroyDecoder;
    decoder->decode = decode;
    decoder->imageCodec = imageCodec;
    decoder->compressCodec = compressCodec;
    return decoder;
}
