// SVG images (html_svg_image.h)

#include "html/src/html_svg_image.h"

#include "pr/alloc.h"
#include "pr/atomic.h"
#include "pr/css_presentation.h"
#include "pr/dom_xml.h"
#include "pr/float.h"
#include "pr/image_texture_source.h"
#include "pr/layout_scene.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"
#include "pr/string_ops.h"
#include "pr/svg.h"
#include "pr/svg_render.h"
#include "pr/url_data.h"

#include "css/src/css_presentation.h"
#include "dom/src/dom_node.h"
#include "dom/src/dom_resource.h"
#include "html/src/html_image.h"
#include "html/src/html_page.h"
#include "html/src/html_window.h"
#include "svg/src/svg_image.h"
#include "svg/src/svg_render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A variant of an SVG image: a build and the painter that draws it. The build's environment, and what the build read,
// decide which environments the variant serves (svgImageServes). A `sized` painter builds the image again at each size
// that it is drawn at, and serves environments that name no size.
struct HtmlSvgImageVariant {
    const PrSvgImage* still;            // the variants hold a reference
    bool sized;
    PrLayoutPainter painter;            // the variants hold a reference
};

// The variants of an SVG image (DomImageVariants): its bytes and URL, and the variants built so far. A variant is built
// the first time that an environment asks for one that no existing variant serves. Environments that differ only in
// what the image did not read share a variant. The page that shows the image holds a reference, as each holder of the
// variants does. Variants are built only while that page lives.
struct HtmlSvgImageVariants {
    Atomic<uint32_t> references;
    PrHtmlPage* PR_NULLABLE page;
    PrRawString bytes;
    PrRawString url;
    bool rendering;
    RawArray<HtmlSvgImageVariant> variants;
};

// A build of an SVG image laid out at `extent`.
struct HtmlSvgImageBuild {
    PrVec2 extent;
    const PrSvgImage* still;            // the build holds a reference
};

// An SVG image that reads the size it is laid out at (PrSvgEnvironmentFlag_Size), in one color scheme and embedding
// environment. It holds a build for each size it is drawn at, because its layout depends on that size. A size is built
// when layout places the image, or when a paint asks for a size that layout did not place, so the image draws at its
// size from its first frame. A build runs on the thread that lays out or paints, and uses the codec, font set and
// shaper of the page's host. The presentation uses those on one thread at a time: the layout thread while a job runs,
// and the caller's thread otherwise (prCssPresentationStart). Each build parses a document of its own. Each painter
// holds a reference.
struct HtmlSvgImageSized {
    Atomic<uint32_t> references;
    PrRawString bytes;
    PrRawString url;
    const PrImageCodecSet* PR_NULLABLE codec;
    PrTextureCache* PR_NULLABLE textures;
    PrFontSet* PR_NULLABLE fonts;
    PrTextShaper* PR_NULLABLE shaper;
    bool dark;
    CssMediaEnvironment media;          // the environment of the document that embeds the image

    // The builds, which the threads that place and draw the image share. The first is at the natural or the default
    // size.
    PrMutex mutex;
    RawArray<HtmlSvgImageBuild> builds;
};

// An animated SVG image: its page, and what its painter plays. The page that shows it holds a reference, as each
// painter does.
struct HtmlSvgImage {
    Atomic<uint32_t> references;
    PrHtmlPage* PR_NULLABLE page;       // the image's own page, or null once the page that shows it is destroyed
    const PrSvgImage* still;            // the document as it is, drawn until the first scene
    SvgImageFit fit;
    SvgImageTimes times;
    double origin;                      // the showing page's clock at the image's first update, or NaN before it
    bool finished;                      // the image's time reached the end of its media fragment

    // What the painter plays: the last scene, the effects that the composited animations give its layers at the image's
    // time, and a count of the changes to either (PrLayoutPainter::revision). A painter on a layout worker reads them
    // while the next update writes them.
    PrMutex mutex;
    const PrLayoutScene* PR_NULLABLE scene;
    RawArray<PrLayoutSceneEffect> effects;
    uint64_t revision;
};

