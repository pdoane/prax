// The drawings' shared toolkit: the images, fonts and text that they draw with, and their captions.

#include "examples/draw_features/drawing.h"

#include "pr/blob_ref.h"
#include "pr/compress_brotli.h"
#include "pr/compress_deflate.h"
#include "pr/compress_select.h"
#include "pr/image.h"
#include "pr/image_select.h"
#include "pr/print.h"
#include "pr/resource.h"
#include "pr/string.h"
#include "pr/vec.h"

namespace drawings {

using namespace pr;

namespace {

// The codecs of WOFF and WOFF2 fonts, zlib and Brotli, created once for the program.
const PrCompressCodecSet* woffCodecs() {
    static PrCompressCodecSet* set = [] {
        PrCompressCodec* codecs[] = {prCompressDeflateCreate(), prCompressBrotliCreate()};
        return prCompressCodecSetCreate(codecs, 2);
    }();
    return set;
}

struct Caption {
    PrRect band;
    String text;
    bool centered;              // true to center in the band, false to start from its left
    float size;
    PrColor tint;
};

Array<Caption>& pendingCaptions() {
    static Array<Caption> captions;
    return captions;
}

const PrTextureSource* cachedCard = nullptr;
const PrTextureSource* cachedFrame = nullptr;
const PrTextureSource* cachedTile = nullptr;
const PrTextureSource* cachedDecoded = nullptr;
PrDrawList* cachedMotif = nullptr;

// Returns an image that holds a copy of the pixels. The library makes the image once and keeps it for every drawing
// that shows the pixels.
const PrTextureSource* imageOf(const Array<uint8_t>& rgba, uint32_t width, uint32_t height,
        PrTextureAlpha alpha = PrTextureAlpha_Straight) {
    return prTextureSourceCreateCopy(PrPixelView{rgba.data(), size_t(width) * 4, {width, height}}, PrPixelFormat_Rgba8, alpha);
}

void put(Array<uint8_t>& rgba, uint32_t width, uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    uint8_t* p = rgba.data() + (size_t(y) * width + x) * 4;
    p[0] = r;
    p[1] = g;
    p[2] = b;
    p[3] = a;
}

// Returns the test card's pixels: a checker under a color wheel, with fine lines that alias when sampled carelessly.
Array<uint8_t> testCardPixels() {
    const uint32_t size = kTestCardSize;
    Array<uint8_t> rgba(size * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            bool check = ((x / 8) + (y / 8)) % 2 == 0;
            uint8_t base = check ? 230 : 190;
            float dx = float(x) - float(size - 1) / 2;
            float dy = float(y) - float(size - 1) / 2;
            float d = prF32Sqrt(dx * dx + dy * dy);
            if (d < 22) {
                float hue = prF32Atan2(dy, dx) / (2 * kPi) + 0.5f;
                float r = prF32Abs(hue * 6 - 3) - 1;
                float g = 2 - prF32Abs(hue * 6 - 2);
                float b = 2 - prF32Abs(hue * 6 - 4);
                auto channel = [](float v) {
                    return uint8_t(255 * (v < 0 ? 0 : v > 1 ? 1 : v));
                };
                put(rgba, size, x, y, channel(r), channel(g), channel(b), 255);
            } else if (x % 4 == 0 || y == 2 || y == 61) {
                put(rgba, size, x, y, 20, 20, 30, 255);
            } else {
                put(rgba, size, x, y, base, base, uint8_t(base - 20), 255);
            }
        }
    }
    return rgba;
}

struct LoadedFont {
    String key;                 // the file, and the derivation applied to it
    const PrFont* font;
};

Array<LoadedFont>& loadedFonts() {
    static Array<LoadedFont> fonts;
    return fonts;
}

PrTextGlyphBuffer* glyphBuffer;

// The shaper and the hinter of the drawing that is being drawn, and the display that it is drawn for (drawDrawing).
PrTextShaper* currentShaper = nullptr;
PrFontHinter* currentHinter = nullptr;
PrAppDisplay currentDisplay = prAppDisplayDefault();
PrTextureCache* currentTextures = nullptr;

// Returns the key that names a derivation of a file among the loaded fonts.
String keyOf(const char* file, const PrFontDerivation& d) {
    String key = file;
    for (uint32_t i = 0; i < d.variationCount; ++i)
        formatTo(key, " {:08x}={}", d.variations[i].tag, d.variations[i].value);
    for (uint32_t i = 0; i < d.featureCount; ++i)
        formatTo(key, " {:08x}:{}", d.features[i].tag, d.features[i].value);
    formatTo(key, " {} {} {} {} {} {} {} {} {}", d.sizeAdjust, d.ascent, d.descent, d.lineGap, uint32_t(d.hinting),
        uint32_t(d.overrides), d.embolden, d.oblique, static_cast<const void*>(d.hinter));
    return key;
}

// Records the captions that the drawing made, then forgets them.
void drawCaptions(PrDrawList* list) {
    TextStyle style;
    style.font = fontNamed("NotoSans.ttf");
    style.paint.color = kGray;
    for (const Caption& c : pendingCaptions()) {
        float room = prRectWidth(c.band);
        style.size = c.size;
        style.paint.color = c.tint;
        float width = textAdvance(style, c.text);
        if (c.centered && width > room && width > 0) {
            style.size = max(c.size * room / width, 7.0f);
            width = width * style.size / c.size;
        }
        float x = c.centered ? (c.band.left + c.band.right - width) / 2 : c.band.left;
        // A chip of paper behind the caption keeps it legible over what the drawing drew there.
        float y = c.band.bottom - 4;
        PrRect chip{x - 4, y - style.size, x + width + 4, y + style.size * 0.35f};
        prDrawListCornerRect(list, prCornerRectRound(chip, 3), {.color = withAlpha(kPaper, 0.85f)});
        drawText(list, style, c.text, PrVec2{x, y});
    }
    pendingCaptions().clear();
}

} // namespace

