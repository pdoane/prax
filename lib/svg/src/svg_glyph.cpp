// The glyph decoder of SVG, which draws the SVG documents of glyphs, and their images in bitmap strikes.
//
// opentype - https://learn.microsoft.com/en-us/typography/opentype/spec/svg

#include "pr/svg_glyph.h"

#include "pr/alloc.h"
#include "pr/css.h"
#include "pr/css_presentation.h"
#include "pr/dom.h"
#include "pr/dom_xml.h"
#include "pr/draw_commands.h"
#include "pr/draw_glyph_backend.h"
#include "pr/image_texture_source.h"
#include "pr/integer.h"
#include "pr/raw_array.h"
#include "pr/string_ops.h"
#include "pr/svg.h"
#include "pr/svg_render.h"

namespace pr_impl {

namespace {

// The number of parsed documents that the decoder keeps. The least recently used document is dropped first.
constexpr uint32_t kDocumentCacheSize = 8;

// The largest document that the decoder inflates, as a multiple of its compressed size.
constexpr size_t kMaxInflation = 64;

// The number of glyph images whose sources the decoder keeps. The decoder drops the least recently used image first. A
// dropped source stays alive until every use of the glyph cache that can draw it has released it.
constexpr uint32_t kImageCacheSize = 64;

// A glyph's image from a bitmap strike, opened as a source. The font, the glyph and the strike are its key.
struct OpenedImage {
    uint64_t font;
    uint16_t glyph;
    float strike;
    const PrTextureSource* PR_NULLABLE source;  // null for an image that did not open
    uint64_t used;
};

// A document parsed from a font, keyed by the font and the document's index in it.
struct ParsedDocument {
    uint64_t font;
    uint32_t index;
    PrDomDocument* PR_NULLABLE document;    // null for a document that did not parse
    uint64_t used;
};

struct SvgDecoder : PrDrawGlyphDecoder {
    const PrImageCodecSet* PR_NULLABLE imageCodec;
    const PrCompressCodecSet* PR_NULLABLE compressCodec;
    PrTextureCache* PR_NULLABLE textures;
    RawArray<ParsedDocument> documents;
    RawArray<OpenedImage> images;
    uint64_t clock;
    RawArray<uint8_t> inflated;
    RawArray<char> text;                       // the palette's custom properties, with names and values end to end
    RawArray<PrSvgCustomProperty> properties;
};

// Returns the document's text: its bytes, inflated if they are gzip-compressed. Returns the null view if they cannot be
// inflated.
PrStringView documentText(SvgDecoder& d, PrByteView bytes) {
    bool gzip = bytes.size >= 2 && bytes.data[0] == 0x1f && bytes.data[1] == 0x8b;
    if (!gzip)
        return prStringFromBytes(bytes);
    if (d.compressCodec == nullptr)
        return PrStringView{};
    for (size_t capacity = bytes.size * 4; capacity <= bytes.size * kMaxInflation; capacity *= 2) {
        d.inflated.clear();
        d.inflated.reserve(uint32_t(capacity));
        PrCompressRead read{};
        PrStatus status = prCompressCodecSetDecode(d.compressCodec, PrCompressFormat_Gzip, bytes, d.inflated.data,
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

void appendText(RawArray<char>& out, const char* text) {
    while (*text != 0)
        out.push(*text++);
}

void appendNumber(RawArray<char>& out, uint32_t value) {
    char digits[kPrU64TextMaxBytes];
    out.append(digits, uint32_t(prU64Format(value, digits, 10)));
}

void appendHex(RawArray<char>& out, float channel) {
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
    RawArray<uint32_t> starts;
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
    for (const OpenedImage& image : d->images.span())
        prTextureSourceRelease(image.source);
    d->images.reset();
    d->inflated.reset();
    d->text.reset();
    d->properties.reset();
    prMemoryFree(d);
}

// Returns the source of the glyph's image, or null if the image does not open. The image is opened once, and the
// decoder keeps its source in `images`.
const PrTextureSource* PR_NULLABLE imageSourceOf(SvgDecoder& d, const PrFont* font, uint16_t glyph,
        const PrFontGlyphImage& image) {
    uint64_t id = prFontId(font);
    ++d.clock;
    for (OpenedImage& opened : d.images.span()) {
        if (opened.font == id && opened.glyph == glyph && opened.strike == image.pixelsPerEm) {
            opened.used = d.clock;
            return opened.source;
        }
    }
    const PrTextureSource* source = nullptr;
    prImageTextureSourceOpenBytes(image.bytes, d.imageCodec, nullptr, PrImageTextureColor_Srgb, &source);
    if (d.images.count == kImageCacheSize) {
        uint32_t oldest = 0;
        for (uint32_t i = 1; i < d.images.count; ++i) {
            if (d.images.data[i].used < d.images.data[oldest].used)
                oldest = i;
        }
        // A use holds the source while it draws the glyph's region, so the source is destroyed only after every use has
        // released it.
        prTextureSourceRelease(d.images.data[oldest].source);
        d.images.erase(oldest);
    }
    d.images.push(OpenedImage{id, glyph, image.pixelsPerEm, source, d.clock});
    return source;
}

// Draws the glyph's image from the strike for `pixelsPerEm` (prFontGlyphImage), in ems with y down from the glyph's
// origin. The picture holds the image's source, so that the texture cache keeps the region while the picture may draw
// it. The glyph cache keeps the picture only once the image's own texture has arrived. A picture of a preview, or of an
// image that has not arrived, is for one use. The next use asks for the texture again.
PrDrawGlyphDecoded decodeImage(SvgDecoder& d, const PrFont* font, uint16_t glyph, float pixelsPerEm, PrDrawList* list) {
    PrFontGlyphImage image{};
    if (d.imageCodec == nullptr || d.textures == nullptr || !prFontGlyphImage(font, glyph, pixelsPerEm, &image))
        return PrDrawGlyphDecoded_None;
    const PrTextureSource* source = imageSourceOf(d, font, glyph, image);
    if (source == nullptr)
        return PrDrawGlyphDecoded_None;
    // A glyph is drawn at many sizes, so its texture has every mip level.
    PrTextureRegion region{};
    PrTextureCacheState state =
        prTextureCacheRealize(d.textures, source, prTextureCacheAskEncoded(PrTextureCacheFlag_Levels), &region);
    if (state == PrTextureCacheState_Failed)
        return PrDrawGlyphDecoded_None;
    if (state == PrTextureCacheState_Pending)
        return PrDrawGlyphDecoded_Use;
    const PrTextureDesc& desc = *prTextureSourceDesc(source);
    PrUVec2 extent = prTextureDescDisplayedExtent(&desc);
    PrDrawSource drawn = prDrawSourceRegion(&region, PrRect{0, 0, float(extent.x), float(extent.y)}, desc.orientation);
    PrDrawBrush brush = prDrawBrushImage(drawn, image.bounds, PrDrawSampling_Mipmap);
    prDrawBrushImageUpright(&brush, desc.orientation, region.rect);
    // A mirrored image reflects about the vertical line through the middle of its bounds.
    if (image.mirrored)
        prDrawListPushTransformAffine2(list, PrAffine2{-1, 0, 0, 1, image.bounds.left + image.bounds.right, 0});
    prDrawListRect(list, image.bounds, {.brush = &brush});
    if (image.mirrored)
        prDrawListPopTransform(list);
    prDrawListHoldSource(list, source);
    return state == PrTextureCacheState_Ready ? PrDrawGlyphDecoded_Kept : PrDrawGlyphDecoded_Use;
}

PrDrawGlyphDecoded decode(PrDrawGlyphDecoder* decoder, const PrFont* font, uint16_t glyph, float pixelsPerEm,
        const PrFontPalette* PR_NULLABLE palette, PrColor foreground, PrDrawList* list) {
    SvgDecoder& d = *static_cast<SvgDecoder*>(decoder);
    PrFontGlyphSvg svg{};
    if (!prFontGlyphSvg(font, glyph, &svg))
        return decodeImage(d, font, glyph, pixelsPerEm, list);
    PrDomDocument* document = documentOf(d, font, svg);
    if (document == nullptr)
        return PrDrawGlyphDecoded_None;
    // The glyph is the element whose id is "glyph" followed by the glyph's id.
    RawArray<char> name;
    appendText(name, "glyph");
    appendNumber(name, glyph);
    PrDomElement* element = prDomNodeGetElementById(prDomDocumentAsNode(document), PrStringView{name.data, name.count});
    name.reset();
    PrSvgElement* drawn = element != nullptr ? prSvgElementQuery(prDomElementAsEventTarget(element)) : nullptr;
    if (drawn == nullptr)
        return PrDrawGlyphDecoded_None;
    paletteProperties(d, font, palette);
    PrSvgDrawOptions options{};
    options.properties = d.properties.data;
    options.propertyCount = d.properties.count;
    options.color = foreground;
    options.hasColor = true;
    options.textures = d.textures;
    uint32_t before = prDrawListCommandCount(list);
    float em = 1 / svg.scale;
    prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{svg.scale, svg.scale}));
    prSvgElementDrawInPlace(drawn, PrVec2{em, em}, &options, list);
    prDrawListPopTransform(list);
    return prDrawListCommandCount(list) > before + 2 ? PrDrawGlyphDecoded_Kept : PrDrawGlyphDecoded_None;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrDrawGlyphDecoder* prSvgGlyphDecoderCreate(const PrImageCodecSet* PR_NULLABLE imageCodec,
        const PrCompressCodecSet* PR_NULLABLE compressCodec, PrTextureCache* PR_NULLABLE textures) {
    SvgDecoder* decoder = allocZeroed<SvgDecoder>();
    decoder->destroyFn = destroyDecoder;
    decoder->decodeFn = decode;
    decoder->imageCodec = imageCodec;
    decoder->compressCodec = compressCodec;
    decoder->textures = textures;
    return decoder;
}