namespace {

// What a still image is drawn with, from the page that shows it: the codec that decodes its raster images, the cache
// that realizes them, the fonts and the shaper that set its text, the color scheme that its document prefers, the
// environment of the document that embeds it, and the size that it is laid out at, or zero for its natural size.
// Without fonts or a shaper, only SVG fonts draw.
struct StillServices {
    const PrImageCodecSet* PR_NULLABLE codec;
    PrTextureCache* PR_NULLABLE textures;
    PrFontSet* PR_NULLABLE fonts;
    PrTextShaper* PR_NULLABLE shaper;
    bool dark;
    const CssMediaEnvironment* PR_NULLABLE media;
    PrVec2 viewport;
};

// Returns the services of the page that shows a still image.
StillServices servicesOf(const PrHtmlPage* page, bool dark, const CssMediaEnvironment* PR_NULLABLE media,
        PrVec2 viewport) {
    return StillServices{
        .codec = page->host.images,
        .textures = page->window->textures,
        .fonts = page->host.font != nullptr ? prTextFontSet(page->host.font) : nullptr,
        .shaper = page->host.shaper,
        .dark = dark,
        .media = media,
        .viewport = viewport,
    };
}

const PrSvgImage* PR_NULLABLE stillOf(PrStringView bytes, PrStringView fragment, const StillServices& services);

// Loads an image that an SVG image names (PrSvgLoadImageFn), with the services that `context` points to. An SVG image
// is processed in secure static mode (svg-integration/#secure-static-mode), which loads no external resource, so only a
// data: URL, which holds its own bytes, loads.
bool loadSecureImage(void* PR_NULLABLE context, PrDomDocument* from, PrStringView url, PrSvgLoadedImage* out) {
    (void)from;
    const StillServices& services = *static_cast<const StillServices*>(context);
    const PrImageCodecSet* codec = services.codec;
    PrRawString body{};
    PrRawString type{};
    if (prUrlProcessData(url, &body, &type) == PrStatus_Ok) {
        PrStringView bytes = prRawStringView(&body);
        if (htmlImageIsSvg(bytes)) {
            out->svg = stillOf(bytes, domUrlFragment(url), services);
        } else if (codec != nullptr) {
            prImageTextureSourceOpenBytes(prStringBytes(bytes), codec, nullptr, PrImageTextureColor_Srgb, &out->raster);
        }
    }
    prRawStringReset(&body);
    prRawStringReset(&type);
    return out->raster != nullptr || out->svg != nullptr;
}

// Returns a document of the SVG image `bytes` that no browsing context shows, or null if the bytes do not parse as one.
// The caller holds the reference.
PrDomDocument* PR_NULLABLE parseImage(PrStringView bytes) {
    PrByteView view{reinterpret_cast<const uint8_t*>(bytes.data), bytes.size};
    PrDomDocument* document = prDomParseXmlBytes(view, "image/svg+xml", nullptr);
    prCssEnable(document);
    prSvgEnable(document);
    return document;
}

// Returns the outermost svg element of the document, or null if its root is not one.
PrSvgElement* PR_NULLABLE rootOf(PrDomDocument* document) {
    PrDomElement* root = prDomDocumentElement(document);
    return root != nullptr ? prSvgElementQuery(prDomElementAsEventTarget(root)) : nullptr;
}

// Returns the image of the outermost svg element `svg` as it is, in secure static mode, named by a URL whose fragment
// is `fragment`, with a reference for the caller. It loads only the images of data: URLs, and sets its text, with
// `services`. The image is built during the call, so the services need outlive only the call.
const PrSvgImage* stillOfRoot(PrSvgElement* svg, PrStringView fragment, const StillServices& services) {
    PrSvgDrawOptions options = prSvgDrawOptionsDefault();
    options.textures = services.textures;
    options.fonts = services.fonts;
    options.shaper = services.shaper;
    options.loadImage = loadSecureImage;
    options.loadContext = const_cast<StillServices*>(&services);
    options.prefersDark = services.dark;
    options.viewport = services.viewport;
    return svgImageCreate(svg, fragment, &options, services.media);
}

// Returns the SVG image `bytes` as it is (stillOfRoot), or null if the bytes do not parse as an SVG document.
const PrSvgImage* PR_NULLABLE stillOf(PrStringView bytes, PrStringView fragment, const StillServices& services) {
    PrDomDocument* document = parseImage(bytes);
    PrSvgElement* svg = rootOf(document);
    const PrSvgImage* image = svg != nullptr ? stillOfRoot(svg, fragment, services) : nullptr;
    prDomDocumentRelease(document);
    return image;
}

// Returns true if `text` names an animation or a transition, as a style sheet or a style attribute that animates does.
bool namesAnimation(PrStringView text) {
    return prStringContains(text, "animation") || prStringContains(text, "transition");
}

// Returns true if a page of its own shows the image. That is so if the document may change over time without a script,
// because it has an animation element (svg-animations/#AnimationElements), or a style sheet or a style attribute that
// names an animation or a transition. It is also so if the document has a foreignObject, whose content only a
// presentation lays out.
bool needsPage(const PrDomDocument* document) {
    const PrDomNode* root = document;
    for (const PrDomNode* node = root; node != nullptr; node = domNodeNext(root, node)) {
        if (node->type == PrDomNodeType_Text) {
            const PrDomElement* parent = domElementFromNode(node->parent);
            if (parent != nullptr && parent->tag == PrDomTag_Style &&
                    namesAnimation(prRawStringView(&static_cast<const PrDomCharacterData*>(node)->data)))
                return true;
            continue;
        }
        if (node->type != PrDomNodeType_Element)
            continue;
        const PrDomElement* element = static_cast<const PrDomElement*>(node);
        if (element->space == PrDomNamespace_Svg && (element->tag == PrDomTag_Animate || element->tag == PrDomTag_Set ||
                element->tag == PrDomTag_AnimateMotion || element->tag == PrDomTag_AnimateTransform ||
                element->tag == PrDomTag_ForeignObject))
            return true;
        if (namesAnimation(domAttributeValue(element, "style")))
            return true;
    }
    return false;
}

void retain(HtmlSvgImage* image) {
    image->references.fetchAddRelaxed(1);
}

// Releases a reference. The last frees what the painter plays. The image's page is gone by then: the page that shows
// the image holds a reference until it destroys the image's page.
void release(HtmlSvgImage* PR_NULLABLE image) {
    if (image == nullptr || image->references.fetchSub(1) != 1)
        return;
    PR_ASSERT(image->page == nullptr);
    prLayoutSceneRelease(image->scene);
    image->effects.reset();
    prSvgImageRelease(image->still);
    prMutexReset(&image->mutex);
    prMemoryFree(image);
}

void painterDraw(void* PR_NULLABLE context, PrVec2 extent, PrDrawList* list) {
    HtmlSvgImage* image = static_cast<HtmlSvgImage*>(context);
    prMutexLock(&image->mutex);
    const PrLayoutScene* scene = image->scene;
    if (scene != nullptr)
        prLayoutSceneRetain(scene);
    RawArray<PrLayoutSceneEffect> effects;
    effects.append(image->effects.span());
    prMutexUnlock(&image->mutex);
    if (scene == nullptr) {
        prSvgImageDraw(image->still, extent, list);
        return;
    }
    // The image's viewport clips its content, whatever the root's overflow, as nothing outside the image draws.
    prDrawListPushClipRect(list, PrRect{0, 0, extent.x, extent.y}, PrDrawEdges_Antialiased);
    prDrawListPushTransformAffine2(list, svgImageFitTransform(image->fit, extent));
    PrLayoutScenePlayOptions options = prLayoutScenePlayOptionsDefault();
    options.effects = effects.data;
    options.effectCount = effects.count;
    prLayoutScenePlay(scene, list, &options);
    prDrawListPopTransform(list);
    prDrawListPopClip(list);
    prLayoutSceneRelease(scene);
    effects.reset();
}

void painterRetain(void* PR_NULLABLE context) {
    retain(static_cast<HtmlSvgImage*>(context));
}

void painterRelease(void* PR_NULLABLE context) {
    release(static_cast<HtmlSvgImage*>(context));
}

uint64_t painterRevision(void* PR_NULLABLE context) {
    HtmlSvgImage* image = static_cast<HtmlSvgImage*>(context);
    prMutexLock(&image->mutex);
    uint64_t revision = image->revision;
    prMutexUnlock(&image->mutex);
    return revision;
}

// Returns a page that shows the SVG image `bytes` at `url` in secure animated mode
// (svg-integration/#secure-animated-mode), for `owner` to show, with `owner`'s agent, fonts, codecs and display. Its
// documents are sandboxed of every capability (html/#sandboxing-flag-set), as an iframe's documents are by its sandbox
// attribute: they run no script, have an opaque origin and navigate nowhere. Its host has no fetcher, so nothing but a
// data: URL loads.
PrHtmlPage* imagePage(PrHtmlPage* owner, PrStringView bytes, PrStringView url, bool dark) {
    PrHtmlHost host = prHtmlHostDefault();
    host.context = owner->host.context;
    host.now = owner->host.now;
    host.wallNow = owner->host.wallNow;
    host.agent = owner->window->agent;
    host.font = owner->host.font;
    host.shaper = owner->host.shaper;
    host.images = owner->host.images;
    host.compress = owner->host.compress;
    host.renderer = owner->host.renderer;
    PrHtmlPage* page = prHtmlPageCreate(&host);
    page->window->sandbox = HtmlSandboxFlag_All;
    page->window->driven = true;
    prHtmlPageSetDisplay(page, &owner->display);
    // The image's document prefers the color scheme of the element that embeds it
    // (css-color-adjust-1/#page-color-scheme).
    PrAppUserPreferences preferences = owner->preferences;
    preferences.colorScheme = dark ? PrAppColorScheme_Dark : PrAppColorScheme_Light;
    prHtmlPageSetUserPreferences(page, &preferences);
    PrHtmlResponseHeaders headers{"image/svg+xml", {}, {}};
    prHtmlPageLoad(page, bytes, url, &headers);
    // An image's canvas stays transparent (css-color-adjust-1/#color-scheme-effect).
    if (page->presentation != nullptr)
        page->presentation->embedding = PrCssPresentationEmbedding_Frame;
    return page;
}

// Runs the image's rendering update at `now` on the showing page's clock, lays its document out and records its scene.
// Returns true if what the painter plays changed.
bool update(HtmlSvgImage* image, double now) {
    PrHtmlWindow* window = image->page->window;
    if (prF64IsNan(image->origin)) {
        image->origin = now;
        // The document's animations begin at its first update (svg-animations/#DocumentBegin), from which the media
        // fragment's start is reached.
        if (image->times.start > 0)
            htmlWindowRunFrame(window, window->timeOrigin);
    }
    double time = image->times.start + (now - image->origin) / 1000;
    image->finished = time >= image->times.end;
    if (image->finished)
        time = image->times.end;
    htmlWindowRunFrame(window, window->timeOrigin + time * 1000);
    PrCssPresentation* presentation = image->page->presentation;
    if (presentation == nullptr)
        return false;
    PrLayoutVec extent = prLayoutVecFromPx(image->fit.layout);
    htmlPresentationUpdate(presentation, extent, extent);
    const PrLayoutScene* scene = prLayoutTreeScene(prCssPresentationTree(presentation));
    RawArray<PrLayoutTrack> tracks;
    tracks.resize(prCssPresentationTracks(presentation, nullptr, 0));
    prCssPresentationTracks(presentation, tracks.data, tracks.count);
    double at = htmlWindowFrameTime(window);
    RawArray<PrLayoutSceneEffect> effects;
    effects.resize(prLayoutSceneSampleTracks(scene, tracks.data, tracks.count, at, nullptr, 0));
    prLayoutSceneSampleTracks(scene, tracks.data, tracks.count, at, effects.data, effects.count);
    tracks.reset();
    prMutexLock(&image->mutex);
    bool same = scene == image->scene && effects.count == image->effects.count;
    for (uint32_t i = 0; same && i < effects.count; ++i) {
        const PrLayoutSceneEffect& a = effects.data[i];
        const PrLayoutSceneEffect& b = image->effects.data[i];
        same = a.node == b.node && a.flags == b.flags && a.opacity == b.opacity &&
            prMemEquals(&a.transform, &b.transform, sizeof(PrMatrix4));
    }
    const PrLayoutScene* old = image->scene;
    image->scene = scene;
    image->effects.reset();
    image->effects = effects;
    if (!same)
        ++image->revision;
    prMutexUnlock(&image->mutex);
    prLayoutSceneRelease(old);
    return !same;
}

// The most builds that an image that reads its size keeps. A size past them draws the first build, scaled.
constexpr uint32_t kMaxSizedBuilds = 8;

void sizedRetain(void* PR_NULLABLE context) {
    static_cast<HtmlSvgImageSized*>(context)->references.fetchAddRelaxed(1);
}

void sizedRelease(void* PR_NULLABLE context) {
    HtmlSvgImageSized* image = static_cast<HtmlSvgImageSized*>(context);
    if (image->references.fetchSub(1) != 1)
        return;
    for (const HtmlSvgImageBuild& build : image->builds.span())
        prSvgImageRelease(build.still);
    image->builds.reset();
    prMutexReset(&image->mutex);
    prRawStringReset(&image->bytes);
    prRawStringReset(&image->url);
    prMemoryFree(image);
}

const PrSvgImage* PR_NULLABLE buildAt(PrStringView bytes, PrStringView url, const StillServices& services);

// Returns the image's build at `extent`, or null if it has none. The caller holds the image's mutex.
const PrSvgImage* PR_NULLABLE sizedFind(const HtmlSvgImageSized* image, PrVec2 extent) {
    for (const HtmlSvgImageBuild& build : image->builds.span()) {
        if (build.extent.x == extent.x && build.extent.y == extent.y)
            return build.still;
    }
    return nullptr;
}

// Returns the build of the image at `extent`, with a reference for the caller, building it now if the image has none
// and has room for it, and else the first build.
const PrSvgImage* sizedBuild(HtmlSvgImageSized* image, PrVec2 extent) {
    prMutexLock(&image->mutex);
    const PrSvgImage* PR_NULLABLE found = sizedFind(image, extent);
    bool room = image->builds.count < kMaxSizedBuilds;
    if (found != nullptr || !room) {
        found = found != nullptr ? found : image->builds.data[0].still;
        prSvgImageRetain(found);
        prMutexUnlock(&image->mutex);
        return found;
    }
    prMutexUnlock(&image->mutex);
    StillServices services{
        .codec = image->codec,
        .textures = image->textures,
        .fonts = image->fonts,
        .shaper = image->shaper,
        .dark = image->dark,
        .media = &image->media,
        .viewport = extent,
    };
    const PrSvgImage* built = buildAt(prRawStringView(&image->bytes), prRawStringView(&image->url), services);
    prMutexLock(&image->mutex);
    // Another thread may have built the size meanwhile, and the first build serves if this one failed.
    found = sizedFind(image, extent);
    if (found == nullptr && built != nullptr && image->builds.count < kMaxSizedBuilds) {
        image->builds.push(HtmlSvgImageBuild{extent, built});
        found = built;
        built = nullptr;
    }
    if (found == nullptr)
        found = image->builds.data[0].still;
    prSvgImageRetain(found);
    prMutexUnlock(&image->mutex);
    prSvgImageRelease(built);
    return found;
}

// Builds the image at the extent that the tree places its content at (PrLayoutPainter::place), before it is drawn.
void sizedPlace(void* PR_NULLABLE context, PrVec2 extent) {
    prSvgImageRelease(sizedBuild(static_cast<HtmlSvgImageSized*>(context), extent));
}

// Draws the build at `extent`, building it first if no layout placed the image at that extent.
void sizedDraw(void* PR_NULLABLE context, PrVec2 extent, PrDrawList* list) {
    const PrSvgImage* still = sizedBuild(static_cast<HtmlSvgImageSized*>(context), extent);
    prSvgImageDraw(still, extent, list);
    prSvgImageRelease(still);
}

// Destroys the image's page and gives up the showing page's reference.
void forget(HtmlSvgImage* image) {
    prHtmlPageDestroy(image->page);
    image->page = nullptr;
    release(image);
}

// Builds the SVG image `bytes`, named by `url`, with `services`, with a reference for the caller, or returns null if
// the bytes do not parse as an SVG document. The build parses a document of its own.
const PrSvgImage* PR_NULLABLE buildAt(PrStringView bytes, PrStringView url, const StillServices& services) {
    PrDomDocument* document = parseImage(bytes);
    PrSvgElement* svg = rootOf(document);
    const PrSvgImage* still = nullptr;
    if (svg != nullptr)
        still = stillOfRoot(svg, domUrlFragment(url), services);
    prDomDocumentRelease(document);
    return still;
}

// Returns a painter of the SVG image `bytes`, named by `url`, that `page` shows in `environment`, with a reference for
// the caller, and writes its natural dimensions, its build with a reference for the caller, and whether the painter is
// sized (HtmlSvgImageVariant::sized). Returns a painter with no paint function if the bytes do not parse as an SVG
// document.
PrLayoutPainter painterOf(PrHtmlPage* page, PrStringView bytes, PrStringView url, bool rendering,
        DomImageEnvironment environment, PrLayoutNaturalSize* natural, const PrSvgImage* PR_NULLABLE* PR_NONNULL built,
        bool* sized) {
    bool dark = environment.dark;
    *built = nullptr;
    *sized = false;
    PrStringView fragment = domUrlFragment(url);
    PrDomDocument* document = parseImage(bytes);
    PrSvgElement* svg = rootOf(document);
    if (svg == nullptr) {
        prDomDocumentRelease(document);
        return PrLayoutPainter{};
    }
    StillServices services = servicesOf(page, dark, environment.media, environment.size);
    const PrSvgImage* still = stillOfRoot(svg, fragment, services);
    prSvgImageRetain(still);
    *built = still;
    // An image element of an SVG document draws its image as it is (svgImageOfPainter). A page that renders nothing
    // shows no image of a page of its own.
    bool paged = !rendering && page->host.font != nullptr && page->host.shaper != nullptr && needsPage(document);
    prDomDocumentRelease(document);
    *natural = svgLayoutNaturalSize(prSvgImageNaturalSize(still));
    // An image that reads its size, in an environment that names none, is built again at each size that it is drawn at.
    // An image element of an SVG document names the size, and draws the build as it is (svgImageOfPainter).
    bool reads = (prSvgImageEnvironmentRead(still) & PrSvgEnvironmentFlag_Size) != 0;
    bool unsized = !(environment.size.x > 0) || !(environment.size.y > 0);
    if (!paged && !rendering && reads && unsized) {
        HtmlSvgImageSized* image = allocZeroed<HtmlSvgImageSized>();
        image->references.storeRelaxed(1);      // the painter holds the first reference
        image->bytes = prRawStringCopy(bytes);
        image->codec = services.codec;
        image->textures = services.textures;
        image->fonts = services.fonts;
        image->shaper = services.shaper;
        image->url = prRawStringCopy(url);
        image->dark = dark;
        if (environment.media != nullptr)
            image->media = *environment.media;
        else
            cssMediaEnvironmentInit(&image->media, false);
        image->media.reads = nullptr;
        *sized = true;
        prMutexInit(&image->mutex);
        image->builds.push(HtmlSvgImageBuild{PrVec2{}, still});
        return PrLayoutPainter{
            .context = image,
            .paint = sizedDraw,
            .retain = sizedRetain,
            .release = sizedRelease,
            .place = sizedPlace,
        };
    }
    if (!paged)
        return prSvgImagePainter(still);
    HtmlSvgImage* image = allocZeroed<HtmlSvgImage>();
    image->references.storeRelaxed(2);      // the painter's and the showing page's
    image->still = still;
    image->origin = kPrF64Nan;
    image->times = svgImageTimes(fragment);
    prMutexInit(&image->mutex);
    image->page = imagePage(page, bytes, url, dark);
    PrDomElement* root = prDomDocumentElement(image->page->window->document);
    if (root != nullptr && prSvgElementQuery(prDomElementAsEventTarget(root)) != nullptr)
        image->fit = svgImageApplyView(root, fragment, false);
    page->svgImages.push(image);
    return PrLayoutPainter{
        .context = image,
        .paint = painterDraw,
        .retain = painterRetain,
        .release = painterRelease,
        .revision = painterRevision,
    };
}

void variantsRetain(void* PR_NULLABLE context) {
    static_cast<HtmlSvgImageVariants*>(context)->references.fetchAddRelaxed(1);
}

void variantsRelease(void* PR_NULLABLE context) {
    HtmlSvgImageVariants* variants = static_cast<HtmlSvgImageVariants*>(context);
    if (variants->references.fetchSub(1) != 1)
        return;
    for (const HtmlSvgImageVariant& variant : variants->variants.span()) {
        prLayoutPainterRelease(variant.painter);
        prSvgImageRelease(variant.still);
    }
    variants->variants.reset();
    prRawStringReset(&variants->bytes);
    prRawStringReset(&variants->url);
    prMemoryFree(variants);
}

// Returns true if `variant` serves `environment`. A sized variant serves an environment that names no size. Any other
// variant serves an environment in which a build would draw the same as the variant's own build.
bool serves(const HtmlSvgImageVariant& variant, DomImageEnvironment environment) {
    bool unsized = !(environment.size.x > 0) || !(environment.size.y > 0);
    if (variant.sized && !unsized)
        return false;
    PrVec2 size = unsized ? PrVec2{} : environment.size;
    return svgImageServes(variant.still, environment.media, environment.dark, size);
}

PrLayoutPainter variantsPainter(void* PR_NULLABLE context, DomImageEnvironment environment) {
    HtmlSvgImageVariants* variants = static_cast<HtmlSvgImageVariants*>(context);
    for (const HtmlSvgImageVariant& variant : variants->variants.span()) {
        if (serves(variant, environment))
            return variant.painter;
    }
    // Variants that outlive their page build no more, and the first serves for any other environment.
    if (variants->page == nullptr)
        return variants->variants.data[0].painter;
    PrLayoutNaturalSize natural{};
    const PrSvgImage* still = nullptr;
    bool sized = false;
    PrLayoutPainter painter = painterOf(variants->page, prRawStringView(&variants->bytes),
        prRawStringView(&variants->url), variants->rendering, environment, &natural, &still, &sized);
    if (painter.paint == nullptr) {
        prSvgImageRelease(still);
        return variants->variants.data[0].painter;
    }
    variants->variants.push(HtmlSvgImageVariant{still, sized, painter});
    return painter;
}

} // namespace