void drawDrawing(const Drawing& drawing, PrDrawList* list, PrTextShaper* shaper, PrFontHinter* PR_NULLABLE hinter,
        const PrAppDisplay& display, PrTextureCache* PR_NULLABLE textures, float width, float height, double time) {
    currentShaper = shaper;
    currentHinter = hinter;
    currentDisplay = display;
    currentTextures = textures;
    drawing.draw(list, width, height, time);
    drawCaptions(list);
    currentShaper = nullptr;
    currentHinter = nullptr;
    currentDisplay = prAppDisplayDefault();
    currentTextures = nullptr;
}

PrTextShaper* drawingShaper() {
    return currentShaper;
}

PrFontHinter* PR_NULLABLE drawingHinter() {
    return currentHinter;
}

const PrAppDisplay& drawingDisplay() {
    return currentDisplay;
}

PrTextureCache* PR_NULLABLE drawingTextures() {
    return currentTextures;
}

PrDrawSource imageSource(const PrTextureSource* image, PrRect rect, PrDrawSampling sampling) {
    PrTextureCacheFlags flags = PrTextureCacheFlags(
        (sampling & PrDrawSamplingFlag_Kind) == PrDrawSampling_Mipmap ? PrTextureCacheFlag_Levels : 0);
    PrTextureRegion realized{};
    if (currentTextures == nullptr ||
            prTextureCacheWait(currentTextures, image, prTextureCacheAskEncoded(flags), &realized) !=
            PrTextureCacheState_Ready)
        return PrDrawSource{};
    return prDrawSourceRegion(&realized, rect, 0);
}

void caption(PrRect band, PrStringView text) {
    pendingCaptions().push(Caption{band, text, true, 11, kGray});
}

const PrTextureSource* testCard() {
    if (cachedCard == nullptr)
        cachedCard = imageOf(testCardPixels(), kTestCardSize, kTestCardSize);
    return cachedCard;
}

const PrTextureSource* frameImage() {
    if (cachedFrame != nullptr)
        return cachedFrame;
    // A frame with 12-pixel corners marked with dots, striped edges and a soft center.
    const uint32_t size = kFrameImageSize;
    Array<uint8_t> rgba(size * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            bool edgeX = x < 12 || x >= 36;
            bool edgeY = y < 12 || y >= 36;
            if (edgeX && edgeY) {
                uint32_t cx = x < 12 ? 6 : 42;
                uint32_t cy = y < 12 ? 6 : 42;
                bool dot = (x - cx) * (x - cx) + (y - cy) * (y - cy) < 16;
                put(rgba, size, x, y, dot ? 250 : 40, dot ? 200 : 60, dot ? 40 : 120, 255);
            } else if (edgeX || edgeY) {
                uint32_t along = edgeY ? x : y;
                bool stripe = (along / 3) % 2 == 0;
                put(rgba, size, x, y, stripe ? 200 : 60, stripe ? 60 : 90, stripe ? 60 : 180, 255);
            } else {
                put(rgba, size, x, y, 240, 235, 210, 200);
            }
        }
    }
    cachedFrame = imageOf(rgba, size, size);
    return cachedFrame;
}

