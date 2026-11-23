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
    if (text.size < name.size + 2 || prStringSlice(text, 0, name.size) != name || text.data[name.size] != '(' ||
            text.data[text.size - 1] != ')')
        return PrStringView{};
    return prStringSlice(text, name.size + 1, text.size - name.size - 2);
}

// Applies an SVG view specification, the arguments of svgView() (SVG2/linking.html#SVGFragmentIdentifiers): its
// viewBox(), preserveAspectRatio() and transform(), separated by semicolons. Its zoomAndPan() changes nothing that an
// image draws.
void applySvgView(PrStringView specification, SvgRootView* view) {
    for (size_t start = 0; start <= specification.size;) {
        size_t end = start + prStringFindByte(prStringFrom(specification, start), ';');
        PrStringView part = prStringTrim(prStringSlice(specification, start, end - start));
        PrStringView viewBox = argumentsOf(part, "viewBox");
        PrStringView aspect = argumentsOf(part, "preserveAspectRatio");
        PrStringView transform = argumentsOf(part, "transform");
        PrRect box{};
        PrSvgPreserveAspectRatio parsed = kSvgAspectDefault;
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
        size_t end = start + prStringFindByte(prStringFrom(text, start), ',');
        PrStringView number = prStringSlice(text, start, end - start);
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
    return count == 4 && prStringFindByte(text, ',') < text.size;
}

// Applies the spatial dimension of a media fragment, `xywh=` with its optional unit (Media Fragments URI 1.0, 4.2.2): a
// region in pixels, or in percentages of the image, which is the view's viewBox or else its natural size. The region is
// clamped to the image, and the last valid dimension applies. A region outside the image, or of no area, applies
// nothing.
void applyMediaFragment(PrStringView fragment, PrRect image, bool hasImage, SvgRootView* view) {
    for (size_t start = 0; start <= fragment.size;) {
        size_t end = start + prStringFindByte(prStringFrom(fragment, start), '&');
        PrStringView pair = prStringSlice(fragment, start, end - start);
        start = end + 1;
        if (pair.size < 5 || prStringSlice(pair, 0, 5) != "xywh=")
            continue;
        PrStringView value = prStringFrom(pair, 5);
        bool percent = false;
        if (value.size >= 6 && prStringSlice(value, 0, 6) == "pixel:") {
            value = prStringFrom(value, 6);
        } else if (value.size >= 8 && prStringSlice(value, 0, 8) == "percent:") {
            value = prStringFrom(value, 8);
            percent = true;
        }
        float n[4];
        if (!parseCoordinates(value, percent, n) || !(n[2] > 0) || !(n[3] > 0) || (percent && !hasImage))
            continue;
        PrRect region = PrRect{n[0], n[1], n[0] + n[2], n[1] + n[3]};
        if (percent) {
            float width = prRectWidth(image) / 100;
            float height = prRectHeight(image) / 100;
            region = PrRect{
                image.left + n[0] * width, image.top + n[1] * height, image.left + (n[0] + n[2]) * width,
                image.top + (n[1] + n[3]) * height,
            };
        }
        if (hasImage)
            region = PrRect{
                prF32Max(region.left, image.left), prF32Max(region.top, image.top),
                prF32Min(region.right, image.right), prF32Min(region.bottom, image.bottom),
            };
        if (!(region.right > region.left) || !(region.bottom > region.top))
            continue;
        view->viewBox = region;
        view->hasViewBox = true;
    }
}

// Parses a time of a media fragment's temporal dimension in normal play time (Media Fragments URI 1.0, 4.2.1): seconds
// with an optional fraction, or minutes and seconds, or hours, minutes and seconds, separated by colons, whose minutes
// and seconds are two digits below 60. Returns false if `text` is not one.
bool parseNptTime(PrStringView text, double* out) {
    double parts[3] = {};
    uint32_t count = 0;
    double fraction = 0;
    double scale = 0;
    uint32_t digits = 0;
    uint32_t firstDigits = 0;
    for (size_t i = 0; i <= text.size; ++i) {
        char c = i < text.size ? text.data[i] : ':';
        if (c >= '0' && c <= '9' && scale == 0) {
            parts[count] = parts[count] * 10 + (c - '0');
            ++digits;
        } else if (c >= '0' && c <= '9') {
            fraction += (c - '0') * scale;
            scale /= 10;
        } else if (c == '.' && scale == 0 && digits != 0) {
            scale = 0.1;
        } else if (c == ':' && digits != 0 && (scale == 0 || i == text.size)) {
            // A field after the first is two digits below 60.
            if (count != 0 && (digits != 2 || parts[count] >= 60))
                return false;
            if (count == 0)
                firstDigits = digits;
            if (++count == 3 && i < text.size)
                return false;
            digits = 0;
        } else {
            return false;
        }
    }
    // Seconds alone are any number of digits, and so are hours; the minutes of minutes and seconds are two.
    if (count == 2 && (firstDigits != 2 || parts[0] >= 60))
        return false;
    double seconds = 0;
    for (uint32_t i = 0; i < count; ++i)
        seconds = seconds * 60 + parts[i];
    *out = seconds + fraction;
    return true;
}

// Writes the start and the end of a media fragment's temporal dimension, `t=` in normal play time with its optional
// `npt:` prefix, as in `t=npt:3,3.01` or `t=,5` (Media Fragments URI 1.0, 4.2.1), in seconds: zero and infinity if
// `text` gives none. The last valid dimension applies. A start that is not before the end is not valid.
void mediaFragmentRange(PrStringView text, SvgImageTimes* out) {
    *out = SvgImageTimes{0, kPrF64Infinity};
    for (size_t at = 0; at <= text.size;) {
        size_t end = at + prStringFindByte(prStringFrom(text, at), '&');
        PrStringView pair = prStringSlice(text, at, end - at);
        at = end + 1;
        if (pair.size < 2 || prStringSlice(pair, 0, 2) != "t=")
            continue;
        PrStringView value = prStringFrom(pair, 2);
        if (value.size >= 4 && prStringSlice(value, 0, 4) == "npt:")
            value = prStringFrom(value, 4);
        size_t comma = prStringFindByte(value, ',');
        PrStringView first = prStringSlice(value, 0, comma);
        PrStringView last = comma < value.size ? prStringFrom(value, comma + 1) : PrStringView{};
        double from = 0;
        double to = kPrF64Infinity;
        if (first.size != 0 && !parseNptTime(first, &from))
            continue;
        if (comma < value.size && !parseNptTime(last, &to))
            continue;
        if ((first.size == 0 && comma >= value.size) || from >= to)
            continue;
        *out = SvgImageTimes{from, to};
    }
}

// Sets the attribute of `element` named `name` to the numbers of `values`, separated by spaces, as `prefix(...)` if
// `prefix` is given.
void setNumbers(PrDomElement* element, PrStringView name, const float* values, uint32_t count, PrStringView prefix = {}) {
    PrTextBuffer text = {};
    if (prefix.size != 0) {
        prTextBufferAppend(&text, prefix);
        prTextBufferAppendChar(&text, '(');
    }
    for (uint32_t i = 0; i < count; ++i) {
        char number[kPrF64TextMaxBytes];
        if (i != 0)
            prTextBufferAppendChar(&text, ' ');
        prTextBufferAppend(&text, PrStringView{number, prF32Format(values[i], number)});
    }
    if (prefix.size != 0)
        prTextBufferAppendChar(&text, ')');
    prDomElementSetAttribute(element, name, PrStringView{text.data, text.size});
    prTextBufferReset(&text);
}

// Returns the end of the call to svgView() at the start of `text`, after its closing parenthesis, or zero if `text`
// does not start with a whole call.
size_t svgViewEnd(PrStringView text) {
    PrStringView name = "svgView(";
    if (text.size < name.size || prStringSlice(text, 0, name.size) != name)
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
    // A root with a width and a height but no viewBox takes them as its viewBox (w3c/svgwg#249), which stretches them
    // to the extent. The root's transform then applies in the extent's space, as it does with a viewBox of its own.
    SvgRootView view = image->view;
    if (image->synthesizedViewBox.x > 0 && image->synthesizedViewBox.y > 0) {
        view.viewBox = PrRect{0, 0, image->synthesizedViewBox.x, image->synthesizedViewBox.y};
        view.hasViewBox = true;
        view.aspect = PrSvgPreserveAspectRatio{PrSvgAlign_None, PrSvgMeetOrSlice_Meet};
    }
    SvgDrawHost host{.view = &view};
    svgTreeDraw(&image->tree, extent, &host, list);
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
    PrTextBuffer decoded = {};
    prTextBufferReserve(&decoded, uint32_t(fragment.size));
    size_t size = prUrlPercentDecode(fragment, decoded.data, fragment.size);
    PrStringView text{decoded.data, size};
    const PrDomElement* target = nullptr;
    size_t call = svgViewEnd(text);
    if (call != 0) {
        // An SVG view may be followed only by a media fragment's temporal dimension, which changes nothing that an
        // image draws. Any other fragment that starts with one is in error, and names nothing.
        PrStringView rest = prStringFrom(text, call);
        if (rest.size == 0 || (rest.size >= 3 && prStringSlice(rest, 0, 3) == "&t="))
            applySvgView(argumentsOf(prStringSlice(text, 0, call), "svgView"), view);
    } else if (prStringFindByte(text, '=') < text.size) {
        PrSvgNaturalSize natural = prSvgElementNaturalSize(static_cast<const PrSvgElement*>(root));
        bool sized = natural.width > 0 && natural.height > 0;
        PrRect image = view->hasViewBox ? view->viewBox : PrRect{0, 0, natural.width, natural.height};
        applyMediaFragment(text, image, view->hasViewBox || sized, view);
    } else {
        target = svgFindById(root->document, text);
        if (target != nullptr && svgIs(target, PrDomTag_View)) {
            PrRect box{};
            if (svgAttributeViewBox(target, &box)) {
                view->viewBox = box;
                view->hasViewBox = true;
            }
            PrSvgPreserveAspectRatio aspect = kSvgAspectDefault;
            if (svgParseAspect(svgAttribute(target, "preserveAspectRatio"), &aspect))
                view->aspect = aspect;
        }
    }
    prTextBufferReset(&decoded);
    return target;
}

SvgImageTimes svgImageTimes(PrStringView fragment) {
    SvgImageTimes times{0, kPrF64Infinity};
    if (fragment.size == 0)
        return times;
    PrTextBuffer decoded = {};
    prTextBufferReserve(&decoded, uint32_t(fragment.size));
    size_t size = prUrlPercentDecode(fragment, decoded.data, fragment.size);
    mediaFragmentRange(PrStringView{decoded.data, size}, &times);
    prTextBufferReset(&decoded);
    return times;
}

SvgImageFit svgImageApplyView(PrDomElement* root, PrStringView fragment, bool rendering) {
    SvgRootView view{};
    view.hasViewBox = svgAttributeViewBox(root, &view.viewBox);
    svgParseAspect(svgAttribute(root, "preserveAspectRatio"), &view.aspect);
    // The natural size is the root's own, whatever view the fragment names.
    PrSvgNaturalSize natural = prSvgElementNaturalSize(static_cast<const PrSvgElement*>(root));
    svgImageView(fragment, root, &view);
    if (rendering)
        view.aspect = PrSvgPreserveAspectRatio{PrSvgAlign_None, PrSvgMeetOrSlice_Meet};
    bool sized = natural.width > 0 && natural.height > 0;
    PrVec2 target = sized ? PrVec2{natural.width, natural.height} : kSvgImageDefaultSize;
    SvgImageFit fit{target, view.aspect, true};
    bool boxed = view.hasViewBox && prRectWidth(view.viewBox) > 0 && prRectHeight(view.viewBox) > 0;
    if (boxed) {
        // The root fills a viewport of its viewBox's ratio, as large as fits the natural size, which the painter fits
        // into the size the image is shown at as the viewBox would be.
        PrVec2 box{prRectWidth(view.viewBox), prRectHeight(view.viewBox)};
        float scale = prF32Min(target.x / box.x, target.y / box.y);
        fit = SvgImageFit{PrVec2{box.x * scale, box.y * scale}, view.aspect, false};
        float numbers[] = {view.viewBox.left, view.viewBox.top, box.x, box.y};
        setNumbers(root, "viewBox", numbers, 4);
        prDomElementSetAttribute(root, "width", "100%");
        prDomElementSetAttribute(root, "height", "100%");
    } else if (rendering && sized) {
        // A root with a width and a height but no viewBox takes them as its viewBox (w3c/svgwg#249).
        float numbers[] = {0, 0, natural.width, natural.height};
        setNumbers(root, "viewBox", numbers, 4);
    }
    // The painter places the drawing as the view's preserveAspectRatio says, so the root's own fills its viewport.
    prDomElementSetAttribute(root, "preserveAspectRatio", "none");
    if (view.hasTransform) {
        const PrAffine2& m = view.transform;
        float numbers[] = {m.a, m.b, m.c, m.d, m.e, m.f};
        setNumbers(root, "transform", numbers, 6, "matrix");
    }
    return fit;
}

PrAffine2 svgImageFitTransform(const SvgImageFit& fit, PrVec2 extent) {
    if (fit.layout.x <= 0 || fit.layout.y <= 0)
        return prAffine2Identity();
    if (fit.stretch || fit.aspect.align <= PrSvgAlign_None)
        return PrAffine2{extent.x / fit.layout.x, 0, 0, extent.y / fit.layout.y, 0, 0};
    return svgViewBoxTransform(PrRect{0, 0, fit.layout.x, fit.layout.y}, fit.aspect, extent.x, extent.y);
}

void svgImageDraw(const SvgImage* image, PrVec2 extent, PrDrawList* list) {
    imageDraw(image, extent, list);
}

void svgImageDrawRendering(const SvgImage* image, PrRect area, PrDrawList* list) {
    SvgRootView view = image->view;
    view.aspect = PrSvgPreserveAspectRatio{PrSvgAlign_None, PrSvgMeetOrSlice_Meet};
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
    return PrLayoutPainter{
        .context = const_cast<SvgImage*>(image),
        .paint = painterDraw,
        .retain = painterRetain,
        .release = painterRelease,
    };
}

const SvgImage* PR_NULLABLE svgImageOfPainter(PrLayoutPainter painter) {
    return painter.paint == painterDraw ? static_cast<const SvgImage*>(painter.context) : nullptr;
}

} // namespace pr_impl