PrLayoutNaturalSize htmlSvgNaturalSize(const PrSvgElement* svg) {
    return svgLayoutNaturalSize(prSvgElementNaturalSize(svg));
}

DomImageVariants htmlSvgImageVariants(PrHtmlPage* page, PrStringView bytes, PrStringView url, bool rendering,
        PrLayoutNaturalSize* natural) {
    // The first variant is built in the page's own environment, with a light scheme, at the image's natural size, which
    // this call writes.
    DomImageEnvironment first{.dark = false, .media = &page->window->media};
    const PrSvgImage* still = nullptr;
    bool sized = false;
    PrLayoutPainter painter = painterOf(page, bytes, url, rendering, first, natural, &still, &sized);
    if (painter.paint == nullptr) {
        prSvgImageRelease(still);
        return DomImageVariants{};
    }
    HtmlSvgImageVariants* variants = allocZeroed<HtmlSvgImageVariants>();
    variants->references.storeRelaxed(2);      // the caller's and the showing page's
    variants->page = page;
    variants->bytes = prRawStringCopy(bytes);
    variants->url = prRawStringCopy(url);
    variants->rendering = rendering;
    variants->variants.push(HtmlSvgImageVariant{still, sized, painter});
    page->svgImageVariants.push(variants);
    return DomImageVariants{
        .context = variants,
        .painter = variantsPainter,
        .retain = variantsRetain,
        .release = variantsRelease,
    };
}

