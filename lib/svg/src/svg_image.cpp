// SVG images: SVG documents shown as images, drawn at whatever size they are shown.

#include "svg/src/svg_image.h"

#include "pr/alloc.h"
#include "pr/float.h"
#include "pr/string_ops.h"
#include "pr/text_buffer.h"
#include "pr/url_percent.h"

#include "svg/src/svg_parse.h"
#include "svg/src/svg_render.h"

namespace pr_impl {

namespace {

// Returns the arguments of `name(...)` if `text` is that call, or the null view.
PrStringView argumentsOf(PrStringView text, PrStringView name) {
    if (text.size < name.size + 2 || stringSlice(text, 0, name.size) != name || text.data[name.size] != '('
        || text.data[text.size - 1] != ')')
        return PrStringView{};
    return stringSlice(text, name.size + 1, text.size - name.size - 2);
}

// Applies an SVG view specification, the arguments of svgView() (SVG2/linking.html#SVGFragmentIdentifiers): its
// viewBox(), preserveAspectRatio() and transform(), separated by semicolons. Its zoomAndPan() changes nothing that an
// image draws.
void applySvgView(PrStringView specification, SvgRootView* view) {
    for (size_t start = 0; start <= specification.size;) {
        size_t end = start + stringFind(stringSlice(specification, start, specification.size - start), ';');
        PrStringView part = stringTrim(stringSlice(specification, start, end - start));
        PrStringView viewBox = argumentsOf(part, "viewBox");
        PrStringView aspect = argumentsOf(part, "preserveAspectRatio");
        PrStringView transform = argumentsOf(part, "transform");
        PrRect box{};
        SvgAspect parsed;
        PrAffine2 matrix{};
        if (viewBox.data != nullptr && svgParseViewBox(viewBox, &box)) {
            view->viewBox = box;
            view->hasViewBox = true;
        } else if (aspect.data != nullptr && svgParseAspect(aspect, &parsed)) {
            view->aspect = parsed;
        } else if (transform.data != nullptr && svgParseTransform(transform, &matrix)) {
            view->transform = matrix;
            view->hasTransform = true;
        }
        start = end + 1;
    }
}

// Parses the four coordinates of a media fragment's spatial dimension, separated by commas: integers in pixels, or
// numbers with an optional fraction in percentages (Media Fragments URI 1.0, 4.2.2). Returns false if `text` is not
// four of them.
bool parseCoordinates(PrStringView text, bool percent, float* out) {
    uint32_t count = 0;
    for (size_t start = 0; start <= text.size && count < 4;) {
        size_t end = start + stringFind(stringSlice(text, start, text.size - start), ',');
        PrStringView number = stringSlice(text, start, end - start);
        start = end + 1;
        double value = 0;
        double scale = 0;
        bool digits = false;
        for (size_t i = 0; i < number.size; ++i) {
            char c = number.data[i];
            if (c >= '0' && c <= '9') {
                digits = true;
                if (scale == 0) {
                    value = value * 10 + (c - '0');
                } else {
                    value += (c - '0') * scale;
                    scale /= 10;
                }
            } else if (c == '.' && percent && scale == 0 && digits && i + 1 < number.size) {
                scale = 0.1;
            } else {
                return false;
            }
        }
        if (!digits)
            return false;
        out[count++] = float(value);
        if (end >= text.size)
            break;
    }
    return count == 4 && stringFind(text, ',') < text.size;
}

// Applies the spatial dimension of a media fragment, `xywh=` with its optional unit (Media Fragments URI 1.0, 4.2.2): a
// region in pixels, or in percentages of the image, which is the view's viewBox or else its natural size. The region is
// clamped to the image, and the last valid dimension applies. A region outside the image, or of no area, applies
// nothing.
void applyMediaFragment(PrStringView fragment, PrRect image, bool hasImage, SvgRootView* view) {
    for (size_t start = 0; start <= fragment.size;) {
        size_t end = start + stringFind(stringSlice(fragment, start, fragment.size - start), '&');
        PrStringView pair = stringSlice(fragment, start, end - start);
        start = end + 1;
        if (pair.size < 5 || stringSlice(pair, 0, 5) != "xywh=")
            continue;
        PrStringView value = stringSlice(pair, 5, pair.size - 5);
        bool percent = false;
        if (value.size >= 6 && stringSlice(value, 0, 6) == "pixel:") {
            value = stringSlice(value, 6, value.size - 6);
        } else if (value.size >= 8 && stringSlice(value, 0, 8) == "percent:") {
            value = stringSlice(value, 8, value.size - 8);
            percent = true;
        }
        float n[4];
        if (!parseCoordinates(value, percent, n) || !(n[2] > 0) || !(n[3] > 0) || (percent && !hasImage))
            continue;
        PrRect region = PrRect{n[0], n[1], n[0] + n[2], n[1] + n[3]};
        if (percent) {
            float width = prRectWidth(image) / 100;
            float height = prRectHeight(image) / 100;
            region = PrRect{image.left + n[0] * width, image.top + n[1] * height, image.left + (n[0] + n[2]) * width,
                            image.top + (n[1] + n[3]) * height};
        }
        if (hasImage)
            region = PrRect{maxF32(region.left, image.left), maxF32(region.top, image.top),
                            minF32(region.right, image.right), minF32(region.bottom, image.bottom)};
        if (!(region.right > region.left) || !(region.bottom > region.top))
            continue;
        view->viewBox = region;
        view->hasViewBox = true;
    }
}

// Returns the end of the call to svgView() at the start of `text`, after its closing parenthesis, or zero if `text`
// does not start with a whole call.
size_t svgViewEnd(PrStringView text) {
    PrStringView name = "svgView(";
    if (text.size < name.size || stringSlice(text, 0, name.size) != name)
        return 0;
    uint32_t depth = 0;
    for (size_t i = name.size - 1; i < text.size; ++i) {
        if (text.data[i] == '(')
            ++depth;
        else if (text.data[i] == ')' && --depth == 0)
            return i + 1;
    }
    return 0;
}

void imageDraw(const SvgImage* image, PrVec2 extent, PrDrawList* list) {
    if (image->background.a > 0)
        prDrawListRect(list, PrRect{0, 0, extent.x, extent.y}, {.color = image->background});
    SvgDrawHost host{.view = &image->view};
    if (image->scaledFrom.x <= 0 || image->scaledFrom.y <= 0) {
        svgTreeDraw(&image->tree, extent, &host, list);
        return;
    }
    PrVec2 scale{extent.x / image->scaledFrom.x, extent.y / image->scaledFrom.y};
    prDrawListPushTransformAffine2(list, prAffine2Scale(scale));
    svgTreeDraw(&image->tree, image->scaledFrom, &host, list);
    prDrawListPopTransform(list);
}

// The functions of an image's painter, whose context is the image.
void painterDraw(void* PR_NULLABLE context, PrVec2 extent, PrDrawList* list) {
    imageDraw(static_cast<const SvgImage*>(context), extent, list);
}

void painterRetain(void* PR_NULLABLE context) {
    svgImageRetain(static_cast<const SvgImage*>(context));
}

void painterRelease(void* PR_NULLABLE context) {
    svgImageRelease(static_cast<const SvgImage*>(context));
}

} // namespace

const PrDomElement* PR_NULLABLE svgImageView(PrStringView fragment, const PrDomElement* root, SvgRootView* view) {
    if (fragment.size == 0)
        return nullptr;
    TextBuffer decoded;
    decoded.text.reserve(uint32_t(fragment.size));
    size_t size = prUrlPercentDecode(fragment, decoded.text.data, fragment.size);
    PrStringView text{decoded.text.data, size};
    const PrDomElement* target = nullptr;
    size_t call = svgViewEnd(text);
    if (call != 0) {
        // An SVG view may be followed only by a media fragment's temporal dimension, which changes nothing that an
        // image draws. Any other fragment that starts with one is in error, and names nothing.
        PrStringView rest = stringSlice(text, call, text.size - call);
        if (rest.size == 0 || (rest.size >= 3 && stringSlice(rest, 0, 3) == "&t="))
            applySvgView(argumentsOf(stringSlice(text, 0, call), "svgView"), view);
    } else if (stringFind(text, '=') < text.size) {
        PrSvgNaturalSize natural = prSvgElementNaturalSize(static_cast<const PrSvgElement*>(root));
        bool sized = natural.width > 0 && natural.height > 0;
        PrRect image = view->hasViewBox ? view->viewBox : PrRect{0, 0, natural.width, natural.height};
        applyMediaFragment(text, image, view->hasViewBox || sized, view);
    } else {
        target = svgFindById(root->document, text);
        if (target != nullptr && svgIs(target, PrDomTag_View)) {
            PrRect box{};
            if (svgParseViewBox(svgAttribute(target, "viewBox"), &box)) {
                view->viewBox = box;
                view->hasViewBox = true;
            }
            SvgAspect aspect;
            if (svgParseAspect(svgAttribute(target, "preserveAspectRatio"), &aspect))
                view->aspect = aspect;
        }
    }
    textReset(&decoded);
    return target;
}

void svgImageDraw(const SvgImage* image, PrVec2 extent, PrDrawList* list) {
    imageDraw(image, extent, list);
}

void svgImageDrawRendering(const SvgImage* image, PrRect area, PrDrawList* list) {
    SvgRootView view = image->view;
    view.aspect = SvgAspect{.x = SvgAlign_None, .y = SvgAlign_None};
    view.clips = !image->overflows;
    if (!view.hasViewBox && image->natural.width > 0 && image->natural.height > 0) {
        view.viewBox = PrRect{0, 0, image->natural.width, image->natural.height};
        view.hasViewBox = true;
    }
    PrVec2 extent{prRectWidth(area), prRectHeight(area)};
    prDrawListPushTransformAffine2(list, prAffine2Translate(PrVec2{area.left, area.top}));
    if (image->background.a > 0)
        prDrawListRect(list, PrRect{0, 0, extent.x, extent.y}, {.color = image->background});
    SvgDrawHost host{.view = &view};
    svgTreeDraw(&image->tree, extent, &host, list);
    prDrawListPopTransform(list);
}

void svgImageRetain(const SvgImage* image) {
    image->references.fetchAddRelaxed(1);
}

void svgImageRelease(const SvgImage* PR_NULLABLE image) {
    if (image == nullptr || image->references.fetchSub(1) != 1)
        return;
    SvgImage* own = const_cast<SvgImage*>(image);
    svgTreeReset(&own->tree);
    prMemoryFree(own);
}

PrLayoutPainter svgImagePainter(const SvgImage* image) {
    return PrLayoutPainter{.paint = painterDraw,
                           .context = const_cast<SvgImage*>(image),
                           .retain = painterRetain,
                           .release = painterRelease};
}

const SvgImage* PR_NULLABLE svgImageOfPainter(PrLayoutPainter painter) {
    return painter.paint == painterDraw ? static_cast<const SvgImage*>(painter.context) : nullptr;
}

} // namespace pr_impl