const PrTextureSource* smallTile() {
    if (cachedTile != nullptr)
        return cachedTile;
    const uint32_t size = kSmallTileSize;
    Array<uint8_t> rgba(size * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            float dx = float(x) - 11.5f;
            float dy = float(y) - 4.5f;
            bool dot = dx * dx + dy * dy < 9;
            bool corner = x < 6 && y > 9;
            put(rgba, size, x, y, dot ? 220 : corner ? 30 : 90, dot ? 50 : corner ? 140 : 160, dot ? 40 : corner ? 60 : 210, 255);
        }
    }
    cachedTile = imageOf(rgba, size, size);
    return cachedTile;
}

const PrTextureSource* PR_NULLABLE decodedCard() {
    if (cachedDecoded != nullptr)
        return cachedDecoded;
    Array<uint8_t> rgba = testCardPixels();
    PrCompressCodecSet* compressCodec = prCompressCodecSetCreateDefault();
    PrImageCodecSet* imageCodec = prImageCodecSetCreateDefault(compressCodec);
    const PrPixelView card{rgba.data(), kTestCardSize * 4, {kTestCardSize, kTestCardSize}};
    size_t size = 0;
    prImageCodecSetEncode(imageCodec, "image/png", card, nullptr, 0, &size);
    Array<uint8_t> png(size);
    prImageCodecSetEncode(imageCodec, "image/png", card, png.data(), png.count(), &size);
    PrImageSource source{};
    source.memory = png;
    PrImageDecoder* decoder = prImageDecoderCreate(imageCodec);
    Array<uint8_t> decoded(kTestCardSize * kTestCardSize * 4);
    if (prImageDecoderOpen(decoder, source, nullptr) == PrStatus_Ok &&
            prImageDecoderReadRgba8(decoder, 0, PrImageAlpha_Premultiplied, PrImageOrientation_Identity,
                PrPixels{decoded.data(), kTestCardSize * 4, {kTestCardSize, kTestCardSize}}) == PrStatus_Ok)
        cachedDecoded = imageOf(decoded, kTestCardSize, kTestCardSize, PrTextureAlpha_Premultiplied);
    prImageDecoderDestroy(decoder);
    prImageCodecSetDestroy(imageCodec);
    prCompressCodecSetDestroy(compressCodec);
    return cachedDecoded;
}

PrDrawList* motif() {
    if (cachedMotif != nullptr)
        return cachedMotif;
    PrDrawList* list = prDrawListCreate();
    for (uint32_t i = 0; i < 6; ++i) {
        float angle = 2 * kPi * float(i) / 6;
        PrVec2 c{20 + 10 * prF32Cos(angle), 20 + 10 * prF32Sin(angle)};
        prDrawListEllipse(list, c, PrVec2{8, 8}, 0, {.color = withAlpha(i % 2 == 0 ? kRed : kGold, 0.85f)});
    }
    prDrawListEllipse(list, PrVec2{20, 20}, PrVec2{6, 6}, 0, {.color = kPurple});
    PrDrawStroke ring = strokeOf(1.5f);
    prDrawListEllipse(list, PrVec2{20, 20}, PrVec2{19, 19}, 0, {.color = kInk, .stroke = &ring});
    cachedMotif = list;
    return cachedMotif;
}

void releaseDrawingImages() {
    prTextureSourceRelease(cachedCard);
    prTextureSourceRelease(cachedFrame);
    prTextureSourceRelease(cachedTile);
    prTextureSourceRelease(cachedDecoded);
    prDrawListRelease(cachedMotif);
    cachedCard = cachedFrame = cachedTile = cachedDecoded = nullptr;
    cachedMotif = nullptr;
}

const PrFont* fontNamed(const char* file) {
    return fontVaried(file, nullptr, 0);
}