bool htmlSvgImagesWantFrame(const PrHtmlPage* page) {
    for (const HtmlSvgImage* image : page->svgImages.span()) {
        if (!image->finished && (prF64IsNan(image->origin) || htmlWindowWantsFrame(image->page->window)))
            return true;
    }
    return false;
}

bool htmlSvgImagesFrame(PrHtmlPage* page, double now) {
    // Variants that nothing shows any more are forgotten, as the page's reference is their last.
    for (uint32_t i = page->svgImageVariants.count; i-- > 0;) {
        HtmlSvgImageVariants* variants = page->svgImageVariants.data[i];
        if (variants->references.loadRelaxed() == 1) {
            page->svgImageVariants.erase(i);
            variants->page = nullptr;
            variantsRelease(variants);
        }
    }
    bool changed = false;
    for (uint32_t i = page->svgImages.count; i-- > 0;) {
        HtmlSvgImage* image = page->svgImages.data[i];
        // An image that nothing shows any more is forgotten, as the page's reference is its last.
        if (image->references.loadRelaxed() == 1) {
            page->svgImages.erase(i);
            forget(image);
            continue;
        }
        if (!image->finished && (prF64IsNan(image->origin) || htmlWindowWantsFrame(image->page->window)))
            changed = update(image, now) || changed;
    }
    return changed;
}

void htmlSvgImagesReset(PrHtmlPage* page) {
    for (HtmlSvgImage* image : page->svgImages.span())
        forget(image);
    page->svgImages.reset();
    // Variants that outlive the page build no more, and keep the painters that they have.
    for (HtmlSvgImageVariants* variants : page->svgImageVariants.span()) {
        variants->page = nullptr;
        variantsRelease(variants);
    }
    page->svgImageVariants.reset();
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