const PrFont* fontVaried(const char* file, const PrFontVariation* PR_NULLABLE values, uint32_t count) {
    PrFontDerivation derivation = prFontDerivationDefault();
    derivation.variations = values;
    derivation.variationCount = count;
    return fontDerived(file, derivation);
}

const PrFont* fontDerived(const char* file, const PrFontDerivation& derivation) {
    String key = keyOf(file, derivation);
    for (const LoadedFont& loaded : loadedFonts()) {
        if (loaded.key == key)
            return loaded.font;
    }
    const PrFont* font = nullptr;
    if (key == keyOf(file, prFontDerivationDefault())) {
        PrStatus status = PrStatus_NotFound;
        if (BlobRef resource = BlobRef::adopt(prResourceOpen(format("fonts/{}", file)))) {
            // A WOFF or WOFF2 file decodes through the built-in codec.
            const PrByteView view = resource.bytes();
            const bool woff = view.size >= 4 && view.data[0] == 'w' && view.data[1] == 'O' && view.data[2] == 'F';
            if (woff)
                status = prFontCreateWoff(view, 0, nullptr, woffCodecs(), &font);
            else
                status = prFontCreate(resource, 0, nullptr, &font);
        }
        if (status != PrStatus_Ok) {
            printError("draw_features: no font {} among its resources; ./dev.sh fetch fonts\n", file);
            PR_FATAL("a drawing's font is missing");
        }
    } else {
        font = prFontCreateDerived(fontNamed(file), &derivation);
    }
    loadedFonts().push(LoadedFont{key, font});
    return font;
}

float shapeText(const TextStyle& style, PrStringView text, PrVec2 origin, ShapedText& out) {
    out.glyphs.clear();
    out.transforms.clear();
    if (glyphBuffer == nullptr)
        glyphBuffer = prTextGlyphBufferCreate();
    PrTextRun run{};
    run.font = style.font;
    run.text = text;
    run.direction = style.direction;
    run.axis = style.axis;
    run.script = style.script;
    if (style.language != nullptr)
        run.locale = prLocaleFromTag(style.language);
    run.features = style.features;
    run.featureCount = style.featureCount;
    run.kerning = style.kerning;
    run.caps = style.caps;
    run.position = style.position;
    run.synthesis = style.synthesis;
    run.pixelsPerEm = style.size;
    if (prTextShaperShape(currentShaper, &run, glyphBuffer) != PrStatus_Ok)
        return 0;
    uint32_t count = prTextGlyphBufferGlyphCount(glyphBuffer);
    out.glyphs.resize(count);
    out.transforms.resize(count);
    PrVec2 pen = prTextGlyphBufferPlace(glyphBuffer, style.size, origin, out.glyphs.data(), out.transforms.data());
    bool scaled = false;
    for (const PrAffine2& transform : out.transforms)
        scaled = scaled || transform.a != 1;
    if (!scaled)
        out.transforms.clear();
    return style.axis == PrTextAxis_Vertical ? pen.y - origin.y : pen.x - origin.x;
}

float textAdvance(const TextStyle& style, PrStringView text) {
    ShapedText shaped;
    return shapeText(style, text, PrVec2{0, 0}, shaped);
}

void drawGlyphs(PrDrawList* list, const TextStyle& style, const ShapedText& shaped) {
    PrFontGlyphRun run{};
    run.font = style.font;
    run.size = style.size;
    run.glyphs = shaped.glyphs.data();
    run.glyphCount = shaped.glyphs.count();
    run.transforms = shaped.transforms.isEmpty() ? nullptr : shaped.transforms.data();
    run.palette.index = style.palette;
    prDrawListGlyphs(list, &run, PrVec2{}, style.paint);
}

float drawText(PrDrawList* list, const TextStyle& style, PrStringView text, PrVec2 origin) {
    ShapedText shaped;
    float advance = shapeText(style, text, origin, shaped);
    drawGlyphs(list, style, shaped);
    return advance;
}

void label(PrStringView text, PrVec2 origin, float size, PrColor tint) {
    pendingCaptions().push(Caption{PrRect{origin.x, origin.y - size, origin.x, origin.y + 4}, text, false, size, tint});
}

void releaseDrawingText() {
    for (LoadedFont& loaded : loadedFonts())
        prFontRelease(loaded.font);
    loadedFonts().clear();
    prTextGlyphBufferDestroy(glyphBuffer);
    glyphBuffer = nullptr;
}

} // namespace drawings
